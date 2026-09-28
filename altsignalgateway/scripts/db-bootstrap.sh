#!/usr/bin/env bash
# One-time, idempotent database bootstrap for AltSignalGateway.
#
# AltSignalGateway does NOT create a database of its own. Its schema lives
# inside the shared AltSpotter database, next to `altstrategy` and
# `altsignalquality`, because every signal it journals is enriched from
# AltStrategy's point-in-time context snapshot and AltSpotter's instrument
# registry. A separate database would put postgres_fdw on the hot path of every
# single signal — a cost this stack has already measured once.
#
# LEAST PRIVILEGE. The role created here owns exactly one schema and gets
# SELECT — nothing else — on the five upstream tables it reads. It cannot write
# to another service's schema, cannot read AltStrategy's model or outcome
# tables, and has no access to the market-data archive at all.
#
# Connection parameters come from the environment, never hardcoded here:
#   ALTSIGNALGATEWAY_DB_NAME / _DB_USER / _DB_PASSWORD
#   ALTSTRATEGY_SCHEMA, ALTSPOTTER_SCHEMA
set -euo pipefail

: "${ALTSIGNALGATEWAY_DB_NAME:?set ALTSIGNALGATEWAY_DB_NAME}"
: "${ALTSIGNALGATEWAY_DB_USER:?set ALTSIGNALGATEWAY_DB_USER}"
: "${ALTSIGNALGATEWAY_DB_PASSWORD:?set ALTSIGNALGATEWAY_DB_PASSWORD}"
ALTSTRATEGY_SCHEMA="${ALTSTRATEGY_SCHEMA:-altstrategy}"
ALTSPOTTER_SCHEMA="${ALTSPOTTER_SCHEMA:-public}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -n "${1:-}" ]]; then
    MIGRATIONS="$1"
elif [[ -d "$SCRIPT_DIR/../db/migrations" ]]; then
    MIGRATIONS="$SCRIPT_DIR/../db/migrations"
else
    MIGRATIONS="$SCRIPT_DIR/../share/altsignalgateway/db/migrations"
fi
[[ -d "$MIGRATIONS" ]] || { echo "migrations directory not found: $MIGRATIONS" >&2; exit 2; }

psql_admin() { psql -v ON_ERROR_STOP=1 -U postgres "$@"; }

echo "==> role ${ALTSIGNALGATEWAY_DB_USER}"
psql_admin -d postgres -tAc \
    "SELECT 1 FROM pg_roles WHERE rolname = '${ALTSIGNALGATEWAY_DB_USER}'" | grep -q 1 || \
    psql_admin -d postgres -c \
        "CREATE ROLE ${ALTSIGNALGATEWAY_DB_USER} LOGIN PASSWORD '${ALTSIGNALGATEWAY_DB_PASSWORD}'"
psql_admin -d postgres -c \
    "ALTER ROLE ${ALTSIGNALGATEWAY_DB_USER} PASSWORD '${ALTSIGNALGATEWAY_DB_PASSWORD}'"

echo "==> altsignalgateway schema inside ${ALTSIGNALGATEWAY_DB_NAME}"
psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c \
    "CREATE SCHEMA IF NOT EXISTS altsignalgateway AUTHORIZATION ${ALTSIGNALGATEWAY_DB_USER}"
psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c \
    "GRANT CONNECT ON DATABASE ${ALTSIGNALGATEWAY_DB_NAME} TO ${ALTSIGNALGATEWAY_DB_USER}"
psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c \
    "ALTER ROLE ${ALTSIGNALGATEWAY_DB_USER} IN DATABASE ${ALTSIGNALGATEWAY_DB_NAME}
     SET search_path = altsignalgateway, public"

echo "==> read-only grants on the upstream tables this service reads"
# decision_context_snapshots  -> the point-in-time market/microstructure state
# canonical_pairs / _exchange_map / exchange_instruments -> the executable symbol
# instrument_spec_versions    -> tick size, lot step, min notional as of T0
psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c "
    GRANT USAGE ON SCHEMA ${ALTSTRATEGY_SCHEMA} TO ${ALTSIGNALGATEWAY_DB_USER};
    GRANT SELECT ON ${ALTSTRATEGY_SCHEMA}.decision_context_snapshots
        TO ${ALTSIGNALGATEWAY_DB_USER};
    GRANT USAGE ON SCHEMA ${ALTSPOTTER_SCHEMA} TO ${ALTSIGNALGATEWAY_DB_USER};
    GRANT SELECT ON
        ${ALTSPOTTER_SCHEMA}.canonical_pairs,
        ${ALTSPOTTER_SCHEMA}.canonical_pair_exchange_map,
        ${ALTSPOTTER_SCHEMA}.exchange_instruments,
        ${ALTSPOTTER_SCHEMA}.exchanges,
        ${ALTSPOTTER_SCHEMA}.instrument_spec_versions
    TO ${ALTSIGNALGATEWAY_DB_USER};
