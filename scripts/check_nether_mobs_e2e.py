#!/usr/bin/env python3
"""── nether-2 ── Our server, end to end: into the Nether, piglins, a trade, fire.

A probe client does everything through the protocol, as a player would:

  1. It builds and lights a portal on the superflat and crosses into the Nether
     (the same moves as scripts/check_nether_e2e.py).
  2. Standing there, it is made an operator and summons a piglin with a
     **player** command — a player in the Nether summons into the Nether —
     and must receive its Spawn Entity (type piglin, a Nether wire id).
  3. It holds gold ingots and right-clicks the piglin: the piglin must show
     the ingot in its off hand (Set Equipment), and about six seconds later an
     item must appear beside it (the barter). Repeated, the items are counted.
  4. It summons a blaze and a ghast and counts the small fireballs and
     fireballs they shoot at it (it is in survival, kept alive by Resistance).
  5. All along, every Nether mob that spawns without being summoned is counted:
     natural spawning by the Nether's biome lists.

Usage: python3 scripts/check_nether_mobs_e2e.py [path/to/ov_dedicated]
Exit status 0 when every check passes. Writes .scratch/nether-mobs-e2e.json.
"""
from __future__ import annotations

import json
import math
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from capture_commands import SB_CHAT_COMMAND  # noqa: E402
from capture_entity_packets import read_varint, varint  # noqa: E402
from check_nether_e2e import Builder, OvServer, unpacked  # noqa: E402
from measure_husbandry import parse_metadata  # noqa: E402
from measure_nether_portal import read_string  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "nether-mobs-e2e"
PORT = 25683
NAME = "ovportal"

NETHER_FIRST_ID = 3_000_000
TYPES = {7: "blaze", 41: "ghast", 47: "hoglin", 54: "item", 57: "fireball", 62: "magma_cube",
         73: "piglin", 74: "piglin_brute", 89: "small_fireball", 98: "strider",
         114: "wither_skeleton", 117: "zoglin", 121: "zombified_piglin", 29: "enderman",
         86: "skeleton"}


def item_names() -> list[str]:
    reg = json.loads((ROOT / "data" / "vanilla" / "1.20.1" / "normalized" /
                      "registries.json").read_text())
    return reg["registries"]["minecraft:item"]["entries"]


class NetherProbe(Builder):
    """The portal builder, recording entities too."""

    def __init__(self, port: int) -> None:
        super().__init__(port)
        self.spawns: dict[int, dict] = {}
        self.equipment: list[tuple[float, int, list]] = []
        self.item_of: dict[int, tuple[int, int]] = {}
        self.clock_ms = int(time.time() * 1000)
        self.ages: list[tuple[float, int]] = []

    def tick_at(self, moment: float) -> float | None:
        """The server's world age at a wall moment, from the Update Time
        packets around it (linear between the two nearest)."""
        before = [(t, a) for t, a in self.ages if t <= moment]
        after = [(t, a) for t, a in self.ages if t > moment]
        if not before or not after:
            return None
        (t0, a0), (t1, a1) = before[-1], after[0]
        return a0 + (a1 - a0) * (moment - t0) / (t1 - t0) if t1 > t0 else float(a0)

    def pump_once(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.005, deadline - time.monotonic()))
            try:
                packet_id, payload = self.read()
            except (TimeoutError, OSError):
                return
            now = time.monotonic()
            if packet_id == 0x0A:
                pos, i = unpacked(payload, 0)
                state, _ = read_varint(payload, i)
                self.blocks[pos] = state
            elif packet_id == 0x24:
                self.chunks_since.append((self.dimension, len(payload)))
            elif packet_id == 0x23:
                self.send(0x12, payload[:8])
            elif packet_id == 0x41:
                _, i = read_string(payload, 0)
                dim_name, i = read_string(payload, i)
                self.dimension = dim_name
                self.events.append(("respawn", (dim_name, now)))
            elif packet_id == 0x3C:
                x, y, z = struct.unpack_from(">ddd", payload, 0)
                yaw, _ = struct.unpack_from(">ff", payload, 24)
                teleport_id, _ = read_varint(payload, 33)
                self.position = (x, y, z)
                self.events.append(("sync", (self.dimension, x, y, z, yaw, 0)))
                self.send(0x00, varint(teleport_id))
                self.send(0x14, struct.pack(">ddd", x, y, z) + bytes([1]))
            elif packet_id == 0x5E and len(payload) >= 8:
                # Update Time: the world's age, so a delay is counted in the
                # server's ticks rather than in a Debug build's wall seconds.
                self.ages.append((now, struct.unpack_from(">q", payload, 0)[0]))
            elif packet_id == 0x01:
                eid, i = read_varint(payload, 0)
                i += 16
                etype, i = read_varint(payload, i)
                x, y, z = struct.unpack_from(">ddd", payload, i)
                self.spawns[eid] = {"type": etype, "t": now, "pos": (x, y, z),
                                    "dimension": self.dimension}
            elif packet_id == 0x52:
                eid, i = read_varint(payload, 0)
                try:
                    for index, kind, value in parse_metadata(payload, i):
                        if index == 8 and kind == 7 and isinstance(value, tuple):
                            self.item_of[eid] = value if value[0] != "nbt" else value[1:]
                except Exception:
                    pass
            elif packet_id == 0x55:
                eid, i = read_varint(payload, 0)
                slots = []
                while i < len(payload):
                    slot = payload[i]
                    i += 1
                    present = payload[i]
                    i += 1
                    item = None
                    if present:
                        item, i = read_varint(payload, i)
                        i += 1  # count
                        if payload[i] != 0:
                            break  # an NBT we do not read; enough for the item
                        i += 1
                    slots.append((slot & 0x7F, item))
                    if not slot & 0x80:
                        break
                self.equipment.append((now, eid, slots))

    def command(self, text: str) -> None:
        body = text.encode()
        self.clock_ms = max(self.clock_ms + 1, int(time.time() * 1000))
        self.send(SB_CHAT_COMMAND, varint(len(body)) + body + struct.pack(">qq", self.clock_ms, 0)
                  + varint(0) + varint(0) + bytes(3))

    def interact(self, eid: int) -> None:
        self.send(0x10, varint(eid) + varint(0) + varint(0) + bytes([0]))

    def nether_spawns(self, since: float = 0.0) -> list[tuple[int, dict]]:
        return [(e, s) for e, s in self.spawns.items()
                if e >= NETHER_FIRST_ID and s["t"] >= since]


