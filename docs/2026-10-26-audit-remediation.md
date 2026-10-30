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
| 01 | ALTMACRO-AUDIT-001 | TLS hostname verification | TESTED_LOCAL |
| 02 | ALTSIGNALQUALITY-AUDIT-001 | Immutable publication replay | FIXED_LOCAL |
| 03 | ALTTECH-AUDIT-001 | Macro availability on method change, Gateway T0 cutoff | FIXED_LOCAL |
| 04 | ALTML-AUDIT-001 | Archive original features before ACK | TESTED_LOCAL |
| 05 | ALTSPOTTER-AUDIT-001 | Effective candle UPSERT corrections | TESTED_LOCAL |
| 06 | ALTMACRO-AUDIT-003 | Correction-aware message identity | TESTED_LOCAL |
| 07 | ALTSTRATEGY-AUDIT-001 | Holding-window validation | TESTED_LOCAL |
| 08 | ALTSIGNALGATEWAY-AUDIT-002 | Safe authentication skew arithmetic | TESTED_LOCAL |
| 09 | ALTMACRO-AUDIT-002 | Bounded HTTP phases | TESTED_LOCAL |
| 10 | ALTML-AUDIT-002 | Load verified model bytes | FIXED_LOCAL |
| 11 | ALTSIGNALQUALITY-AUDIT-002 | Load verified model bytes | TESTED_LOCAL |
| 12 | ALTSTRATEGY-AUDIT-002 | Canonical diagnostic outcomes | TESTED_LOCAL |
| 13 | ALTSIGNALGATEWAY-AUDIT-001 | Hash exact received payload bytes | TESTED_LOCAL |
| 14 | ALTTECH-AUDIT-002 | Six-module readiness inventory | TESTED_LOCAL |

## Validation and releases

Work in progress. Gateway, Macro and Strategy are tested locally. No candidate has been packaged or activated.

- Gateway's first `RelWithDebInfo` build (-O2 -g, one job) was interrupted with
  SIGTERM/exit 143 because paging during session.cpp compilation was excessive.
  No test had run. The second build retains -O2, disables debug symbols and
  module scanning (the service has no C++ modules), and uses GCC
  `--param=ggc-min-expand=5 --param=ggc-min-heapsize=16384`. Same systemd resource
  limits; still one compiler. The second build completed; Gateway passed all 123 CTests twice (the second run followed a diagnostic-only log improvement). Focused ASan/UBSan passed 19 cases / 174 assertions.
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
  Controlled loopback TLS/stall/NSS-worker tests passed in release and under ASan/UBSan. All 74 Macro CTests passed; isolated JetStream tests proved correction IDs, retry deduplication and restart/outage behavior.
