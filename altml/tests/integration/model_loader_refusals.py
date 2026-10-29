"""Actual loader rejects incompatible fixture artifacts; no model training."""
import json
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
cases = {
    "path": "model_path_outside_root",
    "schema": "base_projection_unresolved",
    "width": "rebuilt_width_mismatch",
    "checksum": "checksum_mismatch",
    "malformed": "model_load_failed",
    "shape": "booster_shape_mismatch",
    "magnitude-path": "magnitude_path_outside_root",
    "magnitude-checksum": "magnitude_checksum_mismatch",
    "magnitude-shape": "magnitude_shape_mismatch",
}
for case, reason in cases.items():
    with tempfile.TemporaryDirectory(prefix="altml-model-loader-fixture-") as directory:
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
                result = subprocess.run([sys.argv[1], dsn, f"nats://127.0.0.1:{port}",
                                         "model-check-" + case, "model", directory], env=env,
                                        text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=8)
                print(result.stdout, end="")
                assert result.returncode == 0, f"{case}: loader fixture failed"
                events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
                assert any(row.get("event") == "model_refused" and row.get("reason") == reason
                           for row in events), f"{case}: expected loader refusal absent"
                assert not any(row.get("event") == "model_activated_in_process" for row in events), \
                    f"{case}: incompatible model was activated"
                print(f"PASS refused {case}: {reason}")
            finally:
                broker.terminate()
                try:
                    broker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    broker.kill()
                    broker.wait(timeout=5)
