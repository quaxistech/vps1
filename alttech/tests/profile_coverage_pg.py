"""Exercise actual profile coverage SQL against real AltSpotter migrations."""
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import urlparse
import uuid

admin = os.environ.get("ALTTECH_TEST_PG_DSN", "")
producer = Path(__file__).parents[2] / "altspotter/db/migrations/0001_schema.sql"
if not admin or not shutil.which("psql") or not producer.exists():
    raise SystemExit(77)
uri = urlparse(admin)
assert uri.scheme == "postgresql" and uri.hostname in (None, "localhost", "127.0.0.1") and uri.password is None
name = "alttech_profile_test_" + uuid.uuid4().hex
dsn = admin + ("&" if "?" in admin else "?") + "dbname=" + name

def sql(target, statement):
    result = subprocess.run(["psql", "-Xq", "-v", "ON_ERROR_STOP=1", "-d", target],
                            input=statement, text=True, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stderr)

source = (Path(__file__).parents[1] / "src/modules/structure/structure_module.cpp").read_text()
start = source.index('"WITH candles AS (SELECT count(*) AS n')
fragment = source[start:source.index("iid, from_ms, to_ms", start)]
proof = "".join(ast.literal_eval(m.group()) for m in re.finditer(r'"(?:[^"\\]|\\.)*"', fragment))
sql(admin, "CREATE DATABASE " + name)
try:
    sql(dsn, producer.read_text())
    sql(dsn, "PREPARE profile_proof(bigint,bigint,bigint) AS " + proof + ";\n" + """
SELECT altspotter_ensure_time_partitions('market_candles_1m','month',now()-interval '1 day',now()+interval '1 day');
CREATE TEMP TABLE synthetic_clock AS SELECT date_trunc('minute',now())-interval '10 minutes' AS start_at;
INSERT INTO market_candles_1m(exchange_id,instrument_id,open_time,close_time,open,high,low,close,base_volume,quote_volume,source)
 SELECT 1,9990,start_at+make_interval(mins=>n),start_at+make_interval(mins=>n+1),10,10.5,9.5,10,10,100,'ws'
 FROM synthetic_clock CROSS JOIN generate_series(0,1) n;
INSERT INTO market_trades_raw(exchange_id,instrument_id,trade_time,trade_id,price,size,side)
 SELECT 1,9990,start_at+make_interval(secs=>n*30), 'synthetic-'||n,10,5,'buy'
 FROM synthetic_clock CROSS JOIN generate_series(0,3) n;
CREATE FUNCTION pg_temp.proof() RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; yes boolean;
BEGIN
 SELECT (extract(epoch FROM start_at)*1000)::bigint,(extract(epoch FROM start_at+interval '2 minutes')*1000)::bigint INTO a,b FROM synthetic_clock;
 EXECUTE format('EXECUTE profile_proof(9990,%s,%s)',a,b) INTO yes;
 RETURN COALESCE(yes,false);
END $$;
DO $$ BEGIN ASSERT pg_temp.proof(); END $$;
UPDATE market_trades_raw SET size=4.9 WHERE trade_id='synthetic-0';
DO $$ BEGIN ASSERT NOT pg_temp.proof(); END $$; -- less than1percent missing is still incomplete
UPDATE market_trades_raw SET size=5 WHERE trade_id='synthetic-0';
UPDATE market_candles_1m SET quality_status='incomplete' WHERE open_time=(SELECT start_at FROM synthetic_clock);
DO $$ BEGIN ASSERT NOT pg_temp.proof(); END $$;
UPDATE market_candles_1m SET quality_status='ok';
UPDATE market_trades_raw SET price=CASE WHEN trade_id IN ('synthetic-0','synthetic-2') THEN 9 ELSE 11 END;
DO $$ BEGIN ASSERT NOT pg_temp.proof(); END $$; -- same exact totals, invalid price coverage
UPDATE market_trades_raw SET price=10;
INSERT INTO market_trades_raw(exchange_id,instrument_id,trade_time,trade_id,price,size,side)
 SELECT 1,9990,start_at+interval '3 minutes','synthetic-future',100000,1000,'buy' FROM synthetic_clock;
DO $$ BEGIN ASSERT pg_temp.proof(); END $$;
""")
    print("PASS real source schema, exact base/quote/minute coverage, missing tape, bad candle, price bounds, future exclusion")
finally:
    sql(admin, "DROP DATABASE " + name + " WITH (FORCE)")
