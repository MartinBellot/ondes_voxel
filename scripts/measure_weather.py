#!/usr/bin/env python3
"""Ask a real 1.20.1 server how its weather, its lightning and its beds behave.

Every campaign runs `tools/vanilla/server.jar` in a superflat of its own, with
one or more probe clients connected: a vanilla server with nobody on it ticks
no chunk, so it neither snows, nor strikes, nor lets anyone sleep.

  draws      The durations the weather draws. `/weather rain|thunder|clear`
             without a duration, then `save-all flush` and `level.dat` read
             back, a few hundred times each; then the *natural* cycle: a
             `weather clear 1` let run out with doWeatherCycle on, and the
             timers the cycle drew right after, read the same way.
  sleep      Beds. The day/night edges at which sleeping is refused (clear,
             rain, thunder), too far, obstructed, monsters near (a zombie
             moved a fifth of a block at a time across the box), sleeping
             itself (the metadata, the position, the wake-up, the time after),
             leaving the bed, the respawn at the bed and without it, an
             occupied bed, and a bed clicked in the Nether.
  precip     A snowy_plains superflat under snow: how many columns get a layer
             in a counted number of ticks, the layer histogram once
             snowAccumulationHeight is raised to 3, ponds that freeze from the
             edge, and cauldrons that fill with powder snow.
  lightning  A plains superflat under a thunderstorm, three probes far apart:
             how many bolts, where, and — once lightning rods stand — how many
             are drawn to a rod. Farmland under the open sky and under glass,
             and cauldrons in the rain, are read at the end.
  strike     `/summon lightning_bolt` on mobs: the damage box, the pig, the
             villager, the creeper, the mooshroom and the turtle.

Usage: python3 scripts/measure_weather.py [campaign ...]

Writes .scratch/weather-oracle.json in the worktree, merging campaign by
campaign. Nothing Mojang made is kept: counts, positions and translation keys.
"""
from __future__ import annotations

import gzip
import json
import math
import shutil
import socket
import struct
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import anvil_read  # noqa: E402
from capture_entity_packets import Probe, read_varint, varint  # noqa: E402
from measure_mobs import FlatServer  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / ".scratch" / "weather-oracle.json"
RUN = ROOT / ".scratch" / "weather-server"
PORT = 25631

REGISTRIES = ROOT / "data" / "vanilla" / "1.20.1" / "generated" / "reports" / "registries.json"
ENTITY_IDS = {name.split(":")[1]: entry["protocol_id"] for name, entry in json.loads(
    REGISTRIES.read_text())["minecraft:entity_type"]["entries"].items()}

GROUND = -61  # the grass of the default superflat; things stand at -60


def block_pos(x: int, y: int, z: int) -> bytes:
    v = ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)
    return struct.pack(">Q", v)


def unpack_pos(v: int) -> tuple[int, int, int]:
    x = v >> 38
    y = v & 0xFFF
    z = (v >> 12) & 0x3FFFFFF
    if x >= 1 << 25:
        x -= 1 << 26
    if y >= 1 << 11:
        y -= 1 << 12
    if z >= 1 << 25:
        z -= 1 << 26
    return x, y, z


def parse_metadata(p: bytes, i: int) -> list:
    out = []
    while i < len(p):
        index = p[i]
        i += 1
        if index == 0xFF:
            break
        kind, i = read_varint(p, i)
        if kind == 0:
            value = struct.unpack_from(">b", p, i)[0]
            i += 1
        elif kind in (1, 12, 14, 15, 19, 20, 21, 22, 24, 25):
            value, i = read_varint(p, i)
        elif kind == 2:
            value, i = read_varint(p, i)
        elif kind == 3:
            value = struct.unpack_from(">f", p, i)[0]
            i += 4
        elif kind == 8:
            value = p[i] != 0
            i += 1
        elif kind == 10:
            value = unpack_pos(struct.unpack_from(">q", p, i)[0])
            i += 8
        elif kind == 11:
            present = p[i]
            i += 1
            value = unpack_pos(struct.unpack_from(">q", p, i)[0]) if present else None
            i += 8 if present else 0
        else:
            out.append((index, kind, "undecoded"))
            break
        out.append((index, kind, value))
    return out