def main() -> int:
    checks: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}", flush=True)

    items = item_names()
    server = OvServer(RUN, PORT)
    report: dict = {}
    summoned: set[int] = set()
    try:
        bot = None
        for _ in range(30):
            try:
                bot = NetherProbe(PORT)
                break
            except OSError:
                time.sleep(1.0)
        assert bot is not None
        bot.stand(3.0)

        # ── The portal, and across ──
        px, py, pz = bot.position
        ground = math.floor(py) - 1
        x0, z0 = math.floor(px) + 3, math.floor(pz) + 3
        base = ground + 1
        server.send(f"clear {NAME}", f"give {NAME} minecraft:obsidian 64",
                    f"give {NAME} minecraft:flint_and_steel 1")
        bot.stand(1.0)
        bot.hold(0)
        bot.stand(0.3)
        for x in range(x0 - 1, x0 + 3):
            bot.use_on(x, ground, z0, 1)
        for x in (x0 - 1, x0 + 2):
            for y in range(base, base + 4):
                bot.use_on(x, y, z0, 1)
        bot.use_on(x0 - 1, base + 4, z0, 5)
        bot.use_on(x0, base + 4, z0, 5)
        bot.stand(0.5)
        bot.hold(1)
        bot.stand(0.3)
        bot.use_on(x0, base, z0, 1)
        bot.stand(1.5)
        entry = (x0 + 1.0, float(base + 1), z0 + 0.5)
        bot.position = entry
        # The first crossing builds the Nether's generation stack and the chunks
        # round the new portal: on a Debug build beside other work it has taken
        # minutes (nether.md § 3.4). A deadline, generous, not a fixed sleep.
        arrived = bot.wait_for_dimension("minecraft:the_nether", 240.0)
        check("into the Nether", arrived is not None)
        if arrived is None:
            return 1
        _, nx, ny, nz, _, _ = arrived[1]
        crossed = time.monotonic()
        # Out of the portal, onto its floor, so as not to cross back.
        bot.position = (nx, ny, nz + 2.0)
        bot.stand(8.0)

        # ── A piglin, summoned by a player standing in the Nether ──
        server.send(f"op {NAME}", f"clear {NAME}", f"give {NAME} minecraft:gold_ingot 32",
                    f"effect give {NAME} minecraft:resistance 100000 4 true",
                    f"effect give {NAME} minecraft:fire_resistance 100000 0 true")
        bot.stand(1.0)
        bot.hold(0)
        mark = time.monotonic()
        bx, by, bz = bot.position
        bot.command(f"summon minecraft:piglin {bx + 2.0:.2f} {by:.2f} {bz:.2f}")
        bot.stand(2.0)
        piglins = [e for e, s in bot.nether_spawns(mark) if s["type"] == 73]
        check("a player's /summon in the Nether spawns a piglin there", len(piglins) == 1,
              f"ids {piglins}")
        if not piglins:
            return 1
        piglin = piglins[0]
        summoned.add(piglin)

        # ── The barter: one ingot at a time, each waited out ──
        trades = 8
        drops: list[str] = []
        offhand_seen = 0
        delays: list[float] = []
        walls: list[float] = []
        for _ in range(trades):
            bot.hold(0)
            gave = time.monotonic()
            bot.interact(piglin)
            # The offhand shows the ingot, and later empties; the stack comes
            # with it. A Debug server runs slower than 20 ticks a second, so
            # the wait is for the event, with a deadline, not a fixed sleep.
            deadline = gave + 40.0
            shown = None
            paid = None
            while time.monotonic() < deadline and paid is None:
                bot.stand(0.25)
                for t, e, slots in bot.equipment:
                    if e == piglin and t >= gave and shown is None and \
                            any(s == 1 and item is not None for s, item in slots):
                        shown = t
                for e, s in bot.spawns.items():
                    if s["type"] == 54 and s["t"] >= gave and e not in summoned:
                        paid = (e, s)
                        break
            if shown is not None:
                offhand_seen += 1
            if paid is not None:
                e, s = paid
                summoned.add(e)
                bot.stand(0.3)  # the item's metadata follows its spawn
                got = bot.item_of.get(e)
                drops.append(items[got[0]] if got else "?")
                start_wall = shown if shown is not None else gave
                walls.append(s["t"] - start_wall)
                t0 = bot.tick_at(start_wall)
                t1 = bot.tick_at(s["t"])
                if t0 is not None and t1 is not None:
                    delays.append(t1 - t0)
        report["barter_drops"] = drops
        report["barter_delay_ticks"] = delays
        report["barter_delay_wall"] = walls
        # The server's pace over the stay, from its own Update Time packets.
        if len(bot.ages) >= 2:
            (ta, aa), (tb, ab) = bot.ages[0], bot.ages[-1]
            report["ticks_per_second"] = (ab - aa) / (tb - ta) if tb > ta else None
        report["update_time_packets"] = len(bot.ages)
        check("the piglin shows the ingot in its off hand", offhand_seen == trades,
              f"{offhand_seen} / {trades}")
        check("each ingot pays one stack, 120 ticks later",
              len(drops) == trades and len(delays) > 0 and
              all(100 <= d <= 140 for d in delays),
              f"{len(drops)} stacks, delays in ticks {[round(d) for d in delays]}")
        check("what it pays is on the bartering table", all(d != "?" for d in drops),
              ", ".join(drops))

        # ── Fire, on the roof of the Nether: flat bedrock, open sky, so the
        # shooters see the player whatever the cave round the portal is ──
        natural_until = time.monotonic()
        bx, by, bz = bot.position
        server.send(f"tp {NAME} {bx:.2f} 128 {bz:.2f}")
        bot.stand(3.0)
        bx, by, bz = bot.position
        mark = time.monotonic()
        bot.command(f"summon minecraft:blaze {bx + 7.0:.2f} {by:.2f} {bz:.2f}")
        bot.stand(20.0)
        small = sorted(s["t"] for e, s in bot.nether_spawns(mark) if s["type"] == 89)
        report["blaze_shots"] = [round(t - mark, 2) for t in small]
        check("a blaze fires small fireballs at the player", len(small) >= 3,
              f"{len(small)} in 14 s: {report['blaze_shots']}")
        blazes = [e for e, s in bot.nether_spawns(mark) if s["type"] == 7]
        summoned.update(blazes)
        for blaze in blazes:
            bot.command(f"kill @e[type=minecraft:blaze]")
        mark = time.monotonic()
        bot.command(f"summon minecraft:ghast {bx:.2f} {by + 6.0:.2f} {bz + 16.0:.2f}")
        bot.stand(10.0)
        large = sorted(s["t"] for e, s in bot.nether_spawns(mark) if s["type"] == 57)
        report["ghast_shots"] = [round(t - mark, 2) for t in large]
        check("a ghast fires fireballs every three seconds", len(large) >= 2,
              f"{len(large)} in 10 s: {report['ghast_shots']}")
        summoned.update(e for e, s in bot.nether_spawns(mark) if s["type"] == 41)

        # ── Natural spawning, over the whole stay ──
        natural: dict[str, int] = {}
        for e, s in bot.nether_spawns(crossed):
            if e in summoned or s["type"] in (54, 57, 89) or s["t"] > natural_until:
                continue
            name = TYPES.get(s["type"], str(s["type"]))
            natural[name] = natural.get(name, 0) + 1
        report["natural_spawns"] = natural
        report["stay_seconds"] = time.monotonic() - crossed
        check("the Nether spawns its own mobs", sum(natural.values()) > 0,
              f"{natural} in {report['stay_seconds']:.0f} s")
        bot.socket.close()
        time.sleep(1.0)
    finally:
        server.stop()

    report["checks"] = [{"name": n, "ok": ok, "detail": d} for n, ok, d in checks]
    report["server_log_tail"] = [line for line in server.log if "nether" in line.lower()][-30:]
    out = ROOT / ".scratch" / "nether-mobs-e2e.json"
    out.write_text(json.dumps(report, indent=1))
    shutil.rmtree(RUN, ignore_errors=True)
    failed = [n for n, ok, _ in checks if not ok]
    print(f"\n{len(checks) - len(failed)} / {len(checks)} checks pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
