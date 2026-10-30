"""Exercise the production UPSERT text in an isolated PostgreSQL database."""
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import urlparse
import uuid

admin = os.environ.get("ALTTECH_TEST_PG_DSN", "")
if not admin or not shutil.which("psql"):
    raise SystemExit(77)
uri = urlparse(admin)
assert uri.scheme == "postgresql" and uri.hostname in (None, "localhost", "127.0.0.1")
assert uri.password is None
name = "alttech_availability_test_" + uuid.uuid4().hex

def sql(dsn, text):
    subprocess.run(["psql", "-Xq", "-v", "ON_ERROR_STOP=1", "-d", dsn],
                   input=text, text=True, check=True, timeout=20)

source = (Path(__file__).parents[1] / "src/modules/macro/macro_module.cpp").read_text()
source = re.sub(r"//[^\n]*", "", source)
start = source.index('"INSERT INTO alttech.macro_feature_vectors ')
fragment = source[start:source.index("v.t_ms, set_id", start)]
upsert = "".join(ast.literal_eval(match.group()) for match in re.finditer(r'"(?:[^"\\]|\\.)*"', fragment))
prepare = "PREPARE write_vector(bigint,bigint,text,text,bytea,text,integer) AS " + upsert + ";\n"
vector = "EXECUTE write_vector(1800000000000,1,'v2','{1,2}','\\x03','VALID',2);\n"
dsn = admin + ("&" if "?" in admin else "?") + "dbname=" + name
sql(admin, "CREATE DATABASE " + name + ";")
try:
    sql(dsn, """
CREATE SCHEMA alttech;
CREATE TABLE alttech.macro_feature_vectors (
 timeframe text,bar_open_time timestamptz,feature_set_version_id bigint,method_version text,
 values real[],validity_bitmap bytea,status text,valid_count integer,
 computed_at timestamptz DEFAULT now(),updated_at timestamptz DEFAULT now(),
 PRIMARY KEY(timeframe,bar_open_time,feature_set_version_id));
INSERT INTO alttech.macro_feature_vectors VALUES
 ('1m',to_timestamp(1800000000),1,'v1','{1,2}','\\x03','VALID',2,now()-interval '1 hour',now());
CREATE TABLE checkpoints AS SELECT computed_at AS old_time FROM alttech.macro_feature_vectors;
""" + prepare + vector + """
DO $$ BEGIN
 ASSERT (SELECT method_version = 'v2' AND values = ARRAY[1,2]::real[] AND computed_at > old_time
         FROM alttech.macro_feature_vectors,checkpoints);
 ASSERT NOT EXISTS (SELECT 1 FROM alttech.macro_feature_vectors,checkpoints WHERE computed_at <= old_time);
END $$;
ALTER TABLE checkpoints ADD COLUMN v2_time timestamptz;
UPDATE checkpoints SET v2_time=(SELECT computed_at FROM alttech.macro_feature_vectors);
""")
    # A new connection models the stateless writer after process restart.
    sql(dsn, prepare + vector + """
DO $$ BEGIN
 ASSERT (SELECT computed_at = v2_time FROM alttech.macro_feature_vectors,checkpoints);
END $$;
SELECT pg_sleep(0.01);
EXECUTE write_vector(1800000000000,1,'v2','{3,4}','\\x03','VALID',2);
DO $$ BEGIN
 ASSERT (SELECT values = ARRAY[3,4]::real[] AND computed_at > v2_time
         FROM alttech.macro_feature_vectors,checkpoints);
END $$;
UPDATE checkpoints SET v2_time=(SELECT computed_at FROM alttech.macro_feature_vectors);
SELECT pg_sleep(0.01);
EXECUTE write_vector(1800000000000,1,'v3','{5,0}','\\x01','PARTIAL',1);
DO $$ BEGIN
 ASSERT (SELECT method_version = 'v3' AND computed_at > v2_time AND feature_set_version_id = 1
         FROM alttech.macro_feature_vectors,checkpoints);
 ASSERT NOT EXISTS (SELECT 1 FROM alttech.macro_feature_vectors,checkpoints
                    WHERE method_version='v3' AND computed_at <= v2_time);
 ASSERT EXISTS (SELECT 1 FROM alttech.macro_feature_vectors WHERE method_version='v3' AND computed_at <= now());
END $$;
""")
    print("PASS method-only and value corrections, same-method correction, restart and cutoff selection")
finally:
    sql(admin, "DROP DATABASE " + name + " WITH (FORCE);")