class Watcher(Probe):
    """A probe that reads on a thread and keeps every packet a campaign asks about."""

    KEPT = {0x01, 0x04, 0x1D, 0x1F, 0x28, 0x3E, 0x41, 0x46, 0x52, 0x57, 0x5E, 0x64}

    def __init__(self, port: int, name: str) -> None:
        self.lock = threading.Lock()
        super().__init__(port, name)
        self.name = name
        self.eid: int | None = None
        self.state_lock = threading.Lock()
        self.log: list[tuple[float, int, bytes]] = []
        self.positions: list[tuple[float, tuple[float, float, float]]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def send(self, packet_id: int, payload: bytes) -> None:
        with self.lock:
            super().send(packet_id, payload)

    def _run(self) -> None:
        self.socket.settimeout(0.5)
        while not self._stop.is_set():
            try:
                pid, p = self.read()
            except (socket.timeout, TimeoutError):
                continue
            except Exception:
                return
            now = time.monotonic()
            if pid == 0x23:
                self.send(0x12, p[:8])
            elif pid == 0x3C:
                x, y, z = struct.unpack_from(">ddd", p, 0)
                flags = p[32]
                # Bits 0x01, 0x02, 0x04: x, y, z relative. `tp x y z` keeps the
                # rotation and sends 0x18 — reading only flags 0 as a move, as
                # the first version did, left the probe answering with its old
                # position and the server putting it back there: every click
                # after a teleport was then out of reach.
                old = self.position or (0.0, 0.0, 0.0)
                x = old[0] + x if flags & 0x01 else x
                y = old[1] + y if flags & 0x02 else y
                z = old[2] + z if flags & 0x04 else z
                self.position = (x, y, z)
                with self.state_lock:
                    self.positions.append((now, (x, y, z)))
                tid, _ = read_varint(p, 33)
                self.send(0x00, varint(tid))
                if self.position is not None:
                    self.send(0x14, struct.pack(">ddd", *self.position) + bytes([1]))
            elif pid == 0x28 and self.eid is None:
                self.eid = struct.unpack_from(">i", p, 0)[0]
            if pid in self.KEPT:
                with self.state_lock:
                    self.log.append((now, pid, p))

    def since(self, t: float, pid: int | None = None) -> list[tuple[float, int, bytes]]:
        with self.state_lock:
            return [e for e in self.log if e[0] >= t and (pid is None or e[1] == pid)]

    def chat(self, t: float) -> list[tuple[str, bool]]:
        out = []
        for _, _, p in self.since(t, 0x64):
            n, i = read_varint(p, 0)
            out.append((p[i:i + n].decode(), p[i + n] != 0))
        return out

    def click(self, x: int, y: int, z: int, face: int = 1, sequence: int = 1) -> None:
        self.send(0x31, varint(0) + block_pos(x, y, z) + varint(face)
                  + struct.pack(">fff", 0.5, 0.5625, 0.5) + bytes([0]) + varint(sequence))
        self.send(0x2F, varint(0))

    def leave_bed(self) -> None:
        self.send(0x1E, varint(self.eid or 0) + varint(2) + varint(0))

    def respawn(self) -> None:
        self.send(0x07, varint(0))

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        try:
            self.socket.close()
        except Exception:
            pass


def level_dat(run: Path) -> dict:
    return anvil_read.parse((run / "world" / "level.dat").read_bytes())["Data"]


def save(server) -> None:
    server.batch(["save-all flush"], timeout=120)


# ── Chunk reading ───────────────────────────────────────────────────────────

def section_blocks(section: dict) -> list[str] | None:
    """4096 state strings in y, z, x order, or None for an absent palette."""
    states = section.get("block_states")
    if not states:
        return None
    palette = []
    for entry in states["palette"]:
        props = entry.get("Properties")
        name = entry["Name"]
        if props:
            name += "[" + ",".join(f"{k}={v}" for k, v in sorted(props.items())) + "]"
        palette.append(name)
    if len(palette) == 1:
        return [palette[0]] * 4096
    bits = max(4, (len(palette) - 1).bit_length())
    per = 64 // bits
    out = []
    mask = (1 << bits) - 1
    for word in states["data"]:
        word &= (1 << 64) - 1
        for k in range(per):
            if len(out) == 4096:
                break
            out.append(palette[(word >> (k * bits)) & mask])
    return out


def read_blocks(run: Path, positions: list[tuple[int, int, int]]) -> dict:
    """State strings at the given positions, read from the saved regions."""
    wanted: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
    for x, y, z in positions:
        wanted.setdefault((x >> 4, z >> 4), []).append((x, y, z))
    regions: dict[tuple[int, int], set] = {}
    for cx, cz in wanted:
        regions.setdefault((cx >> 5, cz >> 5), set()).add((cx, cz))
    out = {}
    for (rx, rz), chunks in regions.items():
        path = run / "world" / "region" / f"r.{rx}.{rz}.mca"
        if not path.exists():
            continue
        for lx, lz, chunk in anvil_read.chunks(path):
            cx, cz = rx * 32 + lx, rz * 32 + lz
            if (cx, cz) not in chunks:
                continue
            sections = {s["Y"]: s for s in chunk.get("sections", [])}
            cache: dict[int, list[str] | None] = {}
            for x, y, z in wanted[(cx, cz)]:
                sy = y >> 4
                if sy not in cache:
                    cache[sy] = section_blocks(sections[sy]) if sy in sections else None
                blocks = cache[sy]
                if blocks is None:
                    out[(x, y, z)] = "minecraft:air"
                    continue
                out[(x, y, z)] = blocks[((y & 15) * 16 + (z & 15)) * 16 + (x & 15)]
    return out


# ── draws ───────────────────────────────────────────────────────────────────

def campaign_draws(server, run: Path) -> dict:
    samples = int(__import__("os").environ.get("OV_WEATHER_DRAWS", "300"))
    server.batch(["gamerule doWeatherCycle false", "gamerule doDaylightCycle false"])
    result: dict = {}
    for kind, key in (("rain", "rainTime"), ("thunder", "thunderTime"),
                      ("clear", "clearWeatherTime")):
        values = []
        for _ in range(samples):
            server.batch([f"weather {kind}", "save-all flush"], timeout=120)
            data = level_dat(run)
            values.append(int(data[key]))
            if kind != "clear":
                # rain and thunder both write the drawn duration into both
                # timers; clear writes it into clearWeatherTime alone.
                result.setdefault(f"{kind}_other_timer", []).append(
                    int(data["thunderTime" if key == "rainTime" else "rainTime"]))
        result[kind] = values
        print(f"  {kind}: n={len(values)} min={min(values)} max={max(values)}", flush=True)

    # The natural cycle, right after a clear spell ends.
    natural = []
    for _ in range(max(40, samples // 5)):
        server.batch(["weather clear 1", "gamerule doWeatherCycle true"])
        time.sleep(0.5)
        server.batch(["gamerule doWeatherCycle false", "save-all flush"], timeout=120)
        data = level_dat(run)
        natural.append({k: int(data[k]) for k in ("rainTime", "thunderTime", "clearWeatherTime",
                                                  "raining", "thundering")})
    result["after_clear"] = natural
    print(f"  after clear: {sum(1 for n in natural if n['raining'])} of {len(natural)} raining, "
          f"{sum(1 for n in natural if n['thundering'])} thundering", flush=True)
    return result


# ── sleep ───────────────────────────────────────────────────────────────────

HEAD = (5, -60, 0)
FOOT = (4, -60, 0)


def place_bed(server, dimension: str | None = None) -> None:
    prefix = f"execute in {dimension} run " if dimension else ""
    server.batch([f"{prefix}setblock {HEAD[0]} {HEAD[1]} {HEAD[2]} minecraft:red_bed[part=head,facing=east]",
                  f"{prefix}setblock {FOOT[0]} {FOOT[1]} {FOOT[2]} minecraft:red_bed[part=foot,facing=east]"])


def teleport(server, w: Watcher, stand: tuple[float, float, float]) -> bool:
    """Move a probe and wait until it has confirmed the move.

    A vanilla server ignores Use Item On while a teleport is unconfirmed —
    the first sleep campaign lost every click of its first probe that way,
    and read "nothing happened" for thirty set-ups."""
    t = time.monotonic()
    server.batch([f"tp {w.name} {stand[0]} {stand[1]} {stand[2]}"])
    deadline = time.monotonic() + 8.0
    confirmed = False
    while time.monotonic() < deadline and not confirmed:
        with w.state_lock:
            confirmed = any(tt >= t for tt, _ in w.positions)
        time.sleep(0.05)
    time.sleep(0.3)
    return confirmed


def try_sleep(server, w: Watcher, stand: tuple[float, float, float], wait: float = 1.2,
              leave: bool = True) -> dict:
    confirmed = teleport(server, w, stand)
    t = time.monotonic()
    w.click(*FOOT)
    time.sleep(wait)
    metas = [parse_metadata(p, read_varint(p, 0)[1]) for _, _, p in w.since(t, 0x52)
             if read_varint(p, 0)[0] == w.eid]
    slept = any(any(f[0] == 6 and f[2] == 2 for f in m) for m in metas)
    out = {"chat": w.chat(t), "slept": slept, "metadata": metas, "confirmed": confirmed,
           "positions": [p for tt, p in w.positions if tt >= t]}
    if slept and leave:
        t2 = time.monotonic()
        w.leave_bed()
        time.sleep(0.8)
        out["leave_chat"] = w.chat(t2)
        out["leave_animations"] = [(read_varint(p, 0)[0], p[read_varint(p, 0)[1]])
                                   for _, _, p in w.since(t2, 0x04)]
        out["leave_metadata"] = [parse_metadata(p, read_varint(p, 0)[1])
                                 for _, _, p in w.since(t2, 0x52) if read_varint(p, 0)[0] == w.eid]
        out["leave_positions"] = [p for tt, p in w.positions if tt >= t2]
    return out


def daytime(server) -> int:
    for line in server.batch(["time query daytime"]):
        if "The time is" in line:
            return int(line.rsplit(" ", 1)[1])
    raise RuntimeError("no daytime")


def campaign_sleep(server, run: Path) -> dict:
    server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "gamerule doInsomnia false",
                  "gamerule doImmediateRespawn true", "difficulty easy", "time set 18000",
                  "setworldspawn 0 -60 -8", "forceload add -32 -32 32 32"])
    time.sleep(3.0)
    w = Watcher(PORT, "ovsleep")
    time.sleep(2.0)
    server.batch(["gamemode survival ovsleep",
                  "effect give ovsleep minecraft:resistance infinite 5 true",
                  "effect give ovsleep minecraft:saturation infinite 1 true"])
    place_bed(server)
    near = (4.5, -60.0, -1.5)
    r: dict = {"eid": w.eid}

    # Day and night edges, clear sky.
    edges = {}
    for t in (1000, 12541, 12542, 23459, 23460):
        server.batch([f"time set {t}"])
        edges[t] = try_sleep(server, w, near)
    r["edges_clear"] = edges

    # Rain, then thunder, once the levels have climbed.
    server.batch(["weather rain 1000000"])
    time.sleep(6.0)
    edges = {}
    for t in (12009, 12010, 23991, 23992, 6000):
        server.batch([f"time set {t}"])
        edges[t] = try_sleep(server, w, near)
    r["edges_rain"] = edges
    server.batch(["weather thunder 1000000"])
    time.sleep(6.0)
    edges = {}
    for t in (6000, 1000):
        server.batch([f"time set {t}"])
        edges[t] = try_sleep(server, w, near)
    r["edges_thunder"] = edges
    server.batch(["weather clear 1000000", "time set 18000"])
    time.sleep(6.0)

    # Too far: the head's bottom centre is (5.5, -60, 0.5), the foot's (4.5, -60, 0.5).
    far = {}
    for label, stand in (("dz3.0", (4.5, -60.0, -2.5)), ("dz3.1", (4.5, -60.0, -2.6)),
                         ("dx3.0", (8.5, -60.0, 0.5)), ("dx3.1", (8.6, -60.0, 0.5))):
        far[label] = try_sleep(server, w, stand)
    server.batch(["setblock 4 -59 -2 minecraft:glass", "setblock 4 -58 -2 minecraft:glass"])
    far["dy2.0"] = try_sleep(server, w, (4.5, -58.0, -1.5))
    server.batch(["setblock 4 -57 -2 minecraft:glass"])
    far["dy3.0"] = try_sleep(server, w, (4.5, -57.0, -1.5))
    server.batch(["fill 4 -59 -2 4 -57 -2 minecraft:air"])
    r["too_far"] = far

    # Obstructed.
    obstructed = {}
    server.batch(["setblock 5 -59 0 minecraft:stone"])
    obstructed["above_head"] = try_sleep(server, w, near)
    server.batch(["setblock 5 -59 0 minecraft:air", "setblock 4 -59 0 minecraft:stone"])
    obstructed["above_foot"] = try_sleep(server, w, near)
    server.batch(["setblock 4 -59 0 minecraft:glass"])
    obstructed["glass_above_foot"] = try_sleep(server, w, near)
    server.batch(["setblock 4 -59 0 minecraft:oak_slab"])
    obstructed["slab_above_foot"] = try_sleep(server, w, near)
    server.batch(["setblock 4 -59 0 minecraft:air"])
    r["obstructed"] = obstructed

    # Monsters: one at a time, against the head's bottom centre (5.5, -60, 0.5).
    monsters = {}
    for label, kind, pos in (
            ("zombie_dx8.2", "zombie", (13.7, -60.0, 0.5)),
            ("zombie_dx8.4", "zombie", (13.9, -60.0, 0.5)),
            ("zombie_dz8.2", "zombie", (5.5, -60.0, 8.7)),
            ("zombie_dz8.4", "zombie", (5.5, -60.0, 8.9)),
            ("zombie_back_dx8.2", "zombie", (-2.7, -60.0, 0.5)),
            ("zombie_back_dx9.2", "zombie", (-3.7, -60.0, 0.5)),
            ("zombie_up4.9", "zombie", (5.5, -55.1, 0.5)),
            ("zombie_up5.1", "zombie", (5.5, -54.9, 0.5)),
            ("zombified_piglin_dx2", "zombified_piglin", (7.5, -60.0, 0.5)),
            ("creeper_dx6", "creeper", (11.5, -60.0, 0.5)),
            ("spider_dx6", "spider", (11.5, -60.0, 0.5)),
            ("enderman_dx6", "enderman", (11.5, -60.0, 0.5)),
    ):
        server.batch([f"summon minecraft:{kind} {pos[0]} {pos[1]} {pos[2]} "
                      "{NoAI:1b,NoGravity:1b,Silent:1b,PersistenceRequired:1b,Tags:[\"ovm\"]}"])
        time.sleep(0.3)
        monsters[label] = try_sleep(server, w, near)
        server.batch(["kill @e[tag=ovm]"])
        time.sleep(0.3)
    r["monsters"] = monsters

    # Sleeping through a night: doDaylightCycle on, rain on with the cycle on.
    server.batch(["gamerule doDaylightCycle true", "gamerule doWeatherCycle true",
                  "weather rain 100000", "time set 18000"])
    time.sleep(1.0)
    t = time.monotonic()
    before = daytime(server)
    stay = try_sleep(server, w, near, wait=8.0, leave=False)
    after = daytime(server)
    server.batch(["gamerule doWeatherCycle false", "gamerule doDaylightCycle false"])
    save(server)
    data = level_dat(run)
    stay["daytime_before"] = before
    stay["daytime_after"] = after
    stay["weather_after"] = {k: int(data[k]) for k in ("rainTime", "thunderTime", "raining",
                                                       "thundering", "clearWeatherTime")}
    stay["animations"] = [(read_varint(p, 0)[0], p[read_varint(p, 0)[1]])
                          for _, _, p in w.since(t, 0x04)]
    stay["update_times"] = [struct.unpack_from(">qq", p, 0) for _, _, p in w.since(t, 0x5E)]
    stay["game_events"] = [(p[0], struct.unpack_from(">f", p, 1)[0])
                           for _, _, p in w.since(t, 0x1F)]
    stay["actionbar"] = w.chat(t)
    stay["all_metadata"] = [parse_metadata(p, read_varint(p, 0)[1]) for _, _, p in w.since(t, 0x52)
                            if read_varint(p, 0)[0] == w.eid]
    # How long the bed held the player: first SLEEPING pose to the wake animation.
    r["sleep_through"] = stay

    # Respawn at the bed, then with the bed gone.
    server.batch(["time set 18000"])
    t = time.monotonic()
    server.batch(["kill ovsleep"])
    time.sleep(1.0)
    w.respawn()
    time.sleep(2.0)
    r["respawn_bed"] = {"positions": [p for tt, p in w.positions if tt >= t],
                        "chat": w.chat(t),
                        "game_events": [(p[0], struct.unpack_from(">f", p, 1)[0])
                                        for _, _, p in w.since(t, 0x1F)]}
    server.batch(["setblock 5 -60 0 minecraft:air", "setblock 4 -60 0 minecraft:air"])
    t = time.monotonic()
    server.batch(["kill ovsleep"])
    time.sleep(1.0)
    w.respawn()
    time.sleep(2.0)
    r["respawn_no_bed"] = {"positions": [p for tt, p in w.positions if tt >= t],
                           "chat": w.chat(t),
                           "game_events": [(p[0], struct.unpack_from(">f", p, 1)[0])
                                           for _, _, p in w.since(t, 0x1F)]}
    place_bed(server)
    server.batch(["effect give ovsleep minecraft:resistance infinite 5 true"])

    # Occupied, and the sleeping-players count, with a second probe.
    second = Watcher(PORT, "ovsleep2")
    time.sleep(2.0)
    server.batch(["gamemode survival ovsleep2", "time set 18000",
                  "gamerule doDaylightCycle false"])
    t = time.monotonic()
    other = try_sleep(server, second, near, wait=1.5, leave=False)
    r["second_sleeps"] = {"chat_second": other["chat"], "chat_first": w.chat(t),
                          "slept": other["slept"]}
    r["occupied"] = try_sleep(server, w, (5.5, -60.0, 1.5))
    second.leave_bed()
    time.sleep(0.5)
    server.batch(["gamerule playersSleepingPercentage 50", "gamerule doDaylightCycle true",
                  "time set 18000"])
    t = time.monotonic()
    half = try_sleep(server, second, near, wait=7.0, leave=False)
    r["half_sleeping"] = {"chat_second": half["chat"], "chat_first": w.chat(t),
                          "daytime_after": daytime(server)}
    server.batch(["gamerule playersSleepingPercentage 100", "gamerule doDaylightCycle false"])
    second.close()
    time.sleep(1.0)

    # The Nether.
    server.batch(["execute in minecraft:the_nether run fill -4 -61 -6 12 -52 6 minecraft:air",
                  "execute in minecraft:the_nether run fill -4 -62 -6 12 -62 6 minecraft:netherrack"])
    t = time.monotonic()
    server.batch([f"execute in minecraft:the_nether run tp ovsleep {near[0]} {near[1]} {near[2]}"])
    time.sleep(4.0)
    place_bed(server, "minecraft:the_nether")
    time.sleep(0.5)
    t = time.monotonic()
    w.click(*FOOT)
    time.sleep(1.5)
    explosions = []
    for _, _, p in w.since(t, 0x1D):
        x, y, z, power = struct.unpack_from(">dddf", p, 0)
        records, _ = read_varint(p, 28)
        explosions.append({"x": x, "y": y, "z": z, "power": power, "records": records})
    r["nether"] = {"explosions": explosions, "chat": w.chat(t)}
    w.close()
    return r


# ── precipitation ───────────────────────────────────────────────────────────

class SnowyServer(FlatServer):
    EXTRA_PROPERTIES = FlatServer.EXTRA_PROPERTIES + (
        'generator-settings={"layers":[{"block":"minecraft:bedrock","height":1},'
        '{"block":"minecraft:dirt","height":2},{"block":"minecraft:grass_block","height":1}],'
        '"biome":"minecraft:snowy_plains"}\n'
    )


def gametime(server) -> int:
    for line in server.batch(["time query gametime"]):
        if "The time is" in line:
            return int(line.rsplit(" ", 1)[1])
    raise RuntimeError("no gametime")


PONDS = [(20 + 10 * i, 20 + 10 * j) for i in range(6) for j in range(6)]
CAULDRONS = [(-80 + i, -80 + j) for i in range(20) for j in range(20)]
SNOW_FIELD = [(x, z) for x in range(-64, 0) for z in range(0, 64)]


def campaign_precip(server, run: Path) -> dict:
    server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "gamerule randomTickSpeed 0",
                  "time set noon", "weather clear 1000000",
                  "forceload add -96 -96 95 95"])
    time.sleep(3.0)
    w = Watcher(PORT, "ovsnow")
    time.sleep(2.0)
    server.batch(["gamemode creative ovsnow", "tp ovsnow 0.5 -60 0.5"])
    cmds = []
    for px, pz in PONDS:
        cmds.append(f"fill {px} {GROUND} {pz} {px + 4} {GROUND} {pz + 4} minecraft:water")
    for zs in range(-80, -60, 4):
        cmds.append(f"fill -80 -60 {zs} -61 -60 {zs + 3} minecraft:cauldron")
    server.batch(cmds)
    time.sleep(2.0)

    t0 = gametime(server)
    server.batch(["weather rain 1000000"])
    wait_a = float(__import__("os").environ.get("OV_PRECIP_A", "150"))
    time.sleep(wait_a)
    server.batch(["gamerule snowAccumulationHeight 3"])
    t1 = gametime(server)
    wait_b = float(__import__("os").environ.get("OV_PRECIP_B", "300"))
    time.sleep(wait_b / 2)
    t_mid = gametime(server)
    # Half-way, the map as it stands.
    save(server)
    mid = read_blocks(run, [(x, -60, z) for x, z in SNOW_FIELD])
    time.sleep(wait_b / 2)
    server.batch(["weather clear 1000000"])
    t2 = gametime(server)
    save(server)
    positions = [(x, -60, z) for x, z in SNOW_FIELD]
    positions += [(px + dx, GROUND, pz + dz) for px, pz in PONDS for dx in range(5) for dz in range(5)]
    positions += [(x, -60, z) for x, z in CAULDRONS]
    blocks = read_blocks(run, positions)
    w.close()
    return {
        "ticks": {"start": t0, "height3": t1, "mid": t_mid, "end": t2},
        "mid_field": [mid.get((x, -60, z)) for x, z in SNOW_FIELD],
        "field": [blocks.get((x, -60, z)) for x, z in SNOW_FIELD],
        "ponds": [[blocks.get((px + dx, GROUND, pz + dz)) for dx in range(5) for dz in range(5)]
                  for px, pz in PONDS],
        "cauldrons": [blocks.get((x, -60, z)) for x, z in CAULDRONS],
    }


