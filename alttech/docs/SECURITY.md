# Security

## Threat model

AltTech is a read-mostly analytics service on a host it shares with altspotter.
It holds **no exchange credentials** and opens **no exchange connection** — it
cannot place an order, and a compromise of AltTech cannot move money.

What it can do, and therefore what is constrained: read altspotter's market
data, write its own analytics schema, and publish on its own NATS namespace.

## Privilege boundaries

**PostgreSQL.** AltTech and altspotter are separate databases on the same
instance; AltTech's database has no tables of altspotter's, only foreign
tables imported read-only through `postgres_fdw`. The role behind that FDW
mapping has `SELECT` on altspotter's `public` schema and nothing else there —
no write privilege, so AltTech cannot corrupt altspotter's market data even
through a bug. The same role owns AltTech's own database and its `alttech`
schema outright. The boundary is enforced by the database being physically
separate, not by careful coding.

```sql
-- on altspotter's database
GRANT CONNECT ON DATABASE altspotter TO alttech_app;
GRANT USAGE, SELECT ON ALL TABLES IN SCHEMA public TO alttech_app;

-- alttech_app's own database, which it owns outright
CREATE DATABASE alttech OWNER alttech_app;
```

Because AltTech's schema lives in its own database, no `CREATE` grant on
altspotter's database is needed at all — a stronger version of the same
isolation than a shared-database deployment could offer.

**NATS.** AltTech has its own user, not altspotter's, restricted to publishing
under the namespace it owns:

```
publish:   ["alttech.>", "$JS.API.>", "$JS.ACK.>"]
subscribe: ["bar.>", "md.>", "evt.universe.>", "alttech.>", "_INBOX.>"]
```

A bug in AltTech cannot publish into an altspotter subject.

**Advisory locks.** Schema application uses an AltTech-specific advisory lock,
never altspotter's. The two schemas are independent and must not be able to
block each other.

## Process isolation

`alttech.service` runs as an unprivileged system user with no login shell and
no home directory, and drops everything it does not need:

- `CapabilityBoundingSet=` and `AmbientCapabilities=` — no capabilities at all;
- `NoNewPrivileges=true`, `RestrictSUIDSGID=true`, `LockPersonality=true`;
- `ProtectSystem=strict`, `ProtectHome=true`, `PrivateTmp=true`,
  `PrivateDevices=true`, `ProtectProc=invisible`;
- `ProtectKernelTunables`, `ProtectKernelModules`, `ProtectKernelLogs`,
  `ProtectControlGroups`, `ProtectClock`, `ProtectHostname`;
- `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`, `RestrictNamespaces=true`,
  `RestrictRealtime=true`;
- `SystemCallArchitectures=native` with a deny filter over `@clock`,
  `@cpu-emulation`, `@debug`, `@module`, `@mount`, `@obsolete`, `@raw-io`,
  `@reboot`, `@swap`;
- `LimitCORE=0` — no core dumps, so a crash cannot spill credentials to disk;
- `MemoryMax=1536M`, `TasksMax=128`, `Nice=5`.

The resource limits are also a safety property: AltTech yields CPU to
altspotter, because analytics can be recomputed and lost market data cannot.

## Secrets

Credentials live only in `/etc/alttech/common.env`, owned `root:alttech` with
mode `0640`: readable by the service, not by other users. They reach the process
through systemd's `EnvironmentFile` and are never written to a config file that
is committed, and never logged.

`${VAR}` expansion fails hard on a missing variable. A secret is either present
or start-up fails — it is never silently defaulted to something weak.

The installed tree is `root`-owned with the service group given read access
only, so the service cannot modify its own binaries.

## Network exposure

None. PostgreSQL and NATS both bind to `127.0.0.1`, and AltTech listens on
nothing at all — it is a client of both and a server to no one. Remote access
for an operator is an SSH tunnel.

## Data handling

Analytics are derived statistics over public market data. Nothing AltTech stores
is personal data or a secret. Its outputs are, however, a trading input, so the
integrity properties matter more than the confidentiality ones:

- every value carries a validity bit, so a missing value cannot be mistaken for
  a real zero;
- every cross-venue value carries a quality status and the venue masks that
  explain it;
- every row records the feature-set version, engine version and source bar
  revision it was computed from.

A consumer can therefore always tell what a number means and what it was
computed from — which is the property that matters when the number is about to
move money.

## Failure posture

Fail closed. A schema or contract integrity breach stops the module and, with
it, the process: publishing analytics that silently mean something different is
worse than publishing none. Transient failures degrade quality rather than
fabricate values.
