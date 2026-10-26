# VPS1 audit remediation — 2026-10-26

Owner authorization: the supplied “VPS1 FULL AUDIT REMEDIATION, SAFE PRODUCTION
DEPLOYMENT AND UNINTERRUPTED LIVE TRADING” instruction. Scope is the fourteen
findings below. VPS2 remains strictly read-only. Profitability is not evaluated.

## Baseline and activation constraints

Observed 2026-10-26 17:28–17:35 UTC. Local main and remote main were
`f12f7950389eb34577d182e81d69c5d87739c0b8`. The seven untracked service audit
reports in the original worktree are preserved. Changes use a separate worktree
and branch `codex/vps1-audit-remediation-20261026`.

All seven VPS1 daemons, PostgreSQL and NATS were running. `alttech-schema.service`
was already failed (exit 1); it must not be recreated or force-started. Production
executables were read through `/proc/PID/exe`:

| Service | SHA256 | Installed release metadata |
|---|---|---|
| Spotter | f0e97f1655dc42b80e38f04aa896f9033d99c821673022a1c5a329efc38b8c09 | 20260925-034736-f0e97f16; no VCS |
| Macro | c5d9383d3a4775cd534801367def96a0b3029e4fc939e28fdb6f9a5c7bfa7c77 | source tree 46f040891194425ee25c9541ce70359a5c76ec5b361e4d124eed04e2fd9a6757; commit unknown |
| Tech | 8e507c86ba64523639ee8d268a8b3173ea553ad1cd60b7c5526e7bafa74de589 | VERSION absent |
| ML | fcf5e3f3558f17c6cc603613a87aa9d7205823412d7691ad353a830341ed5d44 | VERSION absent |
| Strategy | 306c58fe797f110f89a9258303ed91a9b7402b26eeb5b1ea8e2eb182bbe9f5b9 | 11760fa446a4 |
| Quality | d1ce55edd9796ba84c9ce80252b5e739dd0a8a78ccc31402c2eba71014959491 | 11760fa446a4 |
| Gateway | f028900144931c1d4ec5febdc0556aa79dc714e07a97a1a2b1b55ef575db6115 | source tree c4cc5a00b033314f7fd06b30afce64bccfff0d64a4be623f2999f9b83419fc29; commit unknown |

Observed owner migration inventories: Macro 0001–0005; Spotter 0001–0002;
Strategy 0001–0013; Quality 0001; Gateway 0001–0002. A labelled repeat query
resolved an ambiguous initial unlabelled output: **Tech** has one singleton with
checksum `e5fa355db01f033c4ee348a08af8d2be7d3b43ac5be0f7fbe21f9c4389b9b9c8`
(2026-08-28); **ML** has zero singleton rows. Quality had zero pending accepted
publications at the repeat query.
No production database writes were performed.

At 17:34 UTC JetStream had 9 streams/8 consumers, all observed durable consumers
had zero pending, ACK-pending and redelivered messages. Bars, Tech, Strategy,
Quality and Gateway were advancing. Existing local dist archives are preserved;
an exact production rollback package has not been established. Flat installers
delete unlisted active-tree files and restart active units even with
`--no-restart`; they must not be invoked without independent rollback capture.

VPS2 was already LIVE/HALTED with kill switch false, all live gates currently
passing, rp_live_v10, Trend ENFORCED/D, zero UNKNOWN orders and 9/9 reconciled
scopes. Halt reason: `altposition_healthy` failed while REDUCE_ONLY; transition
REDUCE_ONLY → HALTED by altops:live_gates at 1792982200921 ms. Its autonomous
recovery only covers its own NORMAL → HALTED transitions. This work must not
change its mode. At 17:34 UTC portfolio reported four positions; exact native
stop quantities have not yet been independently verified in this remediation.
Production activation remains subject to fresh safety checks and proven recovery.

Local baseline: approximately 3.7 GiB RAM, 876 MiB available, 8 GiB swap (1.7 GiB
used), 97 GiB disk free. Build/test policy: one job and one service at a time,
transient user scope MemoryHigh=1100M, MemoryMax=1600M, MemorySwapMax=3G,
CPUQuota=75%; no production compilation or test traffic.

## Implementation register

Source corrections are implemented locally; compilation, focused regression tests
and full suites remain in progress. FIXED_LOCAL does not imply tested or deployable.

