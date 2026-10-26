"""Controlled loopback fixtures; never connects to a public provider."""
import contextlib
import json
import os
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

probe, dns_library = sys.argv[1:]


def run(host, port, cert, flag="", extra=None, timeout=6):
    env = dict(os.environ, SSL_CERT_FILE=str(cert), SSL_CERT_DIR=str(cert.parent / "empty-ca"))
    env.update(extra or {})
    start = time.monotonic()
    proc = subprocess.run([probe, host, str(port), "200", "200", flag], env=env,
                          capture_output=True, text=True, timeout=timeout)
    elapsed = time.monotonic() - start
    assert "fixture_secret" not in proc.stdout + proc.stderr
    assert "DO_NOT_LOG" not in proc.stdout + proc.stderr
    assert proc.stdout.strip(), (proc.returncode, proc.stderr)
    return proc, [json.loads(line) for line in proc.stdout.splitlines()], elapsed


@contextlib.contextmanager
def server(cert, key, mode="ok"):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)
    listener.settimeout(0.1)
    stop, accepted = threading.Event(), threading.Event()
    sockets, errors = [], []
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)

    def serve():
        count = 0
        while not stop.is_set():
            try:
                conn, _ = listener.accept()
            except (socket.timeout, OSError):
                continue
            count += 1
            sockets.append(conn)
            accepted.set()
            conn.settimeout(2)
            try:
                if mode == "handshake":
                    stop.wait(5)
                    continue
                conn = ctx.wrap_socket(conn, server_side=True)
                sockets.append(conn)
                if mode == "write":
                    stop.wait(5)
                    continue
                request = b""
                while b"\r\n\r\n" not in request:
                    part = conn.recv(4096)
                    if not part:
                        break
                    request += part
                    assert len(request) < 65536
                if mode in ("read", "partial"):
                    if mode == "partial":
                        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nx")
                    stop.wait(5)
                elif mode == "retry" and count == 1:
                    pass
                elif mode == "body_limit":
                    conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 268435457\r\n\r\n")
                else:
                    keep = mode in ("shutdown", "stale")
                    conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: " +
                                 (b"keep-alive" if keep else b"close") + b"\r\n\r\nok")
                    if mode == "shutdown":
                        stop.wait(5)
            except (ssl.SSLError, ConnectionError, TimeoutError, OSError):
                pass  # certificate refusal / client deadline is expected
            except Exception as error:
                errors.append(error)
            finally:
                conn.close()
    thread = threading.Thread(target=serve)
    thread.start()
    try:
        yield listener.getsockname()[1], accepted
    finally:
        stop.set()
        listener.close()
        for conn in sockets:
            try:
                conn.close()
            except OSError:
                pass
        thread.join(3)
        assert not thread.is_alive(), "fixture worker leaked"
        assert not errors, errors


with tempfile.TemporaryDirectory(prefix="altmacro-https-") as directory:
    root = Path(directory)
    (root / "empty-ca").mkdir()

    def certificate(name, san, days=1):
        key, csr, cert, ext = [root / (name + suffix) for suffix in (".key", ".csr", ".crt", ".ext")]
        ext.write_text("basicConstraints=critical,CA:TRUE\nsubjectAltName=" + san + "\n")
        subprocess.run(["openssl", "req", "-new", "-newkey", "ec", "-pkeyopt",
                        "ec_paramgen_curve:prime256v1", "-nodes", "-subj", "/CN=localhost",
                        "-keyout", str(key), "-out", str(csr)], capture_output=True, check=True)
        subprocess.run(["openssl", "x509", "-req", "-in", str(csr), "-signkey", str(key),
                        "-out", str(cert), "-days", str(days), "-extfile", str(ext)],
                       capture_output=True, check=True)
        return cert, key

    good, key = certificate("good", "DNS:localhost,IP:127.0.0.1")
    wrong, wrong_key = certificate("wrong", "DNS:wrong.example")
    foreign, foreign_key = certificate("foreign", "DNS:localhost")
    expired, expired_key = certificate("expired", "DNS:localhost", -1)
    for host in ("localhost", "127.0.0.1"):
        with server(good, key) as (port, _):
            proc, rows, elapsed = run(host, port, good)
            assert proc.returncode == 0 and rows[0]["status"] == 200, (proc.stdout, proc.stderr)
    for cert, private_key, trust in ((wrong, wrong_key, wrong),
                                     (foreign, foreign_key, good), (expired, expired_key, expired)):
        with server(cert, private_key) as (port, _):
            proc, rows, _ = run("localhost", port, trust)
            assert proc.returncode == 1 and rows[0]["code"] == "network_error", rows
    print("PASS trusted DNS/IP SAN; mismatched SAN, untrusted and expired certificates refused")

    for mode, flag in (("handshake", ""), ("write", "large"), ("read", ""), ("partial", "")):
        with server(good, key, mode) as (port, _):
            proc, rows, elapsed = run("127.0.0.1", port, good, flag)
            assert proc.returncode == 1 and rows[0]["code"] == "timeout", (mode, rows)
            assert elapsed < 1.5, (mode, elapsed)
    print("PASS handshake/write/read/partial-body deadlines")

    for mode, flag in (("retry", ""), ("stale", "repeat"), ("shutdown", "")):
        with server(good, key, mode) as (port, _):
            proc, rows, elapsed = run("127.0.0.1", port, good, flag)
            assert proc.returncode == 0 and all(row["ok"] for row in rows), (mode, rows)
            assert elapsed < 2, (mode, elapsed)
    with server(good, key, "body_limit") as (port, _):
        proc, rows, elapsed = run("127.0.0.1", port, good)
        assert proc.returncode == 1 and rows[0]["code"] == "network_error", rows
    print("PASS bounded retry, stale keepalive recovery, shutdown deadline and body limit")

    pid_file = root / "resolver.pid"
    asan = os.environ.get("ALTMACRO_TEST_ASAN_LIBRARY", "")
    preload = (asan + ":" if asan else "") + dns_library
    proc, rows, elapsed = run("localhost", 1, good, extra={"LD_PRELOAD": preload,
                                                       "ALTMACRO_DNS_FIXTURE_PID": str(pid_file)})
    assert proc.returncode == 1 and rows[0]["code"] == "timeout" and elapsed < 1.5, rows
    pid = int(pid_file.read_text())
    try:
        os.kill(pid, 0)
        raise AssertionError("DNS worker survived the request")
    except ProcessLookupError:
        pass
    print("PASS stalled NSS lookup deadline and worker reap")

    # Fill only this loopback listener's SYN/accept backlog: no firewall changes.
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0)); listener.listen(1)
        fillers = []
        try:
            for _ in range(8):
                conn = socket.socket(); conn.settimeout(0.1)
                try:
                    conn.connect(listener.getsockname()); fillers.append(conn)
                except TimeoutError:
                    conn.close(); break
            proc, rows, elapsed = run("127.0.0.1", listener.getsockname()[1], good)
            assert proc.returncode == 1 and rows[0]["code"] == "timeout" and elapsed < 1.5, rows
        finally:
            for conn in fillers:
                conn.close()
    print("PASS stalled TCP connect deadline")

    with server(good, key, "handshake") as (port, accepted):
        env = dict(os.environ, SSL_CERT_FILE=str(good))
        proc = subprocess.Popen([probe, "127.0.0.1", str(port), "5000", "5000"], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert accepted.wait(2)
        start = time.monotonic(); proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=2)
        assert json.loads(stdout)["code"] == "shutdown_requested", (stdout, stderr)
        assert time.monotonic() - start < 1
    print("PASS cooperative shutdown cancellation")
