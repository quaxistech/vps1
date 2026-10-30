"""Run the real forward-upgrade CLI against an isolated real PostgreSQL DB."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import urlparse
import uuid

admin = os.environ.get("ALTTECH_TEST_PG_DSN", "")
if not admin or not shutil.which("psql"):
    raise SystemExit(77)
uri = urlparse(admin)
assert uri.scheme == "postgresql" and uri.hostname in (None, "localhost", "127.0.0.1")
assert uri.password is None
binary, schema, upgrade = sys.argv[1:]
name = "alttech_upgrade_test_" + uuid.uuid4().hex
dsn = admin + ("&" if "?" in admin else "?") + "dbname=" + name

def sql(target, statement):
    return subprocess.run(["psql", "-Xq", "-v", "ON_ERROR_STOP=1", "-d", target],
                          input=statement, text=True, check=True, timeout=30)

def cli(*args, success=True):
    result = subprocess.run([binary, "--dsn", dsn, *args], capture_output=True, text=True, timeout=60)
    assert (result.returncode == 0) == success, result.stdout + result.stderr

sql(admin, "CREATE DATABASE " + name)
try:
    cli("--schema", schema, "--days-back", "0", "--days-ahead", "1")
    cli("--check", "--upgrade", upgrade, success=False)
    cli("--upgrade", upgrade)
    cli("--upgrade", upgrade)  # exact replay is a no-op
    cli("--check", "--upgrade", upgrade)
    cli("--check", "--schema", schema)  # base checksum stays intact
    with tempfile.TemporaryDirectory() as directory:
        changed = Path(directory) / Path(upgrade).name
        changed.write_text(Path(upgrade).read_text() + "\n-- rewritten applied bytes\n")
        cli("--upgrade", str(changed), success=False)
        failing = Path(directory) / "0002_transaction_failure.sql"
        failing.write_text("CREATE TABLE alttech.must_rollback(id int); SELECT missing_upgrade_function();")
        cli("--upgrade", str(failing), success=False)
    sql(dsn, """
DO $$ BEGIN
 ASSERT to_regclass('alttech.must_rollback') IS NULL;
 ASSERT NOT EXISTS (SELECT 1 FROM alttech.schema_upgrades WHERE name='0002_transaction_failure.sql');
 ASSERT (SELECT count(*)=1 FROM alttech.schema_upgrades);
END $$;
BEGIN;
INSERT INTO alttech.analytics_outbox(message_id,subject,payload) VALUES('synthetic-rollback','synthetic.test','\\x01');
ROLLBACK;
DO $$ BEGIN ASSERT NOT EXISTS(SELECT 1 FROM alttech.analytics_outbox); END $$;
""")
    print("PASS actual schema, exact upgrade replay/checksum, base compatibility, rollback")
finally:
    sql(admin, "DROP DATABASE " + name + " WITH (FORCE)")