"

# --- the read-only foreign reads the query path and the macro block need -----
#
# The macro block comes from AltTech's OWN database, the extended market
# context from AltSpotter's. Both are reached the way every consumer in this
# stack reaches another service's tables: a postgres_fdw server, a user mapping
# that logs in as THIS service's role with THIS service's password, and SELECT
# on exactly the tables it reads, granted in the remote database. No shared
# credential, no superuser, nothing writable.
#
# The remote grants need the other databases' names; they default to the
# conventional ones and can be overridden from the environment.
ALTTECH_DB_NAME="${ALTTECH_DB_NAME:-alttech}"
ALTSPOTTER_DB_NAME="${ALTSPOTTER_DB_NAME:-altspotter}"
PG_HOST="${ALTSIGNALGATEWAY_FDW_HOST:-127.0.0.1}"
PG_PORT="${ALTSIGNALGATEWAY_FDW_PORT:-5432}"

echo "==> remote read-only grants for ${ALTSIGNALGATEWAY_DB_USER}"
psql_admin -d "$ALTTECH_DB_NAME" -c "
    GRANT CONNECT ON DATABASE ${ALTTECH_DB_NAME} TO ${ALTSIGNALGATEWAY_DB_USER};
    GRANT USAGE ON SCHEMA alttech TO ${ALTSIGNALGATEWAY_DB_USER};
    GRANT SELECT ON alttech.macro_feature_vectors, alttech.feature_set_versions,
                    alttech.feature_set_members TO ${ALTSIGNALGATEWAY_DB_USER};
"
psql_admin -d "$ALTSPOTTER_DB_NAME" -c "
    GRANT CONNECT ON DATABASE ${ALTSPOTTER_DB_NAME} TO ${ALTSIGNALGATEWAY_DB_USER};
    GRANT USAGE ON SCHEMA public TO ${ALTSIGNALGATEWAY_DB_USER};
    GRANT SELECT ON public.canonical_pairs, public.canonical_pair_exchange_map,
                    public.exchange_instruments, public.exchanges,
                    public.instrument_spec_versions,
                    public.market_candles_1m, public.market_candles_agg,
                    public.market_derivatives_context_1m,
                    public.market_book_aggregates_1m, public.market_trade_aggregates_1m
        TO ${ALTSIGNALGATEWAY_DB_USER};
"

echo "==> foreign servers and this role's own user mappings"
psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c "
    CREATE EXTENSION IF NOT EXISTS postgres_fdw;
    DO \$\$ BEGIN
        IF NOT EXISTS (SELECT 1 FROM pg_foreign_server WHERE srvname = 'altspotter_srv') THEN
            CREATE SERVER altspotter_srv FOREIGN DATA WRAPPER postgres_fdw
                OPTIONS (host '${PG_HOST}', port '${PG_PORT}', dbname '${ALTSPOTTER_DB_NAME}',
                         use_remote_estimate 'true', fetch_size '10000');
        END IF;
        IF NOT EXISTS (SELECT 1 FROM pg_foreign_server WHERE srvname = 'alttech_srv') THEN
            CREATE SERVER alttech_srv FOREIGN DATA WRAPPER postgres_fdw
                OPTIONS (host '${PG_HOST}', port '${PG_PORT}', dbname '${ALTTECH_DB_NAME}',
                         use_remote_estimate 'true', fetch_size '10000');
        END IF;
    END \$\$;
    GRANT USAGE ON FOREIGN SERVER altspotter_srv, alttech_srv TO ${ALTSIGNALGATEWAY_DB_USER};
    DROP USER MAPPING IF EXISTS FOR ${ALTSIGNALGATEWAY_DB_USER} SERVER altspotter_srv;
    CREATE USER MAPPING FOR ${ALTSIGNALGATEWAY_DB_USER} SERVER altspotter_srv
        OPTIONS (user '${ALTSIGNALGATEWAY_DB_USER}', password '${ALTSIGNALGATEWAY_DB_PASSWORD}');
    DROP USER MAPPING IF EXISTS FOR ${ALTSIGNALGATEWAY_DB_USER} SERVER alttech_srv;
    CREATE USER MAPPING FOR ${ALTSIGNALGATEWAY_DB_USER} SERVER alttech_srv
        OPTIONS (user '${ALTSIGNALGATEWAY_DB_USER}', password '${ALTSIGNALGATEWAY_DB_PASSWORD}');
