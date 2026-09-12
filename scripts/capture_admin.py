#!/usr/bin/env python3
"""The dedicated server's administration, asked of a server and written down.

The same script is pointed at the real 1.20.1 jar and at our `ov_dedicated`,
each started in a fresh directory holding the same `server.properties`, the
same list files written in vanilla's format and the same `server-icon.png`:

  properties  the jar alone, in a directory whose server.properties has two
              lines: which file does its first start write? (key order,
              defaults, header, an unknown key)
  vanilla     the jar, the whole round below
  ov          our server, the same round

The round, in order:

  status      Status Response JSON (MOTD, icon, player count)
  query       GameSpy4 over UDP: handshake, basic stat, full stat, a bad token
  rcon        TCP: a wrong password, the right one, commands, a long answer
              split over several packets, a request split over two writes
  logins      one fresh connection per case, the first packet it is sent:
              a ban with an expiry read from a pre-written file, an expired
              one, a ban by the console, the whitelist, an IP ban, a full
              server, a duplicate login
  commands    an operator probe types administration commands through a real
              Chat Command packet; the replies are recorded byte for byte
  console     the same kind of commands typed on the console
  files       ops.json, whitelist.json, banned-*.json, server.properties as
              the server left them after `stop`
  shutdown    what a connected player is sent by `stop`

Every client here is written from the protocol pages (wiki.vg archive,
`RCON`, `Query`), not from our code. `scripts/check_admin.py` compares.

Usage:
  python3 scripts/capture_admin.py properties|properties-ov [out.json]
  python3 scripts/capture_admin.py vanilla|ov [out.json]
Ports: OV_ADMIN_PORT (default 25610); rcon +1, query +2.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_commands import CommandProbe, decode as decode_chat  # noqa: E402
from capture_entity_packets import read_varint, varint  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
JAR = ROOT / "tools" / "vanilla" / "server.jar"
SCRATCH = ROOT / ".scratch" / "admin"
LINE = re.compile(r"^\[[0-9:.]+\] \[[^\]]+\](?: \[[^\]]+\])?:? (.*)$")

PORT = int(os.environ.get("OV_ADMIN_PORT", "25610"))
# Inside the two ports this measurement owns (PORT and PORT + 100): RCON on
# the second one over TCP, Query on the first one over UDP — a separate
# namespace, and vanilla's own default for query.port is the server port.
# PORT + 1 was another campaign's, and the jar died on "Address already in use".
RCON_PORT = PORT + 100
QUERY_PORT = PORT
RCON_PASSWORD = "ovsecret"

# Names that no Mojang account is likely to hold: an offline jar still asks
# the profile API about a name it does not know, and a real account would
# come back with a real uuid.
BANNED_TEMP = "Ovq_tempban"
BANNED_OLD = "Ovq_oldban"
WHITELISTED = "Ovq_wally"


def offline_uuid(name: str) -> str:
    """UUID.nameUUIDFromBytes("OfflinePlayer:" + name): MD5, version 3."""
    digest = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    h = digest.hex()
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"


PROPERTIES = (
    "#Written by scripts/capture_admin.py\n"
    f"server-port={PORT}\n"
    "online-mode=false\n"
    "level-type=minecraft\\:flat\n"
    "generate-structures=false\n"
    "max-players=3\n"
    "motd=Ondes admin \\u00e9 probe\n"
    "enable-rcon=true\n"
    f"rcon.port={RCON_PORT}\n"
    f"rcon.password={RCON_PASSWORD}\n"
    "enable-query=true\n"
    f"query.port={QUERY_PORT}\n"
    "spawn-protection=0\n"
    "view-distance=4\n"
    "simulation-distance=4\n"
    "sync-chunk-writes=true\n"
    "ov-unknown-key=kept\n"
)

PREWRITTEN = {
    "banned-players.json": [
        {"uuid": offline_uuid(BANNED_TEMP), "name": BANNED_TEMP,
         "created": "2026-09-11 10:00:00 +0200", "source": "Server",
         "expires": "2099-01-02 03:04:05 +0000", "reason": "Testing expiry"},
        {"uuid": offline_uuid(BANNED_OLD), "name": BANNED_OLD,
         "created": "2000-01-01 00:00:00 +0000", "source": "Server",
         "expires": "2001-01-01 00:00:00 +0000", "reason": "Long gone"},
    ],
    "banned-ips.json": [
        {"ip": "10.9.9.9", "created": "2026-09-11 10:00:00 +0200", "source": "Server",
         "expires": "forever", "reason": "Banned by an operator."},
    ],
    "whitelist.json": [
        {"uuid": offline_uuid(WHITELISTED), "name": WHITELISTED},
    ],
}


def icon_png() -> bytes:
    """A 64x64 RGBA PNG, built here so no image is ever committed."""
    rows = b""
    for y in range(64):
        rows += b"\x00" + b"".join(bytes([x * 4, y * 4, (x ^ y) * 4 & 0xFF, 255]) for x in range(64))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


# ── Servers ──────────────────────────────────────────────────────────────────

class Process:
    """A server driven through its console, its lines collected."""

    def __init__(self, argv: list[str], directory: Path) -> None:
        self.process = subprocess.Popen(argv, cwd=directory, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, bufsize=1)
        self.lines: queue.Queue[str] = queue.Queue()
        self.all: list[str] = []
        threading.Thread(target=self._pump, daemon=True).start()
        self._token = 0

    def _pump(self) -> None:
        assert self.process.stdout is not None
        for raw in self.process.stdout:
            line = raw.rstrip("\n")
            self.all.append(line)
            self.lines.put(line)

    def await_line(self, needle: str, timeout: float) -> list[str]:
        seen: list[str] = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                if self.process.poll() is not None:
                    break
                continue
            seen.append(line)
            if needle in line:
                return seen
        raise TimeoutError(f"never saw {needle!r}; last lines: {seen[-8:]}")

    def write(self, text: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def batch(self, commands: list[str], timeout: float = 30.0) -> list[str]:
        """Run console commands; return what was printed before a marker."""
        self._token += 1
        marker = f"ovsync{self._token}"
        for command in commands:
            self.write(command)
        self.write(f"say {marker}")
        seen = self.await_line(f"[Server] {marker}", timeout)
        return [strip(line) for line in seen[:-1]]

    def stop(self, timeout: float = 60.0) -> int:
        try:
            self.write("stop")
        except (BrokenPipeError, ValueError):
            pass
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            return -9


def strip(line: str) -> str:
    match = LINE.match(line)
    return match.group(1) if match else line


def start(target: str, directory: Path) -> Process:
    if target == "ov":
        server = Process([str(BINARY), "--log-level=info"], directory)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                    break
            except OSError:
                if server.process.poll() is not None:
                    raise RuntimeError("ov_dedicated exited: " + "\n".join(server.all[-10:]))
                time.sleep(0.1)
        time.sleep(1.0)
        return server
    shutil.copy(JAR, directory / "server.jar")
    (directory / "eula.txt").write_text("eula=true\n")
    server = Process(["java", "-Xmx1G", "-jar", "server.jar", "nogui"], directory)
    server.await_line("Done (", timeout=240)
    # "Done" comes before the jar has finished starting: Query and RCON come
    # up after it, and a status ping sent at once was closed unanswered. The
    # last line of the start, then a second, as for our own server.
    server.await_line("RCON running on", timeout=30)
    time.sleep(1.0)
    return server


def prepare(directory: Path) -> None:
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    (directory / "server.properties").write_text(PROPERTIES)
    for name, content in PREWRITTEN.items():
        (directory / name).write_text(json.dumps(content, indent=2))
    (directory / "server-icon.png").write_bytes(icon_png())


# ── Status ───────────────────────────────────────────────────────────────────

def read_packet(sock: socket.socket, threshold: int | None = None) -> tuple[int, bytes]:
    def exact(n: int) -> bytes:
        out = b""
        while len(out) < n:
            chunk = sock.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out

    length, shift = 0, 0
    while True:
        byte = exact(1)[0]
        length |= (byte & 0x7F) << shift
        if not byte & 0x80:
            break
        shift += 7
    data = exact(length)
    if threshold is not None:
        size, i = read_varint(data, 0)
        data = data[i:] if size == 0 else zlib.decompress(data[i:])
    pid, i = read_varint(data, 0)
    return pid, data[i:]


def send_packet(sock: socket.socket, pid: int, payload: bytes) -> None:
    body = varint(pid) + payload
    sock.sendall(varint(len(body)) + body)


def handshake(sock: socket.socket, next_state: int) -> None:
    host = b"127.0.0.1"
    send_packet(sock, 0x00, varint(763) + varint(len(host)) + host + struct.pack(">H", PORT)
                + varint(next_state))


def status() -> dict:
    """The Status Response, asked up to three times: a server that closes the
    first ping unanswered is still starting, not refusing."""
    for attempt in range(3):
        try:
            return status_once()
        except (EOFError, ConnectionError):
            if attempt == 2:
                raise
            time.sleep(0.5)
    raise AssertionError("unreachable")


def status_once() -> dict:
    with socket.create_connection(("127.0.0.1", PORT), timeout=10) as sock:
        handshake(sock, 1)
        send_packet(sock, 0x00, b"")
        pid, payload = read_packet(sock)
        text, _ = read_string(payload, 0)
        send_packet(sock, 0x01, struct.pack(">q", 0x0102030405060708))
        pong_id, pong = read_packet(sock)
    return {"id": pid, "json": text, "pong": [pong_id, pong.hex()]}


def read_string(buf: bytes, i: int) -> tuple[str, int]:
    length, i = read_varint(buf, i)
    return buf[i:i + length].decode("utf-8"), i + length


# ── Login ────────────────────────────────────────────────────────────────────

def login_attempt(name: str, keep: float = 0.0) -> dict:
    """The first answer to a Login Start: Disconnect, or the door opening."""
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=15)
    try:
        handshake(sock, 2)
        send_packet(sock, 0x00, varint(len(name.encode())) + name.encode() + bytes([0]))
        threshold = None
        while True:
            pid, payload = read_packet(sock, threshold)
            if pid == 0x00:
                text, _ = read_string(payload, 0)
                return {"name": name, "result": "disconnect", "reason": text}
            if pid == 0x03 and threshold is None:
                threshold, _ = read_varint(payload, 0)
                continue
            if pid == 0x02:
                result = {"name": name, "result": "success"}
                if keep:
                    # In Play: the first kick, if any, is recorded.
                    deadline = time.monotonic() + keep
                    while time.monotonic() < deadline:
                        sock.settimeout(max(0.05, deadline - time.monotonic()))
                        try:
                            pid, payload = read_packet(sock, threshold)
                        except (socket.timeout, TimeoutError):
                            break
                        if pid == 0x1A:
                            result["kicked"], _ = read_string(payload, 0)
                            break
                        if pid == 0x23:
                            body = varint(0x12) + payload[:8]
                            inner = varint(0) + body
                            sock.sendall(varint(len(inner)) + inner)
                return result
    except (EOFError, ConnectionError, socket.timeout) as error:
        return {"name": name, "result": f"closed: {type(error).__name__}"}
    finally:
        sock.close()


# ── Query (GameSpy4, `Query` page) ───────────────────────────────────────────

def query() -> dict:
    out: dict = {}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3)
    session = 0x01020304 & 0x0F0F0F0F
    try:
        sock.sendto(b"\xFE\xFD\x09" + struct.pack(">i", session), ("127.0.0.1", QUERY_PORT))
        reply, _ = sock.recvfrom(4096)
        out["handshake"] = reply.hex()
        token = int(reply[5:].rstrip(b"\x00").decode())
        sock.sendto(b"\xFE\xFD\x00" + struct.pack(">ii", session, token), ("127.0.0.1", QUERY_PORT))
        basic, _ = sock.recvfrom(4096)
        out["basic"] = basic.hex()
        sock.sendto(b"\xFE\xFD\x00" + struct.pack(">ii", session, token) + bytes(4),
                    ("127.0.0.1", QUERY_PORT))
        full, _ = sock.recvfrom(4096)
        out["full"] = full.hex()
        sock.sendto(b"\xFE\xFD\x00" + struct.pack(">ii", session, token + 1),
                    ("127.0.0.1", QUERY_PORT))
        try:
            bad, _ = sock.recvfrom(4096)
            out["bad_token"] = bad.hex()
        except (socket.timeout, TimeoutError):
            out["bad_token"] = None
    except (socket.timeout, TimeoutError, ValueError) as error:
        out["error"] = f"{type(error).__name__}: {error}"
    finally:
        sock.close()
    return out


# ── RCON (`RCON` page) ───────────────────────────────────────────────────────

class Rcon:
    def __init__(self) -> None:
        self.sock = socket.create_connection(("127.0.0.1", RCON_PORT), timeout=10)
        self.buffer = b""

    @staticmethod
    def packet(request: int, kind: int, body: str) -> bytes:
        payload = struct.pack("<ii", request, kind) + body.encode("utf-8") + b"\x00\x00"
        return struct.pack("<i", len(payload)) + payload

    def send(self, request: int, kind: int, body: str, split: bool = False) -> None:
        data = self.packet(request, kind, body)
        if split:
            self.sock.sendall(data[:6])
            time.sleep(0.3)
            self.sock.sendall(data[6:])
        else:
            self.sock.sendall(data)

    def read(self, timeout: float = 5.0) -> dict | None:
        self.sock.settimeout(timeout)
        try:
            while len(self.buffer) < 4:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return {"closed": True}
                self.buffer += chunk
            (length,) = struct.unpack_from("<i", self.buffer, 0)
            while len(self.buffer) < 4 + length:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return {"closed": True}
                self.buffer += chunk
        except (socket.timeout, TimeoutError):
            return None
        raw = self.buffer[:4 + length]
        self.buffer = self.buffer[4 + length:]
        request, kind = struct.unpack_from("<ii", raw, 4)
        body = raw[12:4 + length]
        return {"length": length, "request": request, "type": kind,
                "body": body[:-2].decode("utf-8", "replace"), "tail": body[-2:].hex()}

    def read_all(self, quiet: float = 1.0) -> list[dict]:
        out = []
        first = self.read(5.0)
        while first is not None:
            out.append(first)
            if first.get("closed"):
                break
            first = self.read(quiet)
        return out

    def close(self) -> None:
        self.sock.close()


RCON_COMMANDS = ["list", "seed", "", "nosuchcommand", "time query daytime", "banlist ips",
                 "help", "say from rcon", "whitelist list", "list uuids"]


def rcon_round() -> dict:
    out: dict = {}
    try:
        bad = Rcon()
        bad.send(7, 3, "wrong")
        out["bad_auth"] = bad.read_all(1.0)
        bad.close()

        good = Rcon()
        good.send(11, 3, RCON_PASSWORD)
        out["auth"] = good.read_all(0.5)
        out["commands"] = []
        for i, command in enumerate(RCON_COMMANDS, start=100):
            good.send(i, 2, command)
            out["commands"].append({"command": command, "packets": good.read_all(1.0)})
        good.send(201, 99, "seed")
        out["unknown_type"] = good.read_all(1.0)
        good.close()

        # A request cut in two TCP writes, on a connection of its own: the jar
        # answered nothing to it, and anything asked after it on the same
        # connection went unanswered too.
        split = Rcon()
        split.send(12, 3, RCON_PASSWORD)
        out["split_auth"] = split.read_all(0.5)
        split.send(200, 2, "seed", split=True)
        out["split"] = split.read_all(1.5)
        split.close()

        unauth = Rcon()
        unauth.send(5, 2, "seed")
        out["unauthenticated_command"] = unauth.read_all(1.0)
        unauth.close()
    except (ConnectionError, OSError) as error:
        out["error"] = f"{type(error).__name__}: {error}"
    return out


# ── Commands ─────────────────────────────────────────────────────────────────

# As an operator, through Chat Command. Names that exist nowhere, for the
# reason given at BANNED_TEMP.
COMMANDS: list[str] = [
    "banlist", "banlist players", "banlist ips",
    "ban Ovq_alice", "ban Ovq_alice", "ban Ovq_bobby Griefing the spawn", "ban",
    "banlist", "banlist players",
    "pardon Ovq_alice", "pardon Ovq_alice", "pardon Ovq_nobody",
    "ban-ip 10.0.0.1", "ban-ip 10.0.0.1", "ban-ip 10.0.0.2 Spamming", "ban-ip not.an.ip",
    "ban-ip 999.1.1.1", "banlist ips", "pardon-ip 10.0.0.1", "pardon-ip 10.0.0.1",
    "pardon-ip bad", "pardon-ip 10.0.0.2",
    "whitelist list", "whitelist add Ovq_carol", "whitelist add Ovq_carol", "whitelist list",
    "whitelist remove Ovq_carol", "whitelist remove Ovq_carol", "whitelist on", "whitelist on",
    "whitelist off", "whitelist off", "whitelist reload", "whitelist add ovprobe",
    "whitelist list", "whitelist remove ovprobe", "whitelist",
    "op Ovq_dave", "op Ovq_dave", "deop Ovq_dave", "deop Ovq_dave",
    "setidletimeout 0", "setidletimeout 10", "setidletimeout -1", "setidletimeout 0",
    "save-off", "save-off", "save-on", "save-on", "save-all", "save-all flush",
    "list", "list uuids",
    "defaultgamemode creative", "defaultgamemode creative", "defaultgamemode survival",
    "seed", "debug start", "debug start", "debug stop", "debug stop", "publish",
    "kick Ovq_nobody", "banlist players",
]

PROFILE_COMMANDS = {"ban", "pardon", "op", "deop", "whitelist"}

CONSOLE: list[str] = [
    "banlist", "ban Ovq_console Console ban", "banlist players", "pardon Ovq_console",
    "ban-ip 10.0.0.3", "pardon-ip 10.0.0.3", "whitelist add Ovq_erin", "whitelist list",
    "whitelist remove Ovq_erin", "op Ovq_frank", "deop Ovq_frank", "list", "list uuids",
    "save-off", "save-on", "save-all", "setidletimeout 0", "defaultgamemode survival",
    "debug start", "debug stop", "publish", "kick Ovq_nobody", "whitelist reload",
    # Which of these does the server write into server.properties? Read in
    # `files` after the stop.
    "defaultgamemode adventure", "difficulty hard", "setidletimeout 7",
]


def is_sync(entry: dict) -> bool:
    return "ovsync" in json.dumps(entry.get("decoded") or {})


def run_round(target: str, directory: Path) -> dict:
    document: dict = {"target": target}
    server = start(target, directory)
    try:
        document["status"] = status()
        document["query"] = query()
        document["rcon"] = rcon_round()
        print("status, query, rcon: done")

        logins = []
        logins.append(login_attempt(BANNED_TEMP))
        logins.append(login_attempt(BANNED_OLD))
        server.batch([f"ban Ovq_banned Being banned", "ban Ovq_banned2"])
        logins.append(login_attempt("Ovq_banned"))
        logins.append(login_attempt("Ovq_banned2"))
        server.batch(["whitelist on"])
        logins.append(login_attempt("Ovq_stranger"))
        logins.append(login_attempt(WHITELISTED))
        server.batch(["whitelist off", "ban-ip 127.0.0.1 Local ban"])
        logins.append(login_attempt("Ovq_anyone"))
        server.batch(["pardon-ip 127.0.0.1", "ban-ip 127.0.0.2"])
        logins.append(login_attempt("Ovq_anyone2"))
        server.batch(["pardon-ip 127.0.0.2"])
        document["logins"] = logins
        print(f"logins: {[entry['result'] for entry in logins]}")

        probe = CommandProbe(PORT)
        probe.pump(3.0)
        probe.drain()
        server.batch(["op ovprobe"])
        probe.pump(1.0)
        probe.take()
        document["commands"] = []
        for text in COMMANDS:
            probe.command(text)
            # A name the jar does not know sends it to Mojang's profile API
            # before it answers: a longer window, or its reply lands in the
            # next command's.
            words = text.split(" ")
            probe.pump(3.0 if words[0] in PROFILE_COMMANDS and len(words) > 1 else 0.7)
            packets = [e for e in probe.take() if not is_sync(e)]
            document["commands"].append({"command": text, "packets": packets})
        print(f"commands: {len(COMMANDS)}")

        document["console"] = []
        for text in CONSOLE:
            lines = server.batch([text])
            probe.pump(0.3)
            packets = [e for e in probe.take() if not is_sync(e)]
            document["console"].append({"command": text, "lines": lines, "packets": packets})
        print(f"console: {len(CONSOLE)}")

        # A duplicate login, with a slot free: the jar checks the player
        # limit before it looks for the same profile, so on a full server
        # this would only ever say "full". The one already on is told why
        # it goes.
        first = CommandProbe(PORT, "Ovq_twin")
        first.pump(0.5)
        document["duplicate"] = login_attempt("Ovq_twin", keep=2.0)
        try:
            first.pump(1.5)
        except (EOFError, ConnectionError):
            pass
        document["duplicate_old"] = [e for e in first.take() if e["id"] == 0x1A]
        try:
            first.socket.close()
        except OSError:
            pass
        time.sleep(1.0)  # both twins gone from the player list

        # A full server: ovprobe is in; two more fill max-players=3.
        extra = [CommandProbe(PORT, "Ovq_fill1"), CommandProbe(PORT, "Ovq_fill2")]
        for p in extra:
            p.pump(0.5)
        document["full"] = login_attempt("Ovq_fill3")
        for p in extra:
            try:
                p.socket.close()
            except OSError:
                pass
        time.sleep(1.0)

        # An IP ban by name kicks the named player.
        probe.command("ban-ip ovprobe Kicked by ip")
        try:
            probe.pump(1.5)
        except (EOFError, ConnectionError):
            pass
        document["ip_ban_kick"] = [e for e in probe.take() if e["id"] in (0x1A, 0x64)]
        server.batch(["pardon-ip 127.0.0.1"])

        # The server stops with a player on it.
        last = CommandProbe(PORT, "Ovq_last")
        last.pump(1.0)
        last.drain()
        code = server.stop()
        try:
            last.pump(3.0)
        except (EOFError, ConnectionError):
            pass
        document["shutdown"] = {"exit": code,
                                "packets": [e for e in last.take() if e["id"] == 0x1A]}
    except Exception as error:  # noqa: BLE001 — the partial round is still evidence
        # Kept, with the server's log, rather than lost with the traceback:
        # the log says why a login was refused.
        document["error"] = f"{type(error).__name__}: {error}"
        document["traceback"] = traceback.format_exc()
        print(f"round stopped: {document['error']}")
        print(document["traceback"])
    finally:
        if server.process.poll() is None:
            server.stop()
        document["log"] = [strip(line) for line in server.all]

    document["files"] = {}
    for name in ["ops.json", "whitelist.json", "banned-players.json", "banned-ips.json",
                 "server.properties", "usercache.json"]:
        path = directory / name
        document["files"][name] = path.read_text() if path.exists() else None
    return document


def properties_round(directory: Path, target: str = "vanilla") -> dict:
    """A first start: what server.properties does the server write?

    The jar is stopped once it prepares the level; ours writes the file and
    then refuses to start, since the file (which exists) leaves online-mode
    at vanilla's default, true — which is itself part of the measure."""
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    before = f"server-port={PORT}\nov-unknown-key=kept\nmotd=Caf\\u00e9 \\: \\= x\n"
    (directory / "server.properties").write_text(before)
    if target == "ov":
        server = Process([str(BINARY), "--log-level=info"], directory)
        try:
            server.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            server.process.kill()
            server.process.wait()
    else:
        shutil.copy(JAR, directory / "server.jar")
        (directory / "eula.txt").write_text("eula=true\n")
        server = Process(["java", "-Xmx1G", "-jar", "server.jar", "nogui"], directory)
        try:
            server.await_line("Preparing level", timeout=240)
        finally:
            server.process.kill()
            server.process.wait()
    time.sleep(0.2)
    files = {}
    for path in sorted(directory.iterdir()):
        if path.is_file() and path.suffix in (".properties", ".json"):
            files[path.name] = path.read_text()
    shutil.rmtree(directory, ignore_errors=True)
    return {"before": before, "files": files, "log": [strip(line) for line in server.all]}


def main() -> int:
    target = sys.argv[1] if len(sys.argv) > 1 else "vanilla"
    SCRATCH.mkdir(parents=True, exist_ok=True)
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else SCRATCH / f"admin_{target}.json"
    directory = SCRATCH / f"run-{target}"
    if target == "properties":
        document = properties_round(directory)
    elif target == "properties-ov":
        document = properties_round(directory, "ov")
    else:
        prepare(directory)
        document = run_round(target, directory)
    out.write_text(json.dumps(document, indent=1, sort_keys=True))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
