# Operations

## Service lifecycle

AltStrategy runs as `altstrategy.service` under systemd. Its maintenance
scheduler handles outcome maturation, statistics, lifecycle updates, snapshot
exports, and retention.

```sh
systemctl status altstrategy.service
systemctl start altstrategy.service
systemctl stop altstrategy.service
journalctl -u altstrategy.service -f
```

## Configuration and initialization

Configuration examples are under `config/`. Validate the service configuration
with:

```sh
altstrategy-config-check --file /etc/altstrategy/altstrategy.yaml --kind service
```

Apply database migrations with `altstrategy-db-migrate` and provision the
service-owned NATS stream with `altstrategy-nats-init`. Deployment layout and
service setup are documented in [Deployment](DEPLOYMENT.md).

## Routine commands

The service runs without cron or a separate training process. One-shot schema,
NATS, and archive tools are installed alongside the service; see the repository
README for the service command and [Deployment](DEPLOYMENT.md) for initialization.

Configuration and secret values are read from `/etc/altstrategy/`. Keep
credentials out of the source tree, command arguments, and logs.
