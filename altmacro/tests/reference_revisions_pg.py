"""Exercise actual owner migrations and immutable causal revision capture on local PG."""
import os
from pathlib import Path
import shutil
import subprocess
from urllib.parse import urlparse
import uuid

admin = os.environ.get("ALTMACRO_TEST_PG_DSN", "")
if not admin or not shutil.which("psql"):
    raise SystemExit(77)
uri = urlparse(admin)
assert uri.scheme == "postgresql" and uri.hostname in (None, "localhost", "127.0.0.1")
assert uri.password is None
name = "altmacro_revision_test_" + uuid.uuid4().hex
dsn = admin + ("&" if "?" in admin else "?") + "dbname=" + name

def sql(target, statement):
    subprocess.run(["psql", "-Xq", "-v", "ON_ERROR_STOP=1", "-d", target],
                   input=statement, text=True, check=True, timeout=30)

sql(admin, "CREATE DATABASE " + name)
try:
    for migration in sorted((Path(__file__).parents[1] / "db/migrations").glob("*.sql")):
        sql(dsn, migration.read_text())
    sql(dsn, """
CREATE TEMP TABLE synthetic_identity AS SELECT i.id AS iid,p.id AS pid FROM macro_instruments i CROSS JOIN macro_providers p LIMIT 1;
INSERT INTO macro_reference_values(instrument_id,provider_id,event_time,publication_time,value,quality_status)
 SELECT iid,pid,'2026-01-01',NULL,1,'VALID' FROM synthetic_identity;
DO $$ BEGIN
 ASSERT (SELECT count(*)=1 FROM macro_reference_revisions);
 ASSERT (SELECT available_at>=recorded_at AND timestamp_granularity='RECEIPT_BOUND' FROM macro_reference_revisions);
END $$;
CREATE TEMP TABLE before_revision AS SELECT max(available_at) AS cutoff FROM macro_reference_revisions;
UPDATE macro_reference_values SET value=1,revision=revision+1,updated_at=now();
DO $$ BEGIN ASSERT (SELECT count(*)=1 FROM macro_reference_revisions); END $$;
SELECT pg_sleep(0.01);
UPDATE macro_reference_values SET value=2,publication_time=now()+interval '1 day',revision=revision+1;
DO $$ BEGIN
 ASSERT (SELECT count(*)=2 FROM macro_reference_revisions);
 ASSERT (SELECT value=1 FROM macro_reference_revisions,before_revision WHERE available_at<=cutoff ORDER BY available_at DESC,id DESC LIMIT 1);
 ASSERT (SELECT value=2 AND available_at>=publication_time FROM macro_reference_revisions ORDER BY id DESC LIMIT 1);
 BEGIN UPDATE macro_reference_revisions SET value=999; RAISE EXCEPTION 'mutation accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM NOT LIKE '%immutable%' THEN RAISE; END IF;
 END;
 BEGIN DELETE FROM macro_reference_revisions; RAISE EXCEPTION 'delete accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM NOT LIKE '%immutable%' THEN RAISE; END IF;
 END;
END $$;
""")
    print("PASS real owner migrations, genuine change dedupe, causal prior revision, future publication, immutability")
finally:
    sql(admin, "DROP DATABASE " + name + " WITH (FORCE)")