# ── lightning ───────────────────────────────────────────────────────────────

CENTRES = [(0, 0), (512, 0), (0, 512)]
ROD_OFFSET = (40, 40)


def bolts_seen(watchers: list[Watcher], t: float) -> dict[int, tuple[float, float, float]]:
    bolts = {}
    for w in watchers:
        for _, _, p in w.since(t, 0x01):
            eid, i = read_varint(p, 0)
            i += 16
            etype, i = read_varint(p, i)
            if etype != ENTITY_IDS["lightning_bolt"]:
                continue
            bolts[eid] = struct.unpack_from(">ddd", p, i)
    return bolts


def campaign_lightning(server, run: Path) -> dict:
    server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "gamerule doFireTick false",
                  "difficulty peaceful", "time set noon", "weather clear 1000000"])
    time.sleep(3.0)
    watchers = []
    for n, (cx, cz) in enumerate(CENTRES):
        w = Watcher(PORT, f"ovbolt{n}")
        time.sleep(1.5)
        server.batch([f"gamemode creative ovbolt{n}", f"tp ovbolt{n} {cx + 0.5} -60 {cz + 0.5}"])
        watchers.append(w)
    time.sleep(8.0)
    # Farmland open to the sky and under glass, and cauldrons, near the first probe.
    cmds = ["fill -30 -61 -30 -16 -61 -16 minecraft:farmland[moisture=0]",
            "fill 16 -61 -30 30 -61 -16 minecraft:farmland[moisture=0]",
            "fill 16 -58 -30 30 -58 -16 minecraft:glass"]
    for zs in range(16, 36, 4):
        cmds.append(f"fill -30 -60 {zs} -11 -60 {zs + 3} minecraft:cauldron")
    server.batch(cmds)
    time.sleep(1.0)

    result: dict = {"centres": CENTRES}
    phase_seconds = float(__import__("os").environ.get("OV_BOLT_SECONDS", "480"))
    for phase in ("open", "rods"):
        if phase == "rods":
            cmds = []
            for cx, cz in CENTRES:
                rx, rz = cx + ROD_OFFSET[0], cz + ROD_OFFSET[1]
                cmds += [f"fill {rx} -60 {rz} {rx} -57 {rz} minecraft:stone",
                         f"setblock {rx} -56 {rz} minecraft:lightning_rod"]
            server.batch(cmds)
            result["rod_positions"] = [(cx + ROD_OFFSET[0], -56, cz + ROD_OFFSET[1])
                                       for cx, cz in CENTRES]
        server.batch(["weather thunder 1000000"])
        t = time.monotonic()
        g0 = gametime(server)
        time.sleep(phase_seconds)
        g1 = gametime(server)
        bolts = bolts_seen(watchers, t)
        result[phase] = {"ticks": g1 - g0, "bolts": list(bolts.values())}
        print(f"  {phase}: {len(bolts)} bolts in {g1 - g0} ticks", flush=True)
    server.batch(["weather clear 1000000"])
    save(server)
    farm = [(x, -61, z) for x in range(-30, -15) for z in range(-30, -15)]
    covered = [(x, -61, z) for x in range(16, 31) for z in range(-30, -15)]
    cauldrons = [(x, -60, z) for x in range(-30, -10) for z in range(16, 36)]
    blocks = read_blocks(run, farm + covered + cauldrons)
    result["farmland_open"] = [blocks.get(p) for p in farm]
    result["farmland_covered"] = [blocks.get(p) for p in covered]
    result["cauldrons"] = [blocks.get(p) for p in cauldrons]
    for w in watchers:
        w.close()
    return result


