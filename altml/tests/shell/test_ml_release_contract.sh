#!/usr/bin/env bash
# Release-layout contract for the fully native AltML: one binary, one service,
# an in-process trainer, and NO Python anywhere in the runtime path.
set -euo pipefail

ROOT="${1:?repository root required}"
BUILDER="$ROOT/scripts/build-release.sh"

# Unified-service source layout (native trainer included).
for path in \
    src/main.cpp \
    src/altml/core \
    src/altml/components/trainer.cpp \
    src/altml/components/trainer_logic.hpp \
    src/altml/components/storage_guard.cpp \
    src/altml/cli/train.cpp \
    proto/alttech \
    db/schema.sql \
    db/migrations/0001_original_live_payload.sql \
    scripts/db-bootstrap.sh \
    scripts/apply-additive-migrations.sh \
    config/altml.example.yaml \
    config/altml.env.example; do
    [[ -e "$ROOT/$path" ]] || {
        echo "missing AltML path: $path" >&2
        exit 1
    }
done

# The Python era is over: no scripts, no venv installer, no job exec-wrapper,
# no requirements. The model trains inside the service binary. The fragmented
# numbered-migration era is over too: one final db/schema.sql, no scaffolding
# for a probabilistic v2 contract that no runtime component implements. The
# owner-scoped additive live-payload extension is intentionally retained beside
# the immutable bootstrap schema.
for retired in \
    python \
    tests/python \
    src/altml/cli/job.cpp \
    scripts/install-ml-venv.sh \
    scripts/check-ml-build-platform.sh \
    proto/altml_v2.proto \
    deploy/systemd/altml-dataset-build.service \
    deploy/systemd/altml-dataset-build.timer \
    deploy/systemd/altml-training.service \
    deploy/systemd/altml-training.timer; do
    [[ ! -e "$ROOT/$retired" ]] || {
        echo "retired Python-era path still present: $retired" >&2
        exit 1
    }
done
if grep -RIn --exclude-dir=__pycache__ -E '(^|[^a-zA-Z])python3?([^a-zA-Z]|$)' \
    "$ROOT/src" "$ROOT/deploy/systemd" "$ROOT/config" >/dev/null 2>&1; then
    echo "runtime path references python" >&2
    exit 1
fi

# Exactly the single-service unit set…
for unit in altml.service altml-service-hardening.conf; do
    [[ -f "$ROOT/deploy/systemd/$unit" ]] || {
        echo "missing AltML unit: $unit" >&2
        exit 1
    }
done
# …and none of the retired pre-unification units.
for retired in \
    altml-label-builder.service \
    altml-model-registry.service \
    altml-inference.service \
    altml-nats-setup.service \
    altml.target; do
    [[ ! -e "$ROOT/deploy/systemd/$retired" ]] || {
        echo "retired unit still present: $retired" >&2
        exit 1
    }
done

# Reliability directives of the unified service, including the watchdog and
# the writable model root the in-process trainer requires.
service_unit="$ROOT/deploy/systemd/altml.service"
for directive in \
    'Type=notify' \
    'ExecStart=/opt/altml/bin/altml run --config /etc/altml/altml.yaml' \
    'Restart=on-failure' \
    'StartLimitIntervalSec=0' \
    'RestartSteps=5' \
    'RestartMaxDelaySec=5min' \
    'TimeoutStartSec=5min' \
    'WatchdogSec=60' \
    'ReadWritePaths=/var/lib/altml'; do
    grep -Fxq "$directive" "$service_unit" || {
        echo "missing AltML service reliability directive: $directive" >&2
        exit 1
    }
done
# A start budget locks the stage out for good after a database outage longer
# than the budget (reproduced 2026-09-26); only the restart delay is bounded.
if grep -Eq '^StartLimitBurst=' "$service_unit"; then
    echo "start limit present: a PostgreSQL outage would leave AltML permanently failed" >&2
    exit 1
fi
if grep -Fq 'ReadOnlyPaths=/var/lib/altml/models' "$service_unit"; then
    echo "model root is read-only: the in-process trainer cannot publish models" >&2
    exit 1
fi

# Release builder is Python-free and still test-gated. Word-bounded match so
# `pipefail` does not count as `pip`.
grep -Fq 'ctest --preset release' "$BUILDER"
if grep -qiE '(^|[^a-z])(python3?|wheelhouse|pip)([^a-z]|$)' "$BUILDER"; then
    echo "release builder still references Python tooling" >&2
    exit 1
fi

# Independence: no HFT-owned name may leak into the AltML contract.
if grep -RInE --exclude-dir=__pycache__ \
    '/opt/hft|/etc/hft|/var/lib/hft|HFT_POSTGRES_DSN|HFT_ANALYTICS|HFT_ML_CONTROL|hft\.v1' \
    "$ROOT/src" "$ROOT/proto" "$ROOT/config" "$ROOT/scripts" \
    "$ROOT/deploy/systemd"; then
    echo "HFT-owned name leaked into the independent AltML contract" >&2
    exit 1
fi

echo "release layout contract OK"
