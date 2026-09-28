#!/usr/bin/env bash
# What a release must contain, and what it must never contain.
set -euo pipefail
SRC="${1:?source dir}"
BIN="${2:?binary}"
fail() { echo "FAIL: $*" >&2; exit 1; }

# 1. Every documented subcommand exists and answers --help with exit 0.
for cmd in run train validate model-status dataset-build nats-setup; do
    "$BIN" "$cmd" --help >/dev/null || fail "'$cmd --help' did not succeed"
done
"$BIN" --version >/dev/null || fail "--version did not succeed"
# 2. An unknown subcommand is a usage error, not a silent no-op.
if "$BIN" definitely-not-a-command >/dev/null 2>&1; then
    fail "an unknown subcommand exited 0"
fi

# 3. The artefacts a deployment needs are in the repository.
for f in db/migrations/0001_altsignalquality_schema.sql \
         config/altsignalquality.example.yaml \
         config/altsignalquality.env.example \
         deploy/systemd/altsignalquality.service \
         proto/signalquality.proto proto/vendor/decision.proto \
         scripts/db-bootstrap.sh scripts/provision.sh scripts/install-systemd-units.sh; do
    [[ -f "$SRC/$f" ]] || fail "missing release artefact: $f"
done

# 4. No secret is committed. vps1.md holds live credentials and is gitignored;
#    the config examples must reference environment variables, never values.
if grep -REn '^[A-Z_]*(PASSWORD|SECRET|API_KEY|PASSPHRASE)[[:space:]]*=[[:space:]]*[^$C]' \
        "$SRC/config" 2>/dev/null | grep -v CHANGE_ME; then
    fail "a config example contains a literal credential"
fi
grep -q '^vps1.md$' "$SRC/.gitignore" || fail "vps1.md is not gitignored"
for key in postgres_dsn nats_url; do
    # ERE, so the ${...} braces are literal rather than an interval expression.
    grep -Eq "^${key}: \\\$\\{ALTSIGNALQUALITY_" "$SRC/config/altsignalquality.example.yaml" \
        || fail "$key in the config example is not an environment reference"
done

# 5. No placeholder implementations survived into the shipped source.
if grep -Rn --include=*.cpp --include=*.hpp -E '\b(TODO|FIXME|XXX|not implemented|unimplemented)\b' \
        "$SRC/src" >/dev/null 2>&1; then
    grep -Rn --include=*.cpp --include=*.hpp -E '\b(TODO|FIXME|XXX)\b' "$SRC/src" >&2
    fail "placeholder markers remain in src/"
fi
echo "release contract ok"
