"""Private JetStream fixture; never contacts a configured production endpoint."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

server = shutil.which("nats-server")
if not server:
    raise RuntimeError("nats-server is required for the input window migration test")
with tempfile.TemporaryDirectory(prefix="altml-window-migration-fixture-") as directory:
    root = Path(directory)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with (root / "nats.log").open("w") as log:
        broker = subprocess.Popen([server, "-js", "-a", "127.0.0.1", "-p", str(port),
                                   "-sd", str(root / "store")], stdout=log, stderr=log)
        try:
            for attempt in range(100):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1):
                        break
                except OSError:
                    assert broker.poll() is None, "private broker exited"
                    time.sleep(.05)
            else:
                raise RuntimeError("private broker unavailable")
            env = os.environ.copy()
            env.pop("JOURNAL_STREAM", None)
            subprocess.run([sys.argv[1], f"nats://127.0.0.1:{port}"], check=True, env=env, timeout=30)
        finally:
            broker.terminate()
            try:
                broker.wait(timeout=5)
            except subprocess.TimeoutExpired:
                broker.kill()
                broker.wait(timeout=5)
