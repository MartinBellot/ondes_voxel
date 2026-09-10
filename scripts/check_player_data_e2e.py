#!/usr/bin/env python3
"""Player files, end to end, against our own server.

The unit tests pin the reader and the writer; scripts/measure_player_data.py
holds them against the real server's files. This checks that ov_dedicated
actually writes and reads them — at the disconnect, at the shutdown, at the
join — with a probe client, protocol 763, no screen.

Three runs of the server on one fresh world, and one stranger:

  run 1  creative, `--effect=speed:1:2400,night_vision:0:infinite`. The probe
         fills four slots through Set Creative Mode Slot (hotbar, backpack,
         head, off hand), picks hotbar slot 3, walks to (40.5, 25.5) and
         leaves. world/playerdata/<uuid>.dat must hold all of it, gzip, with
         DataVersion 3465. The server is then stopped.

  run 2  survival, `--effect=hunger:255:40`. The probe joins: it must be sent
         back to (40.5, 25.5), its four stacks, slot 3 and the speed effect
         with *less* than 2400 ticks left. It falls ten blocks (seven points,
         survie.md) and leaves. The file must say Health 13, a food level
         below 20, playerGameType 0 and previousPlayerGameType 1. Stopped.

  run 3  survival, no effect flag: the restart. The first Set Health must be
         exactly run 2's health and food, the effects run 2's durations — no
         time passes offline. Then a disconnect and a reconnect inside the same
         process: the same again.

  stranger  a file of DataVersion 3337 for another name: the login is refused
         with a Login Disconnect, and the file is left byte for byte as it was.

Usage: python3 scripts/check_player_data_e2e.py [path/to/ov_dedicated]
"""
from __future__ import annotations

import gzip
import json
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_entity_packets import read_varint, varint  # noqa: E402
from check_survival import Faller  # noqa: E402
from measure_player_data import flatten, offline_uuid, read_nbt  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WORLD = ROOT / ".scratch" / "pd-e2e"
PORT = 25641
NAME = "ovfaller"
STRANGER = "ovstranger"

CB_CONTAINER_CONTENT = 0x12
CB_SET_HELD_ITEM = 0x4D
CB_ENTITY_EFFECT = 0x6C
CB_SET_HEALTH = 0x57
CB_SYNC_POSITION = 0x3C
SB_SET_HELD_ITEM = 0x28
SB_CREATIVE_SLOT = 0x2B

REGISTRIES = json.loads((ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" /
                         "registries.json").read_text())
ITEM = {name: e["protocol_id"] for name, e in REGISTRIES["minecraft:item"]["entries"].items()}

# Window slot -> (item, count). Hotbar 36, backpack 20, head 5, off hand 45.
STACKS = {36: ("minecraft:stone", 32), 20: ("minecraft:bread", 7),
          5: ("minecraft:iron_helmet", 1), 45: ("minecraft:torch", 10)}
FILE_SLOT = {36: 0, 20: 20, 5: 103, 45: -106}

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        failures.append(what)


