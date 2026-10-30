"""Bounded, loopback-only fault/restart test of the real inference component."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

dsn = os.environ.get("ALTML_TEST_PG_DSN")
server = shutil.which("nats-server")
if not dsn or not server:
    raise SystemExit(77)
probe = sys.argv[1]
for mode in ("absent", "model"):
    with tempfile.TemporaryDirectory(prefix="altml-consumer-fixture-") as directory:
        root = Path(directory)
        with socket.socket() as port_socket:
            port_socket.bind(("127.0.0.1", 0))
            port = port_socket.getsockname()[1]
        with (root / "nats.log").open("w") as log:
            command = [server, "-js", "-a", "127.0.0.1", "-p", str(port),
                       "-sd", str(root / "store")]
            broker = subprocess.Popen(command, stdout=log, stderr=log)

            def ready():
                for attempt in range(100):
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=.1):
                            break
                    except OSError:
                        assert broker.poll() is None, "fixture broker exited"
                        time.sleep(.05)
                else:
                    raise RuntimeError("fixture broker unavailable")

            try:
                ready()
                for phase in ("fail", "recover"):
                    probe_env = os.environ.copy()
                    # The test asserts actual model activation from stdout. A
                    # systemd scope otherwise routes logs directly to journald.
                    probe_env.pop("JOURNAL_STREAM", None)
                    result = subprocess.run([probe, dsn, f"nats://127.0.0.1:{port}", phase,
                                             mode, directory], text=True, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, timeout=40, env=probe_env)
                    print(result.stdout, end="")
                    assert result.returncode == 0, f"{mode}/{phase} failed"
                    if mode == "model":
                        assert 'model_activated_in_process' in result.stdout, "fixture model was not loaded"
                    if phase == "fail":
                        broker.terminate()
                        broker.wait(timeout=5)
                        broker = subprocess.Popen(command, stdout=log, stderr=log)
                        ready()
            finally:
                broker.terminate()
                try:
                    broker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    broker.kill()
                    broker.wait(timeout=5)
