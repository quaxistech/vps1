-- AltSpotter tables AltStrategy reads, as postgres_fdw foreign tables in the
-- `altstrategy` database. Not a migration: altstrategy-db-migrate owns only the
-- `altstrategy` schema. Applied by an operator, as altstrategy_app (the owner of
-- the existing foreign tables), after approval:
--
--   psql -X -v ON_ERROR_STOP=1 -d altstrategy -f db/fdw/altspotter_reference.sql
--
-- Prerequisites, as they stand on the production host: the postgres_fdw
-- extension, the server `altspotter_srv` (dbname=altspotter), a user mapping
-- for altstrategy_app, and SELECT for that role on the remote tables. The
-- remote role has no write privilege on them, so these tables are read-only
-- whatever the local grants say.
--
-- The first two have existed in production since deployment without being
-- written down anywhere in this repository; they are recorded here exactly as
-- the production catalog defines them. IF NOT EXISTS leaves them untouched.

CREATE FOREIGN TABLE IF NOT EXISTS public.market_candles_1m (
    exchange_id             SMALLINT,
    instrument_id           BIGINT,
    open_time               TIMESTAMPTZ,
    close_time              TIMESTAMPTZ,
    open                    NUMERIC,
    high                    NUMERIC,
    low                     NUMERIC,
    close                   NUMERIC,
    base_volume             NUMERIC,
    quote_volume            NUMERIC,
    trade_count             BIGINT,
    taker_buy_base_volume   NUMERIC,
    taker_sell_base_volume  NUMERIC,
    is_closed               BOOLEAN,
    source                  TEXT,
    quality_status          TEXT,
    revision                BIGINT,
    exchange_received_at    TIMESTAMPTZ,
    stored_at               TIMESTAMPTZ,
    updated_at              TIMESTAMPTZ
) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'market_candles_1m');

CREATE FOREIGN TABLE IF NOT EXISTS public.market_trades_raw (
    exchange_id      SMALLINT,
    instrument_id    BIGINT,
    trade_time       TIMESTAMPTZ,
    trade_id         TEXT,
    price            NUMERIC,
    size             NUMERIC,
    side             TEXT,
    retention_class  SMALLINT,
    received_at      TIMESTAMPTZ
) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'market_trades_raw');

-- DESIGN-02: the venue's price grid at T0. The plan generator rounds to the
-- tick of the spec version in force at the context's event time -- the same
-- version the Gateway attaches to the signal -- instead of guessing one from
-- the price magnitude (wrong by 10x for 18 of the 27 Bitget instruments on
-- 2026-10-15). Until these two exist the service runs on the derived tick and
-- every proposal carries TICK_SIZE_DERIVED.
CREATE FOREIGN TABLE IF NOT EXISTS public.instrument_spec_versions (
    id                      BIGINT,
    exchange_instrument_id  BIGINT,
    version                 BIGINT,
    normalized_hash         TEXT,
    spec                    JSONB,
    valid_from              TIMESTAMPTZ
) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'instrument_spec_versions');

CREATE FOREIGN TABLE IF NOT EXISTS public.canonical_pair_exchange_map (
    canonical_pair_id       BIGINT,
    exchange_id             SMALLINT,
    exchange_instrument_id  BIGINT,
    mapped_at               TIMESTAMPTZ
) SERVER altspotter_srv OPTIONS (schema_name 'public', table_name 'canonical_pair_exchange_map');