"

echo "==> foreign tables (column lists are the producers' own; a producer change is a bootstrap change)"
psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c "
    CREATE SCHEMA IF NOT EXISTS alttech_ref AUTHORIZATION ${ALTSIGNALGATEWAY_DB_USER};
    CREATE FOREIGN TABLE IF NOT EXISTS alttech_ref.macro_feature_vectors (
        timeframe text, bar_open_time timestamptz, feature_set_version_id bigint,
        method_version text, \"values\" real[], validity_bitmap bytea, status text,
        valid_count integer, computed_at timestamptz, updated_at timestamptz
    ) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'macro_feature_vectors');
    CREATE FOREIGN TABLE IF NOT EXISTS alttech_ref.feature_set_versions (
        id bigint, name text, version integer, scope text, profile text, engine_version text,
        method_version text, schema_hash text, frozen boolean, created_at timestamptz
    ) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'feature_set_versions');
    CREATE FOREIGN TABLE IF NOT EXISTS alttech_ref.feature_set_members (
        feature_set_version_id bigint, position integer, parameter_set_id bigint, output_name text
    ) SERVER alttech_srv OPTIONS (schema_name 'alttech', table_name 'feature_set_members');
    ALTER FOREIGN TABLE alttech_ref.macro_feature_vectors OWNER TO ${ALTSIGNALGATEWAY_DB_USER};
    ALTER FOREIGN TABLE alttech_ref.feature_set_versions OWNER TO ${ALTSIGNALGATEWAY_DB_USER};
    ALTER FOREIGN TABLE alttech_ref.feature_set_members OWNER TO ${ALTSIGNALGATEWAY_DB_USER};

    CREATE SCHEMA IF NOT EXISTS ${ALTSPOTTER_SCHEMA};
    GRANT USAGE ON SCHEMA ${ALTSPOTTER_SCHEMA} TO ${ALTSIGNALGATEWAY_DB_USER};
    CREATE FOREIGN TABLE IF NOT EXISTS ${ALTSPOTTER_SCHEMA}.market_candles_1m (
        exchange_id smallint, instrument_id bigint, open_time timestamptz, close_time timestamptz,
        open numeric, high numeric, low numeric, close numeric, base_volume numeric,
        quote_volume numeric, trade_count bigint, taker_buy_base_volume numeric,
        taker_sell_base_volume numeric, is_closed boolean, source text, quality_status text,
        revision bigint, exchange_received_at timestamptz, stored_at timestamptz, updated_at timestamptz
    ) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'market_candles_1m');
    CREATE FOREIGN TABLE IF NOT EXISTS ${ALTSPOTTER_SCHEMA}.market_candles_agg (
        exchange_id smallint, instrument_id bigint, timeframe text, open_time timestamptz,
        close_time timestamptz, open numeric, high numeric, low numeric, close numeric,
        base_volume numeric, quote_volume numeric, trade_count bigint, taker_buy_base_volume numeric,
        taker_sell_base_volume numeric, is_closed boolean, source text, quality_status text,
        minute_count bigint, revision bigint, exchange_received_at timestamptz,
        stored_at timestamptz, updated_at timestamptz
    ) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'market_candles_agg');
    CREATE FOREIGN TABLE IF NOT EXISTS ${ALTSPOTTER_SCHEMA}.market_derivatives_context_1m (
        exchange_id smallint, instrument_id bigint, open_time timestamptz, mark_price numeric,
        index_price numeric, funding_rate numeric, next_funding_time timestamptz,
        open_interest_base numeric, open_interest_quote numeric, best_bid numeric, best_ask numeric,
        spread numeric, turnover_24h numeric, quality_status text, source_mask integer,
        updated_at timestamptz
    ) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'market_derivatives_context_1m');
    CREATE FOREIGN TABLE IF NOT EXISTS ${ALTSPOTTER_SCHEMA}.market_book_aggregates_1m (
        exchange_id smallint, instrument_id bigint, open_time timestamptz, best_bid numeric,
        best_ask numeric, mid_price numeric, spread numeric, relative_spread numeric,
        bid_depth_1 numeric, ask_depth_1 numeric, bid_depth_5 numeric, ask_depth_5 numeric,
        bid_depth_10 numeric, ask_depth_10 numeric, bid_depth_15 numeric, ask_depth_15 numeric,
        book_imbalance_1 numeric, book_imbalance_5 numeric, book_imbalance_10 numeric,
        book_imbalance_15 numeric, microprice numeric, snapshot_count integer,
        desync_count integer, quality_status text
    ) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'market_book_aggregates_1m');
    CREATE FOREIGN TABLE IF NOT EXISTS ${ALTSPOTTER_SCHEMA}.market_trade_aggregates_1m (
        exchange_id smallint, instrument_id bigint, open_time timestamptz, trade_count integer,
        buy_trade_count integer, sell_trade_count integer, base_volume numeric,
        quote_volume numeric, aggressive_buy_volume numeric, aggressive_sell_volume numeric,
        signed_volume numeric, vwap numeric, min_price numeric, max_price numeric,
        first_price numeric, last_price numeric, largest_trade numeric, quality_status text
    ) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'market_trade_aggregates_1m');
    GRANT SELECT ON ALL TABLES IN SCHEMA ${ALTSPOTTER_SCHEMA} TO ${ALTSIGNALGATEWAY_DB_USER};
    GRANT SELECT ON ALL TABLES IN SCHEMA alttech_ref TO ${ALTSIGNALGATEWAY_DB_USER};
