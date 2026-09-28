# Database

AltTech owns a PostgreSQL database with the `alttech` application schema.
AltSpotter remains the source of market history and instrument reference data;
AltTech reads the required tables through `postgres_fdw` foreign tables in its
`public` schema. AltTech's application role has read access to those imports
and owns the service's analytics schema.

## Data sources

The foreign tables include market candles and aggregates, trade and book
aggregates, derivatives context, funding settlements, order-book snapshots,
the global observation universe, instrument/exchange registries and data
quality/clock metadata. The authoritative table list for provisioning is in
`docs/DEPLOYMENT.md` and the deployment configuration.

Application queries read imported market tables and write only to `alttech`
tables. AltTech does not create, alter or drop objects in AltSpotter's database.

## Schema and partitions

`db/schema.sql` defines the authoritative schema, constraints and partitioned
analytics tables. The `alttech-schema` utility applies and validates this
definition. `alttech.schema_version` records its identity. The maintenance
service ensures future partitions exist and applies table retention policies.

Database topology, least-privilege grants and provisioning steps are documented
in [DEPLOYMENT.md](DEPLOYMENT.md).