| ID | Service-qualified audit ID | Correction | Status |
|---|---|---|---|
| 01 | ALTMACRO-AUDIT-001 | TLS hostname verification | FIXED_LOCAL |
| 02 | ALTSIGNALQUALITY-AUDIT-001 | Immutable publication replay | FIXED_LOCAL |
| 03 | ALTTECH-AUDIT-001 | Macro availability on method change | FIXED_LOCAL |
| 04 | ALTML-AUDIT-001 | Archive original features before ACK | FIXED_LOCAL |
| 05 | ALTSPOTTER-AUDIT-001 | Effective candle UPSERT corrections | FIXED_LOCAL |
| 06 | ALTMACRO-AUDIT-003 | Correction-aware message identity | FIXED_LOCAL |
| 07 | ALTSTRATEGY-AUDIT-001 | Holding-window validation | FIXED_LOCAL |
| 08 | ALTSIGNALGATEWAY-AUDIT-002 | Safe authentication skew arithmetic | FIXED_LOCAL |
| 09 | ALTMACRO-AUDIT-002 | Bounded HTTP phases | FIXED_LOCAL |
| 10 | ALTML-AUDIT-002 | Load verified model bytes | FIXED_LOCAL |
| 11 | ALTSIGNALQUALITY-AUDIT-002 | Load verified model bytes | FIXED_LOCAL |
| 12 | ALTSTRATEGY-AUDIT-002 | Canonical diagnostic outcomes | FIXED_LOCAL |
| 13 | ALTSIGNALGATEWAY-AUDIT-001 | Hash exact received payload bytes | FIXED_LOCAL |
| 14 | ALTTECH-AUDIT-002 | Six-module readiness inventory | FIXED_LOCAL |

## Validation and releases

Work in progress; no source release is yet tested, packaged or activated.

- Gateway's first `RelWithDebInfo` build (-O2 -g, one job) was interrupted with
  SIGTERM/exit 143 because paging during session.cpp compilation was excessive.
  No test had run. The second build retains -O2, disables debug symbols and
  module scanning (the service has no C++ modules), and uses GCC
  `--param=ggc-min-expand=5 --param=ggc-min-heapsize=16384`. Same systemd resource
  limits; still one compiler. The build remains in progress.
- Tech `tests/macro_availability_sql.py` PASS: actual writer SQL, native
  PostgreSQL 16.15, method-only/value changes, same-method correction, restart
  and cutoff filtering. Full C++ suite remains pending. Old method snapshots
  are not reconstructed: after replacement an earlier cutoff yields absence,
  never newly computed values. No feature set/version/schema/model order changed.
- PostgreSQL server/client packages were extracted into the task-owned local
  evidence directory, without installing a system service. A disposable local
  cluster uses only a private Unix socket, port 55486, 16 MiB shared buffers.
  Never use production DSNs or deployed-stack integration scripts for tests.
- Official pinned-family LightGBM C API header supports
  `LGBM_BoosterLoadModelFromString`. Loaders now pass a single verified owned
  text buffer, capped at 128 MiB and rejecting embedded NUL/truncation. Existing
  path-root, schema, feature-name/width and shape checks remain mandatory.
- Macro HTTP retains Boost TCP/TLS/HTTP and system NSS name-resolution policy.
  DNS runs in one owned `/usr/bin/getent` child per resolve, killed/reaped on
  timeout; no uncancellable resolver threads. VPS1 has this executable and its
  current unit permits execution. Connect/TLS share a connect deadline,
  write/read share a request deadline, teardown is bounded at one second,
  shutdown cancellation is polled at 50 ms, GET retry is capped at two attempts.
  Controlled loopback TLS/stall/NSS-worker tests are added but not yet run.
- Quality requires additive migration 0002 (original evaluation protobuf and
  original input digest). First-claim evidence, not current model/context,
  drives both replay paths. Conflicting or unverifiable legacy duplicates are
  refused; legacy pending rows without original evaluation evidence cannot be
  safely reconstructed. Fresh production pending count was zero; recheck
  immediately before any future activation. Existing decisions are not rewritten.
- ML requires the separate additive migration 0001_original_live_payload.
  Existing feature values/keys/training reads are preserved, even if backfill
  arrived first. Original live wire evidence commits before ACK; mismatch of
  original evidence is an error, never a successful archive. The old schema.sql
  and singleton fingerprint are untouched. New nullable bytea adds storage
  overhead within existing partition retention; capacity must be checked before
  activation. Its new bounded owner-only migration runner avoids schema rebuilds.
- GitHub automation read: zero Actions workflows and zero repository webhooks.
  No pushes or production rollout have occurred.

Pending: finish all seven local builds/suites, native archival/publication and
broker tests, focused sanitizers, contract checks, focused source commits,
immutable packages/ABI evidence and staging. Recheck production safety before
activation; LIVE/HALTED and the existing Tech schema failure must not be bypassed.