- Quality requires additive migration 0002 (frozen final publication bytes and
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
  The branch has been pushed through the tested Gateway fixes; no production rollout has occurred.

Pending: finish all seven local builds/suites, native archival/publication and
broker tests, focused sanitizers, contract checks, focused source commits,
immutable packages/ABI evidence and staging. Recheck production safety before
activation; LIVE/HALTED and the existing Tech schema failure must not be bypassed.

## Bounded patch and contract map

| Findings | Owner and source boundary | Correction and preserved contract | Regression evidence / limit |
|---|---|---|---|
| 01, 09 | Macro `core/http.cpp` | Verify chain and requested host; deadline/cancellation spans DNS, TCP, TLS, write/read and shutdown. Provider credentials/targets never enter errors. Existing GET policy and body limit remain. | Controlled trusted/wrong-SAN/untrusted/expired TLS, stalled phases, NSS child reap and cancellation; release and sanitizers passed. |
| 06 | Macro `components/publisher.cpp::revision_message_id` | Semantic content hash excludes only ingest time; corrected value/quality receives a new broker identity. Observation natural keys and FDW identity remain. | Real private JetStream duplicate, correction, restart and outage cases passed. Initial rollout may append one equivalent transport event; no stream purge. |
| 05 | Spotter `persistence/candle_store.cpp` | Compare all effective assigned values; absent optional quantities retain known data. Receipt alone is a no-op; source is meaningful provenance. | Native test exercises both real UPSERTs and exact revision/WriteStats. Tech's revision barrier re-reads current persisted revision and recomputes before ACK; no feature formula change. |
| 03 | Tech `modules/macro/macro_module.cpp::store/run_macro` | Method/value replacement advances computation time, wire availability follows successful storage. Keys, feature-set identity/order and model schema stay fixed. | Actual writer SQL tested against native PostgreSQL: method-only, changed values, late correction, restart and cutoff. An old cutoff gets absence after replacement; the single-row schema cannot recover the older method snapshot. |
| 14 | Tech `application/module_inventory.hpp`, `app/main.cpp` | One six-role inventory drives supervisor size, launch, reservation and log count. Roles/order/readiness conditions stay fixed. | Inventory/readiness regression and all 135 Tech CTests passed. Existing schema-service failure remains an independent activation blocker. |
| 04 | ML `components/inference.cpp`, `components/feature_archive.hpp` | Commit original live envelope before cache/model processing and source ACK; return persistence errors to existing 1-second NAK path. Original payload is independent of a prior backfill occupying the same key. | Native archive and actual-consumer fault/restart tests passed with absent and loaded models, delayed NAK, file-backed broker restart, original/revised vectors and duplicate forecast identity. No new queue or unbounded memory buffer. |
| 10, 11 | ML/Quality `core/verified_artifact.hpp` and live loaders | One bounded owned buffer is hashed then loaded with the native LightGBM string API, including ML magnitude models. Existing root/schema/name/width checks stay mandatory. | Checksum, empty/NUL/missing/oversized/replaced path and hand-authored constant-model parity tests added. No training, promotion or production artifact changes. |
| 07 | Strategy `engine/io/plan_config.hpp`, `trade_plan.cpp`, `proposal.cpp` | Explicit invalid holding hours fail before conversion/multiplication; direct duration admission precedes arithmetic and clamp. Valid default and plan formulas stay fixed. | First revision passed 205 tests. Additional malformed direct-proposal and expiry-boundary cases need a final rerun before packaging. |
| 12 | Strategy `engine/io/maintenance_scheduler.cpp::calibrate_geometry`, `engine/outcomes.hpp` | Existing v2 path/evaluator and realized eligibility drive diagnostics; version/fill labels included. Sampling/grid/costs and active geometry unchanged. | Canonical ambiguity/gap/fill/time-stop cases and denominator regression passed. No historical outcome update or automatic parameter calibration. |
| 08 | Gateway `domain/auth.cpp` | Reject invalid nonpositive time/negative skew, then compare unsigned distance without signed overflow. Inclusive skew window and HMAC/nonce rules stay fixed. | Full 123-test suite and focused sanitizers passed, including int64 extremes and exact window boundaries. |
| 13 | Gateway `core/codec.cpp`, `cli/client.cpp` | Bounded raw wire scanner verifies original payload field slices in delivery and nested query responses before parse/exposure/ACK. Server hashes and framing unchanged. | Unknown/reordered fields, nested results, duplicate payloads, malformed lengths and truncation passed in release/sanitizers. |

All source `.proto` files remain unchanged. Gateway protocol 2 / public signal
schema 3 remain authoritative; internal service envelope schema numbers are not
substituted for the public version. Existing vendored producer/consumer tests
remain in the full suites. PostgreSQL writes remain within the owning service.
ML forecast/Strategy cutoff and original plan geometry are unchanged. Quality
freezes the original final envelope before its first claim, and both retry paths
publish those exact bytes; Gateway continues hashing/storing the original source
payload. No VPS2 source, configuration, migration or financial history is changed.

## Checkpoint at 19:30 UTC

Tested/pushed source commits: `5e82e69bb9aa6bf4ee52781750a0fa6d2f6f5384`
(authentication), `ccf61219769ce7772c625614f7d8b9e0e4d9c2e2` (wire verification).
Tested Macro commits: `7065a51` (TLS/deadlines), `bee278c` (revision identity and
pinned-header consistency); push still pending. Baseline checkpoint: `94645d0`.

Macro's first configure lacked liblzma development metadata. Matching Ubuntu
headers were extracted privately; the existing runtime was used, without a
system package install. During path correction two local build invocations
briefly overlapped; both were stopped (exit 143), then one build was restarted.
A nonblocking flock now rejects overlapping heavy commands. Builds/tests and
the disposable PostgreSQL fixture now share a transient user resource slice
with the requested aggregate limits. No production service was affected.

The first Macro suite had 73/74 passing: the JetStream fixture buffered its
READY marker. Switching the fixture to raw pipe reads fixed it; targeted rerun
and then all 74 tests passed. No failed test is being counted as a pass.

Macro provisioning had included the system NATS 3.7.0 SDK while linking pinned
3.8.2. It now includes the pinned header; no library version or stream setting
changed. Known `/var/lib/altml/models/*/*.txt` artifacts: 33, maximum 1,026,161
bytes, below the new 128 MiB bound. No artifact contents were inspected remotely.

Quality forward migrations passed local application and idempotence. Its 0001
checksum still equals production (`50414498e285a5ebe0231dc1120b09370f7d1320cf735063da4b10ff88593a56`).
An initial attempt against an uninitialized test schema correctly failed; the
existing owner schema was then prepared and the tests rerun. The migration runner
never changes roles, grants or retention and refuses checksum drift.

## Checkpoint at 20:06 UTC

Strategy passed all 205 CTests, including eleven native integration tests.
An initial compile correctly rejected the legacy `PathResult` passed to the v2
evaluator; the diagnostic now uses the existing `read_price_path` API. The full
corrected build and suite passed. Holding validation: `87ed2a1`; canonical
diagnostic: `117f589`. No historical outcome or parameter was changed.

Macro commits `7065a51` and `bee278c` and checkpoint `8fc0b29` are pushed.
Four retained local release archives contain binaries exactly matching the
running Macro, Strategy, Quality and Gateway hashes above. Read-only copies of
the other three running binaries are being captured outside active trees; these
are not complete operator-configuration/unit recovery snapshots.

At 20:05–20:06 UTC VPS2 remained LIVE/HALTED, Intelligence READY/ENFORCED/D,
Execution READY with four verified native positions and zero UNKNOWN orders.
A read-only owner-table join verified four OPEN positions with PLACED,
reduce-only STOPs, exact `actual_quantity = quantity = verified_quantity`:
DOGEUSDT SHORT 268, HBARUSDT SHORT 84, LINKUSDT LONG 1, SOLUSDT LONG 0.2.
Proof ages were 878–2,891 ms. No exchange request was issued for validation.

Current bounded sequential queue: Spotter build, then Tech, ML and Quality.
Remaining: those four full builds/suites, native archival/publication tests,
immutable packaging/ABI/staging and remaining focused commits/push. All
production activation remains gated by fresh LIVE/NORMAL evidence. No production
service has been restarted, no configuration changed and no migration applied.

At 20:10 UTC review found the forecast holding-estimate clamp also needed a
guard for direct malformed `TradePlanVariant` callers, independently of the
already validated `PlanConfig` path. Invalid durations now expire immediately
and carry zero holding estimates; valid plans keep their formulas. Two more
regression cases cover forecast/no-forecast and supported expiry boundaries.
Finding 07 is returned to FIXED_LOCAL until the final Strategy rerun. The prior
205-test result applies to the preceding source only. Rerun Strategy after the
current heavy sequence; do not package its stale build.

## Resumption on 2026-10-29

Spotter completed its full CTest invocation: 47 Catch cases and 6,870
assertions passed, with one unrelated PostgreSQL-restart integration case
explicitly skipped because no local restart command was provided. The actual
candle correction case ran. Tech completed all 135 CTests, including the
inventory and native PostgreSQL macro-availability tests. Commits are
`d7617e9` (Spotter), `b1799c1` (Tech availability) and `7d4ec93` (Tech
inventory); these were local and unpushed at resumption.

ML's first run passed 86/88 tests and failed two: the release-layout test still
prohibited the newly required additive migration directory, and the new
consumer fixture could not observe model-activation logs while systemd directed
them to journald. The release contract now requires the specific extension and
runner; the isolated probe explicitly logs to stdout. The corrected ML suite
passed all 88 tests, including both actual-consumer modes; the initial failure
log is preserved. One additional malformed-model regression is pending. Quality
and the final Strategy rerun follow serially under the same aggregate resource
limits. The skipped
Spotter PostgreSQL-restart case will be run separately against the local fixture.
An additional isolated test now exercises the actual Quality evaluator through
failed publication, NAK/redelivery, duplicate and process-restart recovery,
frozen bytes and conflicting same-ID input; it has not yet been compiled or run.
Macro and Gateway's earlier successful sanitizer outputs were under `/tmp` and
are no longer present; their preserved sanitizer binaries will be rerun, with
new logs retained in the task evidence directory.

At 05:55 UTC on October 29, all seven VPS1 daemons, PostgreSQL and NATS were
active; `alttech-schema.service` remained failed. VPS2 independently remained
LIVE/HALTED with kill switch false, gates passing, ENFORCED/D, `rp_live_v10`,
zero held positions and zero UNKNOWN orders; all nine reconciliation scopes
were RECONCILED. The earlier four-position snapshot is historical, not current.
No service was restarted on either production server, and no production
configuration, migration, position or financial record was changed by this work.

At 06:02 UTC a read-only cross-check found the VPS2 Ingest status file still
displayed cursor/head/watermark 32370 from the long-lived session. Current owner
data showed Gateway journal head and VPS2 client acknowledgement at 38603,
and VPS2's persisted ingest watermark and newest stored signal also at 38603;
the last signal was received and ACKed at 06:02:05 UTC. This is a pre-existing
status-versus-owner-data discrepancy; do not treat the status values as current
cursor proof or change VPS2 as part of this task. Source inspection shows the
displayed Gateway cursor/head are session observations. Final deployment
preflight must use fresh owner data and validate recovery independently.

At 06:06–06:09 UTC all seven running executable hashes matched the October 26
baseline. Macro's process had independently restarted on October 27 at 03:17:35
UTC (systemd reports one restart); no restart was initiated by this task. Its
restart-window journal entries are unavailable, so the cause, interruption and
recovery gap are not established. Current owner metadata shows provider IDs 1,
2 and 5 succeeded recently with zero consecutive failures; this demonstrates
ongoing polling, not complete historical coverage. The old `macro_ingest_runs`
rows record bootstrap activity and alone do not measure current polling.

At 06:26 UTC the bounded serial queue remains in Quality's first build (94/139);
Strategy follows it. ML's successful full `LastTest.log` and the retained full
Spotter/Tech/Macro/Gateway logs were copied into the task evidence directory
before focused runs can overwrite them. Commit `1e3dad4` documents the ML
additive migration, storage overhead, NAK and executable-rollback limitations.
The existing actual ML loader fixture is being extended with invalid root,
schema, width, checksum, native format and classifier/magnitude shape cases;
these additions are untested and must not be counted yet. The final focused
script now rebuilds the changed probes and reruns full Quality/ML suites, then
the preserved Gateway/Macro sanitizers and skipped Spotter restart case.

Read-only VPS1 ABI evidence was refreshed at 06:24 UTC. The packaging helper
now checks version requirements against each exact target soname's defined
ELF versions, as well as runtime-library availability and local RPATH absence.
The prior October 26 runtime evidence is retained. No candidate has been
packaged, uploaded, installed or activated at this checkpoint.

Scoped review of finding 03 found Gateway's `load_macro_asof` bounded only bar
time, allowing later Tech computations into an earlier T0 enrichment. A narrow
`computed_at <= T0` reader predicate and actual `JournalStore` PostgreSQL test
are now added but untested. Gateway must be rebuilt and its full suite rerun
serially in the final focused queue before packaging. This extends finding 03
across its necessary consumer boundary without a schema or protocol change.

The other direct table reader is ML's macro backfill, which preserves the
producer's computation time in its owned archive. The econ training and
replay use `aligned_source_sql_pit` (`source.computed_at <= target.computed_at`).
Strategy has no direct reader of this table. The retained legacy training join
uses the older bar-time rule; that pre-existing compatibility limitation is
not a claim of causal legacy retraining. No training or model promotion is run.
At 06:36:36 UTC a read-only check of `/etc/altml/altml.yaml` observed the
explicit `training.mode: econ`. This is deployed-file configuration evidence;
legacy remains source-reachable if selected. Only the non-secret mode enum was
reported, in `ml-deployed-training-mode-20261029.json`.

At 06:42:50 UTC the resumed serial queue completed successfully. Quality passed
147/147 CTests, including the checksummed malformed-model case; Strategy's final
holding-clamp revision passed 207/207, including eleven native integration tests.
Their full logs were preserved before any focused run. The Gateway T0 reader
correction is checkpointed as `7ae9cb6`, explicitly untested until its queued
build/suite completes.

The revised final focused queue is now running under the same aggregate
resource limits. It separately configures/builds Quality's evaluator probe and
runs the full suite, then builds/runs ML's full suite with nine real-loader
refusals, Gateway's full suite with the native cutoff and exact recovered
Quality-envelope validation, Gateway/Macro sanitizers, and Spotter's previously
skipped private PostgreSQL restart case. Quality now includes an actual fresh
model-absent claim before failed publication and a restart under changed policy,
in addition to hand-authored first claims for historical scored/cold evidence.
These additional tests remain pending at this checkpoint.

The first final-focused invocation failed compiling the new Quality probe:
it lacked the codec/generated-protobuf include, leaving the protobuf namespaces
undeclared. No test ran in that invocation. The explicit include is added and
the queue was restarted serially; the full initial failure is retained as
`final-focused-tests-first-failure-20261029.log`. Staging preflight at 06:43:56
UTC found the proposed task-owned VPS1 directory absent and 64,869,158,912 free
bytes. No remote directory has yet been created.

## Resumption 2026-10-29 15:45–19:36 UTC (Claude)

Root cause of the two ML failures (`altml_archive_consumer_recovery`,
`altml_model_loader_refusals`): a NatsClient teardown race, not the loader or
the archive. The destructor closed the connection and freed the client while
the library's disconnected callback was still queued on its own thread; the
callback then read `shutting_down_` from freed memory and logged through a
logger that `exit()` had already destroyed (gdb: SIGSEGV in
`spdlog::sinks::sink::should_log` from `on_disconnected`, main in `_dl_fini`;
`ml-teardown-gdb-20261029.log`). Intermittent, so one run of each passed and
the next failed. Quality and Strategy already waited for the closed callback;
ML, Gateway, Macro, Spotter and Tech did not, and every daemon stops the same
way the probe does. All six now wait (bounded, 5 s) for the closed callback,
the last one the library queues for a connection, and notify under the lock
so a destructor that wakes on its own cannot release the condition variable
first (Quality and Strategy received that one-line change too).

Results, serial, same resource limits (`queue-20261029-nats-*.log`):
ML 89/89, then both former failures 10/10 repeats; Quality 148/148 (fresh
publication evidence `quality-final-fixtures-20261029-nats-fvDBES`); Gateway
125/125 including the untested T0 cutoff reader (`7ae9cb6`) and recovered
Quality envelopes; Macro 74/74; Spotter full suite and the previously skipped
private PostgreSQL restart case; Tech 135/135; Strategy 207/207; preserved
Gateway sanitizer binary 19 cases / 174 assertions; Macro controlled HTTPS
sanitizer cases pass. The private test cluster (unit
`vps1-audit-pg-20261029`) had stopped at 10:54 UTC and was restarted with the
same options. No production host was changed by this work.
