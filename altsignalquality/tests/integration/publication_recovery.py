"""File-backed, loopback-only evaluator publication failure/recovery test."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

dsn = os.environ.get("ALTSQ_TEST_PG_DSN")
server = shutil.which("nats-server")
if not dsn or not server:
    raise SystemExit(77)
for mode in ("duplicate", "restart"):
    for scored in (("cold", "scored", "fresh") if mode == "restart" else ("cold", "scored")):
        with tempfile.TemporaryDirectory(prefix="altsq-publication-fixture-") as directory:
            root = Path(directory)
            with socket.socket() as sock:
                sock.bind(("127.0.0.1",0)); port = sock.getsockname()[1]
            with (root/"nats.log").open("w") as log:
                command=[server,"-js","-a","127.0.0.1","-p",str(port),"-sd",str(root/"store")]
                broker=subprocess.Popen(command,stdout=log,stderr=log)
                try:
                    for attempt in range(100):
                        try:
                            with socket.create_connection(("127.0.0.1",port),timeout=.1): break
                        except OSError:
                            assert broker.poll() is None
                            time.sleep(.05)
                    else: raise RuntimeError("private broker unavailable")
                    env=os.environ.copy(); env.pop("JOURNAL_STREAM",None)
                    phases=("fail","recover") if mode=="restart" else ("fail",)
                    for phase in phases:
                        result=subprocess.run([sys.argv[1],dsn,f"nats://127.0.0.1:{port}",phase,mode,
                                               scored,directory],env=env,text=True,stdout=subprocess.PIPE,
                                              stderr=subprocess.STDOUT,timeout=55)
                        print(result.stdout,end="")
                        assert result.returncode==0, f"{mode}/{scored}/{phase} failed"
                    evidence = os.environ.get("ALTSQ_TEST_PUBLICATION_EVIDENCE_DIR")
                    if evidence:
                        target = Path(evidence) / f"{mode}-{scored}.bin"
                        assert not target.exists(), "preserve previous publication evidence"
                        shutil.copyfile(root / "publication.bin", target)
                finally:
                    broker.terminate()
                    try: broker.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        broker.kill(); broker.wait(timeout=5)
