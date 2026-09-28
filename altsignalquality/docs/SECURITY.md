# Security

## Secrets

Every credential lives in `/etc/altsignalquality/common.env`
(`root:altsignalquality`, `0640`) and is referenced from the YAML as
`${ALTSIGNALQUALITY_*}`. A missing variable is a startup error, so a service can
never come up with a silently empty DSN or NATS URL.

Nothing secret is committed: `vps.md` is gitignored, the config examples contain
only `CHANGE_ME` placeholders, and a release-contract test fails the build if
either stops being true.

Nothing secret is logged. The structured logger emits named events with explicit
fields; connection strings, passwords and tokens are never among them. Errors
from libpqxx are surfaced with their message, never with the DSN.

## Least privilege

The database role has full rights on the `altsignalquality` schema and
**`SELECT` only** on the six AltStrategy tables it reads. It cannot write to
another service's schema even by mistake, which is the mechanical version of the
service boundary.

The systemd unit runs as an unprivileged `altsignalquality` account with an
empty capability bounding set, `NoNewPrivileges`, `ProtectSystem=strict`,
`ProtectHome`, `PrivateTmp`, `PrivateDevices`, restricted address families,
`RestrictNamespaces`, `RestrictRealtime`, `LockPersonality`, a syscall filter
and `TasksMax=32`. The only writable path is `/var/lib/altsignalquality`.

## Untrusted inputs

**NATS messages.** Every candidate is parsed defensively and structurally
validated; an unparseable or malformed one is terminated rather than retried
forever. A payload that is not a proposal is *absence*, not corruption.

**Registry rows.** A model's `path` is data, not authority: it is resolved to a
canonical path and rejected unless it is strictly inside the configured model
root, then its sha256 must match the checksum recorded at registration, then its
feature count and feature-schema hash must match the running contract. A model
failing any of these is refused, not scored with.

**Retention policy rows.** Table and column names come from this service's own
policy table, seeded only by the migration — and are still validated as plain
identifiers before being interpolated, because a policy row is data and data is
never trusted as SQL.

**Everything else is parameterised.** No user-supplied value is ever
concatenated into a statement.

## The service cannot fabricate its own input

Its NATS user is permitted to publish only under `signalquality.>` (plus the
JetStream API and its own inbox). It has **no write permission on
`signal.proposed.>`** — the subject it consumes.

This is load-bearing, not decorative. A filter able to write into its own input
subject is a filter able to fabricate a trade proposal and then approve it, and
no amount of application logic compensates for that. The property is asserted by
an integration test (`the service cannot publish into AltStrategy's namespace`),
which is also why that test needs the producer's credentials to publish a
candidate at all.

The same shape holds in PostgreSQL: the service role receives `SELECT` on the
AltStrategy tables it reads and no write access to those tables.

## What the service cannot do

It has no exchange credentials, no order path, and no code that could place,
modify or cancel an order. It cannot size a position or set leverage. The
strongest action available to it is publishing a decision about a signal
AltStrategy already produced.