# ── strike ──────────────────────────────────────────────────────────────────

def campaign_strike(server, run: Path) -> dict:
    server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "gamerule doFireTick false",
                  "difficulty normal", "time set noon", "forceload add -64 -64 63 63"])
    time.sleep(3.0)
    w = Watcher(PORT, "ovstrike")
    time.sleep(2.0)
    server.batch(["gamemode creative ovstrike", "tp ovstrike 0.5 -60 -20.5"])
    time.sleep(1.0)
    out: dict = {}

    def count(kind: str) -> int:
        lines = server.batch([f"execute store result score ovc ovcount if entity @e[type=minecraft:{kind}]",
                              "scoreboard players get ovc ovcount"])
        for line in lines:
            if " has " in line:
                return int(line.split(" has ")[1].split(" ")[0])
        return -1

    server.batch(["scoreboard objectives add ovcount dummy", "scoreboard players set ovc ovcount 0"])
    # Conversions, one per spot, 20 blocks apart.
    for n, (kind, extra) in enumerate((("pig", ""), ("villager", ""), ("creeper", ""),
                                       ("mooshroom", ",Type:\"red\""), ("mooshroom", ",Type:\"brown\""),
                                       ("turtle", ""), ("cow", ""), ("zombie", ""))):
        x = -40 + 20 * n
        server.batch(["kill @e[type=!minecraft:player]"])
        time.sleep(0.3)
        t = time.monotonic()
        server.batch([f"summon minecraft:{kind} {x}.5 -60 0.5 {{NoAI:1b,PersistenceRequired:1b,"
                      f"Tags:[\"ovs\"]{extra}}}"])
        time.sleep(0.4)
        before = {k: count(k) for k in (kind, "zombified_piglin", "witch")}
        server.batch([f"summon minecraft:lightning_bolt {x}.5 -60 0.5"])
        time.sleep(0.6)
        after = {k: count(k) for k in (kind, "zombified_piglin", "witch", "mooshroom")}
        lines = server.batch(["data get entity @e[tag=ovs,limit=1]"])
        metas = [parse_metadata(p, read_varint(p, 0)[1]) for _, _, p in w.since(t, 0x52)]
        out[f"{kind}{extra}"] = {"before": before, "after": after,
                                 "data": [l for l in lines if "entity data" in l][-1:],
                                 "metadata": metas[-6:]}
    # The damage box: NoAI zombies around one bolt.
    server.batch(["kill @e[type=!minecraft:player]"])
    time.sleep(0.3)
    spots = {"dx3.2": (3.2, 0.0), "dx3.4": (3.4, 0.0), "dx-3.2": (-3.2, 0.0),
             "dz3.2": (0.0, 3.2), "dz3.4": (0.0, 3.4), "up8.9": (0.0, 8.9), "up9.1": (0.0, 9.1)}
    cmds = []
    for label, (dx, dy) in spots.items():
        dz = 0.0
        if label.startswith("dz"):
            dz, dx = dx, 0.0
        cmds.append(f"summon minecraft:zombie {0.5 + dx} {-60 + dy} {0.5 + dz} "
                    f"{{NoAI:1b,NoGravity:1b,PersistenceRequired:1b,Tags:[\"ovz_{label}\"]}}")
    server.batch(cmds)
    time.sleep(0.5)
    server.batch(["summon minecraft:lightning_bolt 0.5 -60 0.5"])
    time.sleep(0.25)
    health = {}
    for label in spots:
        for line in server.batch([f"data get entity @e[tag=ovz_{label},limit=1] Health"]):
            if "entity data" in line:
                health[label] = float(line.rsplit(" ", 1)[1].rstrip("f"))
    out["damage_box"] = health
    w.close()
    return out


CAMPAIGNS = {
    "draws": (FlatServer, campaign_draws),
    "sleep": (FlatServer, campaign_sleep),
    "precip": (SnowyServer, campaign_precip),
    "lightning": (FlatServer, campaign_lightning),
    "strike": (FlatServer, campaign_strike),
}


def main(argv: list[str]) -> int:
    names = argv or list(CAMPAIGNS)
    results = json.loads(OUT.read_text()) if OUT.exists() else {}
    for name in names:
        kind, run_campaign = CAMPAIGNS[name]
        if RUN.exists():
            shutil.rmtree(RUN)
        print(f"── {name}", flush=True)
        server = kind(RUN, port=PORT)
        started = time.monotonic()
        try:
            results[name] = run_campaign(server, RUN)
            results[name]["_seconds"] = round(time.monotonic() - started, 1)
        except Exception as error:  # one campaign failing must not lose the others
            import traceback
            traceback.print_exc()
            results[name] = {"_error": repr(error)}
        finally:
            OUT.parent.mkdir(parents=True, exist_ok=True)
            OUT.write_text(json.dumps(results, indent=1, default=str))
            server.stop()
            shutil.rmtree(RUN, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
