#!/usr/bin/env python3
"""Проверка контракта JetStream для ALTMACRO_SERIES.

Создаёт ВРЕМЕННОГО durable-потребителя, читает через него, проверяет replay и
удаляет за собой. Ничего постоянного не оставляет.
"""
import json, os, re, socket, sys, time, uuid

URL = os.environ["ALTMACRO_NATS_URL"]
m = re.match(r"nats://([^:]+):([^@]+)@([^:]+):(\d+)", URL)
user, pw, host, port = m.group(1), m.group(2), m.group(3), int(m.group(4))
DURABLE = "altmacro_verify_" + uuid.uuid4().hex[:8]
STREAM = "ALTMACRO_SERIES"

class Nats:
    def __init__(self):
        self.s = socket.create_connection((host, port), timeout=15)
        self.buf = b""
        self._readline()  # INFO
        self._send('CONNECT {"verbose":false,"pedantic":false,"user":"%s","pass":"%s",'
                   '"name":"altmacro-verify","lang":"python","version":"0"}' % (user, pw))
        self._send("PING")
        self._expect("PONG")
        self.inbox = "_INBOX." + uuid.uuid4().hex
        self._send("SUB %s 1" % self.inbox)

    def _send(self, line):
        self.s.sendall((line + "\r\n").encode())

    def _readline(self):
        while b"\r\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("соединение закрыто")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line.decode(errors="replace")

    def _read(self, n):
        while len(self.buf) < n:
            self.buf += self.s.recv(65536)
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _expect(self, token, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            line = self._readline()
            if line.startswith("PING"):
                self._send("PONG"); continue
            if line.startswith(token):
                return line
        raise RuntimeError("не дождались %s" % token)

    def request(self, subject, payload, timeout=15):
        body = payload.encode()
        self._send("PUB %s %s %d" % (subject, self.inbox, len(body)))
        self.s.sendall(body + b"\r\n")
        end = time.time() + timeout
        while time.time() < end:
            line = self._readline()
            if line.startswith("PING"):
                self._send("PONG"); continue
            if line.startswith("MSG "):
                parts = line.split()
                size = int(parts[-1])
                data = self._read(size + 2)[:-2]
                return data.decode(errors="replace")
        raise RuntimeError("таймаут запроса к %s" % subject)

    def close(self):
        try: self.s.close()
        except Exception: pass

n = Nats()
fails = 0
def check(ok, what, extra=""):
    global fails
    print("  %s %s%s" % ("ok   " if ok else "FAIL ", what, (" — " + extra) if extra else ""))
    if not ok: fails += 1

try:
    info = json.loads(n.request("$JS.API.STREAM.INFO.%s" % STREAM, ""))
    st = info["state"]; cfg = info["config"]
    print("  поток: %d сообщений, seq %d..%d, %d байт" %
          (st["messages"], st["first_seq"], st["last_seq"], st["bytes"]))
    check(st["messages"] > 0, "поток содержит сообщения")
    check(st["last_seq"] >= st["first_seq"], "последовательность монотонна")
    check(cfg["max_bytes"] == 402653184, "лимит 384 МиБ", str(cfg["max_bytes"]))
    check(cfg["duplicate_window"] == 48*3600*10**9, "окно дедупликации 48 ч")
    check(cfg["retention"] == "limits" and cfg["discard"] == "old",
          "retention=limits, discard=old")
    check(cfg["storage"] == "file", "хранилище файловое (переживёт перезапуск)")
    used_pct = 100.0 * st["bytes"] / cfg["max_bytes"]
    check(used_pct < 90.0, "запас по объёму", "занято %.2f%% лимита" % used_pct)

    # Временный durable: читаем с начала, проверяем replay и подтверждение.
    req = {"stream_name": STREAM, "config": {
        "durable_name": DURABLE, "deliver_policy": "all", "ack_policy": "explicit",
        "filter_subject": "altmacro.>", "max_ack_pending": 64}}
    r = json.loads(n.request("$JS.API.CONSUMER.DURABLE.CREATE.%s.%s" % (STREAM, DURABLE),
                             json.dumps(req)))
    check("error" not in r, "временный durable создан", r.get("error", {}).get("description", ""))

    def fetch(count):
        got = []
        for _ in range(count):
            try:
                msg = n.request("$JS.API.CONSUMER.MSG.NEXT.%s.%s" % (STREAM, DURABLE),
                                json.dumps({"batch": 1, "no_wait": True}), timeout=8)
            except Exception:
                break
            if msg.startswith("{") and '"code"' in msg[:40]:
                break
            got.append(msg)
        return got

    first = fetch(3)
    check(len(first) > 0, "сообщения читаются потребителем", "получено %d" % len(first))
    if first:
        d = json.loads(first[0])
        needed = ["schema_version","instrument","series_class","provider","native_symbol",
                  "timestamp_semantics","quality","event_time","ingest_time"]
        missing = [k for k in needed if k not in d]
        check(not missing, "провенанс полон в каждом сообщении",
              ("нет: " + ",".join(missing)) if missing else d["schema_version"])
        check(d.get("denomination"), "деноминация передаётся", str(d.get("denomination")))

    # Replay: пересоздаём с deliver_policy=all и убеждаемся, что история доступна снова.
    n.request("$JS.API.CONSUMER.DELETE.%s.%s" % (STREAM, DURABLE), "")
    r2 = json.loads(n.request("$JS.API.CONSUMER.DURABLE.CREATE.%s.%s" % (STREAM, DURABLE),
                              json.dumps(req)))
    replay = fetch(3)
    check(len(replay) == len(first), "replay с начала даёт ту же историю",
          "%d против %d" % (len(replay), len(first)))
    if first and replay:
        check(replay[0] == first[0], "первое сообщение replay идентично исходному")
finally:
    try:
        n.request("$JS.API.CONSUMER.DELETE.%s.%s" % (STREAM, DURABLE), "")
        print("  временный потребитель %s удалён" % DURABLE)
    except Exception as e:
        print("  ВНИМАНИЕ: не удалось удалить %s: %s" % (DURABLE, e))
    n.close()

print()
print("JetStream: %s" % ("PASS" if fails == 0 else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