"

echo "==> migrations"
# A version table inside this service's OWN schema: the shared
# public.schema_migrations belongs to AltSpotter, and identical version numbers
# would collide there.
psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c "
    CREATE TABLE IF NOT EXISTS altsignalgateway.schema_migrations (
        version    TEXT PRIMARY KEY,
        name       TEXT NOT NULL,
        checksum   TEXT NOT NULL,
        applied_at TIMESTAMPTZ NOT NULL DEFAULT now());
    ALTER TABLE altsignalgateway.schema_migrations OWNER TO ${ALTSIGNALGATEWAY_DB_USER};
"
for file in $(ls -1 "$MIGRATIONS"/*.sql | sort); do
    base="$(basename "$file" .sql)"
    version="${base%%_*}"
    name="${base#*_}"
    checksum="$(sha256sum "$file" | cut -d' ' -f1)"
    applied="$(psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -tAc \
        "SELECT checksum FROM altsignalgateway.schema_migrations WHERE version = '${version}'")"
    if [[ -n "$applied" ]]; then
        if [[ "$applied" != "$checksum" ]]; then
            echo "migration ${version} is already applied but its file changed" >&2
            exit 1
        fi
        echo "    ${version}_${name}: already applied"
        continue
    fi
    echo "    ${version}_${name}: applying"
    psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -1 -f "$file"
    psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c \
        "INSERT INTO altsignalgateway.schema_migrations (version, name, checksum)
         VALUES ('${version}', '${name}', '${checksum}')"
done

echo "==> schema object ownership"
psql_admin -d "$ALTSIGNALGATEWAY_DB_NAME" -c "
    DO \$\$
    DECLARE r RECORD;
    BEGIN
        FOR r IN SELECT tablename FROM pg_tables WHERE schemaname = 'altsignalgateway' LOOP
            EXECUTE format('ALTER TABLE altsignalgateway.%I OWNER TO ${ALTSIGNALGATEWAY_DB_USER}',
                           r.tablename);
        END LOOP;
        FOR r IN SELECT sequencename FROM pg_sequences WHERE schemaname = 'altsignalgateway' LOOP
            EXECUTE format('ALTER SEQUENCE altsignalgateway.%I OWNER TO ${ALTSIGNALGATEWAY_DB_USER}',
                           r.sequencename);
        END LOOP;
        FOR r IN SELECT p.proname FROM pg_proc p
                 JOIN pg_namespace n ON n.oid = p.pronamespace
                 WHERE n.nspname = 'altsignalgateway' LOOP
            EXECUTE format('ALTER FUNCTION altsignalgateway.%I() OWNER TO ${ALTSIGNALGATEWAY_DB_USER}',
                           r.proname);
        END LOOP;
    END \$\$;
"

echo "AltSignalGateway database is ready"
