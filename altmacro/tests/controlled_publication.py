"""Real isolated JetStream: no production addresses, accounts or subjects."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

binary = shutil.which("nats-server")
if not binary:
    raise SystemExit(77)
probe = sys.argv[1]

def port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]

with tempfile.TemporaryDirectory(prefix="altmacro-js-") as directory:
    root = Path(directory)
    client_port, monitor_port = port(), port()
    config = root / "nats.json"
    config.write_text(json.dumps({"listen": f"127.0.0.1:{client_port}",
                                  "http": f"127.0.0.1:{monitor_port}",
                                  "jetstream": {"store_dir": str(root / "store")}}))
    url = f"nats://127.0.0.1:{client_port}"
    process = None
    log = (root / "nats.log").open("w")
    def start():
        global process
        process = subprocess.Popen([binary, "-c", str(config)], stdout=log, stderr=log)
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{monitor_port}/healthz", timeout=.2):
                    return
            except OSError:
                time.sleep(.05)
        raise AssertionError("local JetStream did not start")
    def stop():
        if process is not None:
            process.terminate()
            process.wait(timeout=4)
    def count():
        with urllib.request.urlopen(f"http://127.0.0.1:{monitor_port}/jsz?streams=true", timeout=2) as response:
            return json.load(response)["messages"]
    try:
        start()
        subprocess.run([probe, url], check=True, timeout=10)
        assert count() == 3, "value/quality correction suppressed or retry duplicated"
        subprocess.run([probe, url], check=True, timeout=10)
        assert count() == 3
        outage = subprocess.Popen([probe, url, "outage"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            import selectors
            ready = False
            selector = selectors.DefaultSelector()
            selector.register(outage.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + 5
            ready_output = b""
            while time.monotonic() < deadline and not ready:
                if selector.select(.2):
                    chunk = os.read(outage.stdout.fileno(), 8192)
                    if not chunk:
                        break
                    ready_output += chunk
                    ready = b"READY" in ready_output.splitlines()
            selector.close()
            assert ready, ("publisher fixture did not become ready", ready_output.decode(errors="replace"))
            stop()
            stdout, stderr = outage.communicate("publish\n", timeout=12)
            assert outage.returncode == 0, (stdout, stderr)
        finally:
            if outage.poll() is None:
                outage.kill(); outage.wait()
        start()
        subprocess.run([probe, url, "new"], check=True, timeout=10)
        assert count() == 4, "outage/restart changed identity or lost a correction"
        print("PASS exact retry, corrected value/quality, producer/broker restart and outage recovery")
    finally:
        stop()
        log.close()
