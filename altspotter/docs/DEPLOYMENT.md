# Deployment

AltSpotter is deployed as a native systemd service. Build the executable on a build host, stage the release for the target, and activate it on the target.

## Host layout

| Path | Purpose |
|---|---|
| <code>/opt/altspotter/</code> | Installed executable, libraries, migrations, and shared files |
| <code>/etc/altspotter/</code> | Production YAML configuration and environment file |
| <code>/var/lib/altspotter/</code> | Runtime state |

The service runs as the unprivileged <code>altspotter</code> user. PostgreSQL and NATS are configured for local service access; venue connections use outbound TLS.

## Provisioning

Install PostgreSQL and the runtime libraries required by the executable. Install and configure NATS separately. Then provision the service account, directories, database role, and environment file:

    sudo deploy/scripts/provision.sh

The script creates an empty database. Apply the checked-in migrations with <code>altspotter migrate</code>. Copy the example configuration files as a starting point and set environment values in <code>/etc/altspotter/common.env</code>. Keep credentials out of the source tree.

## Build and stage

On the build host:

    cmake --preset release
    cmake --build --preset release
    deploy/scripts/publish.sh user@host [release-id]

The publish script packages the executable, migrations, unit files, and configuration examples, verifies the target ABI, and stages the release under <code>/tmp/altspotter-deploy/</code>. Staging does not install or activate the release.

## Activate

The publish script prints the activation command for the staged release. Run that command on the target with the required system privileges. The installer places the files under <code>/opt/altspotter/</code>, reloads systemd, and starts the service.

Database migrations are separate from file installation. Run them with the service stopped, then start the unit:

    systemctl start altspotter-db-migrate.service
    systemctl start altspotter-nats-init.service
    systemctl enable --now altspotter.service

The migration and NATS initialization units are idempotent. See [Configuration](CONFIGURATION.md) for production file paths and [Operations](OPERATIONS.md) for routine service commands.
