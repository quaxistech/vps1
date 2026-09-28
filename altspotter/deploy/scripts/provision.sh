#!/usr/bin/env bash
# Prepare a host for AltSpotter: service account, directories, PostgreSQL role
# and database, and the secrets file.
#
# IDEMPOTENT. A re-run on a live host changes nothing that is already correct:
# it creates no second account, drops no database, and — importantly — does not
# regenerate a password that already exists, because doing so would lock the
# running service out of its own database.
#
# It imports NOTHING. The database it creates is empty; schema comes from
# `altspotter migrate`, which reads db/migrations/. There is deliberately no
# pg_dump/pg_restore path here: a new deployment starts from the repository and
# its migrations, never from another host's operational state.
#
#   sudo deploy/scripts/provision.sh
#
# Secrets are written to /etc/altspotter/common.env (root:altspotter, 0640) and
# are never echoed.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 2; }

SVC=altspotter
ETC=/etc/$SVC
OPT=/opt/$SVC
VAR=/var/lib/$SVC
DB=$SVC
ROLE=${SVC}_app

say() { printf '==> %s\n' "$*"; }

say "service account"
getent group  "$SVC" >/dev/null || groupadd --system "$SVC"
getent passwd "$SVC" >/dev/null || useradd --system --gid "$SVC" \
    --home-dir "$VAR" --shell /usr/sbin/nologin "$SVC"

say "directories"
install -d -o root      -g "$SVC" -m 0750 "$ETC"
install -d -o root      -g root   -m 0755 "$OPT" "$OPT/releases"
install -d -o "$SVC"    -g "$SVC" -m 0750 "$VAR"

say "postgresql role and database"
command -v psql >/dev/null || { echo "psql not found; install postgresql first" >&2; exit 1; }

role_exists() { sudo -u postgres psql -tAc \
    "SELECT 1 FROM pg_roles WHERE rolname='$ROLE'" | grep -q 1; }
db_exists()   { sudo -u postgres psql -tAc \
    "SELECT 1 FROM pg_database WHERE datname='$DB'" | grep -q 1; }

# Reuse the existing password when one is already recorded; only generate on a
# genuinely fresh host. Rotating it here would break a running service.
if [[ -f $ETC/common.env ]] && grep -q '^ALTSPOTTER_DB_PASSWORD=' "$ETC/common.env"; then
    PGPW="$(grep '^ALTSPOTTER_DB_PASSWORD=' "$ETC/common.env" | cut -d= -f2-)"
    say "  reusing the recorded database password"
else
    PGPW="$(head -c 32 /dev/urandom | base64 | tr -d '/+=' | head -c 32)"
    say "  generated a new database password (not printed)"
fi

if role_exists; then
    say "  role $ROLE exists"
else
    sudo -u postgres psql -q -c "CREATE ROLE $ROLE LOGIN PASSWORD '$PGPW'"
    say "  role $ROLE created"
fi
if db_exists; then
    say "  database $DB exists"
else
    sudo -u postgres psql -q -c "CREATE DATABASE $DB OWNER $ROLE"
    say "  database $DB created (empty; run 'altspotter migrate' for the schema)"
fi

say "secrets file"
if [[ ! -f $ETC/common.env ]]; then
    umask 027
    cat > "$ETC/common.env" <<ENV
# AltSpotter secrets. root:altspotter 0640. Never in git, never in a log.
ALTSPOTTER_ENVIRONMENT=production
ALTSPOTTER_LOG_LEVEL=info
ALTSPOTTER_INSTANCE_ID=altspotter-1
ALTSPOTTER_DB_NAME=$DB
ALTSPOTTER_DB_USER=$ROLE
ALTSPOTTER_DB_PASSWORD=$PGPW
ALTSPOTTER_POSTGRES_DSN=postgresql://$ROLE:$PGPW@127.0.0.1:5432/$DB
ALTSPOTTER_NATS_URL=nats://${SVC}_app:CHANGE_ME@127.0.0.1:4222
ENV
    say "  created (NATS password still CHANGE_ME — set it with the NATS config)"
else
    say "  exists, left untouched"
fi
chown root:"$SVC" "$ETC/common.env"
chmod 0640 "$ETC/common.env"

say "done. Next: install a release, then"
say "  altspotter migrate --config $ETC/altspotter.yaml --common $ETC/common.yaml"
