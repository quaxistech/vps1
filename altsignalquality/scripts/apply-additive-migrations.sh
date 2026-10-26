#!/usr/bin/env bash
# Owner-scoped forward extensions, separate from the immutable full-schema
# bootstrap. Uses standard PG* connection environment. Never recreates schema.
set -euo pipefail
: "${PGDATABASE:?set the owner database explicitly}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -n "${1:-}" ]]; then
    migration_dir="$1"
elif [[ -d "$script_dir/../db/migrations" ]]; then
    migration_dir="$script_dir/../db/migrations"
else
    migration_dir="$script_dir/../share/altsignalquality/db/migrations"
fi
[[ -d "$migration_dir" ]] || exit 2
psql -Xq -v ON_ERROR_STOP=1 <<'SQL'
BEGIN;
SET LOCAL lock_timeout = '2s';
SET LOCAL statement_timeout = '10s';
CREATE TABLE IF NOT EXISTS altsignalquality.schema_migrations (
    version TEXT PRIMARY KEY, name TEXT NOT NULL, checksum TEXT NOT NULL,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
COMMIT;
SQL
for migration in "$migration_dir"/*.sql; do
    [[ -f "$migration" ]] || continue
    name="$(basename "$migration" .sql)"
    [[ "$name" =~ ^[0-9]{4}_[a-z0-9_]+$ ]] || exit 2
    version="${name%%_*}"
    migration_label="${name#*_}"
    checksum="$(sha256sum "$migration" | cut -d' ' -f1)"
    applied="$(psql -XAt -v ON_ERROR_STOP=1 -v migration_name="$version" <<'SQL'
SELECT checksum FROM altsignalquality.schema_migrations WHERE version = :'migration_name';
SQL
)"
    if [[ -n "$applied" ]]; then
        [[ "$applied" == "$checksum" ]] || { printf 'Migration checksum mismatch: %s\n' "$name" >&2; exit 1; }
        continue
    fi
    {
        printf "BEGIN;\nSET LOCAL lock_timeout = '2s';\nSET LOCAL statement_timeout = '10s';\n"
        cat "$migration"
        printf '\n'
        cat <<'SQL'
INSERT INTO altsignalquality.schema_migrations(version, name, checksum)
VALUES (:'migration_name', :'migration_label', :'migration_checksum');
COMMIT;
SQL
    } | psql -Xq -v ON_ERROR_STOP=1 -v migration_name="$version" -v migration_label="$migration_label" -v migration_checksum="$checksum"
done