class Server:
    def __init__(self, binary: Path, *flags: str) -> None:
        self.log = open(WORLD / "server.log", "a")
        self.process = subprocess.Popen(
            [str(binary), f"--port={PORT}", f"--world={WORLD / 'world'}", *flags],
            cwd=ROOT, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
                return
            except OSError:
                time.sleep(0.3)
        raise RuntimeError("server did not start")

    def stop(self) -> None:
        self.process.send_signal(signal.SIGINT)
        self.process.wait(timeout=60)
        self.log.close()


def player_file(name: str = NAME) -> Path:
    return WORLD / "world" / "playerdata" / f"{offline_uuid(name)}.dat"


def leaves(name: str = NAME) -> dict:
    return {k: v[1] for k, v in flatten(read_nbt(player_file(name).read_bytes())).items()}


def slot_payload(item: str, count: int) -> bytes:
    return bytes([1]) + varint(ITEM[item]) + bytes([count]) + bytes([0])


def join() -> Faller:
    probe = Faller(PORT)
    probe.pump(2.5)
    return probe


def leave(probe: Faller) -> None:
    probe.socket.close()
    time.sleep(1.0)


def window_contents(probe: Faller) -> dict[int, tuple[int, int]]:
    """The last Set Container Content for window 0: slot -> (item, count)."""
    out: dict[int, tuple[int, int]] = {}
    for pid, payload in probe.captured:
        if pid != CB_CONTAINER_CONTENT or payload[0] != 0:
            continue
        _, i = read_varint(payload, 1)
        count, i = read_varint(payload, i)
        out = {}
        for slot in range(count):
            present = payload[i]
            i += 1
            if not present:
                continue
            item, i = read_varint(payload, i)
            n = payload[i]
            i += 1
            if payload[i] != 0:
                raise ValueError("an item with NBT: this parser does not skip NBT")
            i += 1
            out[slot] = (item, n)
    return out


def effects_seen(probe: Faller) -> dict[int, tuple[int, int]]:
    """Entity Effect: effect id -> (amplifier, duration), the first of each."""
    out: dict[int, tuple[int, int]] = {}
    for pid, payload in probe.captured:
        if pid != CB_ENTITY_EFFECT:
            continue
        _, i = read_varint(payload, 0)
        effect, i = read_varint(payload, i)
        amplifier = payload[i]
        duration, _ = read_varint(payload, i + 1)
        if duration >= 1 << 31:
            duration -= 1 << 32
        out.setdefault(effect, (amplifier, duration))
    return out


def effects_in_file(file: dict) -> dict[int, tuple[int, int]]:
    """Every top-level effect of a file: id -> (amplifier, duration). The
    amplifier is a signed byte on disk and an unsigned one on the wire."""
    out: dict[int, tuple[int, int]] = {}
    for key, value in file.items():
        if key.startswith("/ActiveEffects[id") and key.endswith("]/Id"):
            base = key[: -len("/Id")]
            out[value] = (file[f"{base}/Amplifier"] & 0xFF, file[f"{base}/Duration"])
    return out


def first_health(probe: Faller) -> tuple[float, int] | None:
    for pid, payload in probe.captured:
        if pid == CB_SET_HEALTH:
            health = struct.unpack_from(">f", payload, 0)[0]
            food, _ = read_varint(payload, 4)
            return health, food
    return None


def check_restored(probe: Faller, expect_effects: dict[int, tuple[int, int]] | None,
                   where: str) -> None:
    pos = probe.position
    check(pos is not None and abs(pos[0] - 40.5) < 1e-9 and abs(pos[2] - 25.5) < 1e-9,
          f"{where}: sent back to (40.5, 25.5), got {pos}")
    contents = window_contents(probe)
    for slot, (item, count) in STACKS.items():
        check(contents.get(slot) == (ITEM[item], count),
              f"{where}: window slot {slot} holds {count} {item} (got {contents.get(slot)})")
    held = [p for pid, p in probe.captured if pid == CB_SET_HELD_ITEM]
    check(held == [bytes([3])], f"{where}: Set Held Item says slot 3 (got {held})")
    seen = effects_seen(probe)
    if expect_effects is not None:
        check(seen == expect_effects, f"{where}: effects {expect_effects}, got {seen}")


def main() -> int:
    binary = Path(sys.argv[1]) if len(sys.argv) > 1 else (
        ROOT / "build" / "macos-debug" / "bin" / "ov_dedicated")
    if WORLD.exists():
        shutil.rmtree(WORLD)
    WORLD.mkdir(parents=True)

    print("run 1: creative, four stacks, slot 3, two effects")
    server = Server(binary, "--effect=speed:1:2400,night_vision:0:infinite")
    try:
        probe = join()
        for slot, (item, count) in STACKS.items():
            probe.send(SB_CREATIVE_SLOT, struct.pack(">h", slot) + slot_payload(item, count))
        probe.send(SB_SET_HELD_ITEM, struct.pack(">h", 3))
        _, y, _ = probe.position
        probe.move_to(40.5, y, 25.5, True)
        probe.pump(2.0)
        leave(probe)
    finally:
        server.stop()
    check(player_file().exists(), f"{player_file().name} written at the disconnect")
    raw = player_file().read_bytes()
    check(raw[:2] == b"\x1f\x8b", "gzip")
    f1 = leaves()
    check(f1.get("/DataVersion") == 3465, "DataVersion 3465")
    check(f1.get("/Pos[0]") == 40.5 and f1.get("/Pos[2]") == 25.5, "Pos 40.5, 25.5")
    for slot, (item, count) in STACKS.items():
        s = FILE_SLOT[slot]
        check(f1.get(f"/Inventory[slot{s}]/id") == item and
              f1.get(f"/Inventory[slot{s}]/Count") == count, f"file slot {s}: {count} {item}")
    check(f1.get("/SelectedItemSlot") == 3, "SelectedItemSlot 3")
    check(f1.get("/playerGameType") == 1, "playerGameType 1 (creative)")
    speed_1 = f1.get("/ActiveEffects[id1]/Duration")
    check(speed_1 is not None and 0 < speed_1 < 2400, f"speed counted down: {speed_1} < 2400")
    check(f1.get("/ActiveEffects[id16]/Duration") == -1, "night vision infinite")

    print("run 2: survival, restored, then a fall and hunger")
    server = Server(binary, "--survival", "--effect=hunger:255:40")
    try:
        probe = join()
        seen = effects_seen(probe)
        check_restored(probe, None, "run 2 join")
        check(1 in seen and seen[1][0] == 1 and seen[1][1] == leaves()["/ActiveEffects[id1]/Duration"],
              f"speed II restored with the file's duration: {seen.get(1)}")
        check(seen.get(16) == (0, -1), f"night vision restored infinite: {seen.get(16)}")
        probe.pump(2.5)  # the hunger runs out
        probe.fall(10.0)
        probe.pump(1.0)
        leave(probe)
    finally:
        server.stop()
    f2 = leaves()
    old = player_file().with_name(player_file().name + "_old")
    check(old.exists() and old.read_bytes() == raw,
          ".dat_old is run 1's file: the save before the last one, as vanilla keeps it")
    check(f2.get("/Health") == 13.0, f"Health 13 after a ten-block fall (file: {f2.get('/Health')})")
    check(f2.get("/foodLevel", 20) < 20, f"food drained: {f2.get('/foodLevel')}")
    check(f2.get("/playerGameType") == 0, "playerGameType 0 (survival)")
    check(f2.get("/previousPlayerGameType") == 1, "previousPlayerGameType 1: the mode changed here")
    print("run 3: the restart, then a reconnect")
    server = Server(binary, "--survival")
    try:
        probe = join()
        check_restored(probe, effects_in_file(f2), "restart")
        check(first_health(probe) == (13.0, f2["/foodLevel"]),
              f"restart: first Set Health {first_health(probe)} == (13.0, {f2['/foodLevel']})")
        probe.pump(1.0)
        leave(probe)
        f3 = leaves()
        durations = effects_in_file(f3)
        check(f3["/ActiveEffects[id1]/Duration"] < f2["/ActiveEffects[id1]/Duration"],
              "the effect kept counting while online")
        probe = join()
        check_restored(probe, durations, "reconnect")
        check(first_health(probe) == (13.0, f3["/foodLevel"]),
              f"reconnect: first Set Health {first_health(probe)}")
        leave(probe)
    finally:
        server.stop()

    print("stranger: a file from another version")
    target = player_file(STRANGER)
    name = b"DataVersion"
    body = (b"\x0a\x00\x00" + b"\x03" + struct.pack(">H", len(name)) + name +
            struct.pack(">i", 3337) + b"\x00")
    target.write_bytes(gzip.compress(body))
    before = target.read_bytes()
    server = Server(binary, "--survival")
    try:
        sock = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        host = b"127.0.0.1"
        def frame(pid: int, payload: bytes) -> bytes:
            body = varint(pid) + payload
            return varint(len(body)) + body
        sock.sendall(frame(0, varint(763) + varint(len(host)) + host + struct.pack(">H", PORT)
                           + varint(2)))
        sock.sendall(frame(0, varint(len(STRANGER)) + STRANGER.encode() + bytes([0])))
        data = sock.recv(65536)
        length, i = read_varint(data, 0)
        pid, i = read_varint(data, i)
        check(pid == 0x00, f"login refused with Login Disconnect (got packet 0x{pid:02X})")
        sock.close()
        time.sleep(0.5)
    finally:
        server.stop()
    check(target.read_bytes() == before, "the refused file is byte for byte untouched")
    log = (WORLD / "server.log").read_text()
    check("3337" in log and STRANGER in log, "the log names the player and the version")

    print(f"\n{len(failures)} failure(s)")
    for failure in failures:
        print(f"  - {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
