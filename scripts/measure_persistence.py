#!/usr/bin/env python3
"""What the real 1.20.1 server writes for the entities that are not mobs —
and whether it reads back what ours wrote.

The oracle is tools/vanilla/server.jar, driven through its console with a
probe client connected (a server with nobody on it ticks nothing: piège 11).

Campaigns:

  oracle    On a superflat: an item, an experience orb, an arrow, a spectral
            arrow and a trident fired into the grass, a snowball, an egg, an
            ender pearl, a splash potion and a bottle o' enchanting in flight,
            a primed TNT, a falling sand, an area effect cloud; in the Nether
            nine of its mobs; in the End a crystal and the dragon. Each is read
            with `data get entity` (the typed SNBT: `3b`, `40s`, `1.0f`), then
            the probe is put in a minecart with `/ride`, the world saved, the
            probe's playerdata read for `RootVehicle`, the probe disconnected
            (is the cart still there?) and reconnected (is it riding again?).
            The world is kept in .scratch/persistence-vanilla/ for our server
            to read.

  readback  Vanilla opens the world ov_dedicated saved
            (.scratch/persistence-ours/, written by
            scripts/check_persistence_e2e.py) and lists what it finds.

Nothing is written outside the worktree: readings go to
.scratch/persistence.json (not tracked). docs/provenance/persistance-entites.md
has the results.

    lockf /tmp/ov-vanilla.lock python3 scripts/measure_persistence.py oracle
    lockf /tmp/ov-vanilla.lock python3 scripts/measure_persistence.py readback
"""
from __future__ import annotations

import gzip
import hashlib
import json
import re
import shutil
import sys
import time
import uuid as uuidlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from anvil_read import chunks, parse  # noqa: E402
from measure_mobs import FlatServer, KeptAlive, count  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUN = ROOT / ".scratch" / "persistence-oracle"
OUT = ROOT / ".scratch" / "persistence.json"
KEEP = ROOT / ".scratch" / "persistence-vanilla"
OURS = ROOT / ".scratch" / "persistence-ours"
PORT = 25670
Y = -60
PROBE = "ovprobe"

DATA = re.compile(r"has the following entity data: (.+)$")


class Oracle(FlatServer):
    EXTRA_PROPERTIES = FlatServer.EXTRA_PROPERTIES + "difficulty=normal\n"
    HEAP = "-Xmx1200M"


def offline_uuid(name: str) -> str:
    digest = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode()).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuidlib.UUID(bytes=bytes(digest)))


def value(lines: list[str]) -> str | None:
    for line in lines:
        m = DATA.search(line)
        if m:
            return m.group(1).strip()
    return None


def get(server, selector: str, prefix: str = "") -> str | None:
    return value(server.batch([f"{prefix}data get entity {selector}"], timeout=30))


def near(kind: str, x: float, y: float, z: float, radius: float = 3.0) -> str:
    return f"@e[type=minecraft:{kind},limit=1,sort=nearest,x={x},y={y},z={z},distance=..{radius}]"


def entity_files(world: Path) -> dict[str, list[dict]]:
    """Every entity in every entities/ region of the three dimensions."""
    out: dict[str, list[dict]] = {}
    for label, sub in (("overworld", "entities"), ("nether", "DIM-1/entities"),
                       ("end", "DIM1/entities")):
        found = []
        folder = world / sub
        if folder.is_dir():
            for region in sorted(folder.glob("r.*.mca")):
                for _, _, chunk in chunks(region):
                    for entity in chunk.get("Entities", []):
                        found.append({"chunk": chunk.get("Position"), "id": entity.get("id"),
                                      "Pos": entity.get("Pos"),
                                      "keys": sorted(entity.keys())})
        out[label] = found
    return out


def campaign_oracle(server) -> dict:
    server.batch(["gamerule doMobSpawning false", "gamerule doDaylightCycle false",
                  "gamerule doWeatherCycle false", "gamerule sendCommandFeedback true",
                  "gamerule doFireTick false", "scoreboard objectives add ovcount dummy",
                  "time set noon", "weather clear", f"gamemode creative {PROBE}",
                  f"tp {PROBE} 0.5 {Y} -8.5"], timeout=60)
    time.sleep(2.0)
    dump: dict[str, str | None] = {}
    # ── the overworld, grass top at y = -60 ──
    server.batch([
        f'summon item 4.5 {Y} 4.5 {{Item:{{id:"minecraft:diamond",Count:3b}},PickupDelay:40s,Age:100s}}',
        f"summon experience_orb 7.5 {Y} 4.5 {{Value:7s}}",
        f"summon arrow 10.5 {Y + 5} 4.5 {{Motion:[0.0,-1.0,0.0],pickup:1b}}",
        f"summon spectral_arrow 13.5 {Y + 5} 4.5 {{Motion:[0.0,-1.0,0.0],pickup:1b}}",
        f"summon trident 16.5 {Y + 5} 4.5 {{Motion:[0.0,-1.0,0.0],pickup:1b}}",
        f"summon tnt 22.5 {Y} 4.5 {{Fuse:400s}}",
        f'summon area_effect_cloud 28.5 {Y} 4.5 {{Potion:"minecraft:poison",Duration:6000,Radius:3.0f}}',
    ], timeout=60)
    time.sleep(3.0)
    dump["item"] = get(server, near("item", 4.5, Y, 4.5))
    dump["experience_orb"] = get(server, near("experience_orb", 7.5, Y, 4.5))
    dump["arrow_in_ground"] = get(server, near("arrow", 10.5, Y, 4.5, 6))
    dump["spectral_arrow_in_ground"] = get(server, near("spectral_arrow", 13.5, Y, 4.5, 6))
    dump["trident_in_ground"] = get(server, near("trident", 16.5, Y, 4.5, 6))
    dump["tnt"] = get(server, near("tnt", 22.5, Y, 4.5, 4))
    dump["area_effect_cloud"] = get(server, near("area_effect_cloud", 28.5, Y, 4.5))
    # In flight: summoned high, moving up, read in the same batch.
    fly = f"{{Motion:[0.0,0.5,0.0]}}"
    for i, (kind, extra) in enumerate([
            ("snowball", ""), ("egg", ""), ("ender_pearl", ""),
            ("potion", 'Item:{id:"minecraft:splash_potion",Count:1b,tag:{Potion:"minecraft:poison"}}'),
            ("experience_bottle", ""),
            ("snowball", 'Item:{id:"minecraft:snowball",Count:1b}'),
            ("arrow", ""), ("falling_block", 'BlockState:{Name:"minecraft:sand"},Time:1')]):
        x = 40.5 + i * 3
        nbt = fly[:-1] + ("," + extra if extra else "") + "}"
        lines = server.batch([f"summon {kind} {x} 60 4.5 {nbt}",
                              f"data get entity {near(kind, x, 60, 4.5, 4)}"], timeout=30)
        dump[f"{kind}_in_flight" + ("_with_item" if kind == "snowball" and extra else "")] = value(lines)
    # ── the Nether ──
    nether = "execute in minecraft:the_nether run "
    server.batch([nether + "forceload add 0 0 31 31"], timeout=120)
    time.sleep(3.0)
    for i, mob in enumerate(["piglin", "piglin_brute", "zombified_piglin", "ghast", "blaze",
                             "magma_cube", "hoglin", "strider", "wither_skeleton"]):
        x = 2.5 + i * 3
        server.batch([nether + f"summon minecraft:{mob} {x} 140 8.5 {{NoAI:1b,NoGravity:1b}}"],
                     timeout=30)
        dump[f"nether:{mob}"] = get(server, near(mob, x, 140, 8.5), nether)
    # ── the End ──
    end = "execute in minecraft:the_end run "
    server.batch([end + "forceload add -16 -16 15 15"], timeout=120)
    time.sleep(3.0)
    server.batch([end + "summon minecraft:end_crystal 5.5 80 5.5 {ShowBottom:0b,BeamTarget:{X:1,Y:2,Z:3}}",
                  end + "summon minecraft:ender_dragon 0.5 100 0.5"], timeout=30)
    time.sleep(1.0)
    dump["end:end_crystal"] = get(server, near("end_crystal", 5.5, 80, 5.5), end)
    dump["end:ender_dragon"] = get(server, "@e[type=minecraft:ender_dragon,limit=1]", end)
    # ── a player in a minecart ──
    server.batch([f"summon minecart 60.5 {Y} -8.5", f"tp {PROBE} 60.5 {Y} -8.5"], timeout=30)
    time.sleep(1.0)
    rode = server.batch([f"ride {PROBE} mount {near('minecart', 60.5, Y, -8.5)}"], timeout=30)
    time.sleep(1.0)
    riding = server.batch([f"execute as {PROBE} on vehicle run data get entity @s id"], timeout=30)
    server.batch(["save-all flush"], timeout=120)
    time.sleep(2.0)
    world = RUN / "world"
    player = parse((world / "playerdata" / f"{offline_uuid(PROBE)}.dat").read_bytes())
    carts_online = count(server, {"carts": "@e[type=minecraft:minecart]"})["carts"]
    return {"data_get": dump, "ride": rode, "riding": riding,
            "root_vehicle": player.get("RootVehicle"), "carts_online": carts_online}


def campaign_after_leave(server, probe) -> dict:
    """The probe leaves while riding, then comes back."""
    probe.close()
    time.sleep(3.0)
    left = count(server, {"carts": "@e[type=minecraft:minecart]"})["carts"]
    back = KeptAlive(PORT, PROBE)
    time.sleep(4.0)
    carts = count(server, {"carts": "@e[type=minecraft:minecart]"})["carts"]
    riding = server.batch([f"execute as {PROBE} on vehicle run data get entity @s id"], timeout=30)
    server.batch(["save-all flush"], timeout=120)
    time.sleep(2.0)
    return {"probe": back, "carts_after_leave": left, "carts_after_return": carts,
            "riding_after_return": riding}


READBACK_SELECTORS = {
    "item": "@e[type=minecraft:item]",
    "experience_orb": "@e[type=minecraft:experience_orb]",
    "arrow": "@e[type=minecraft:arrow]",
    "spectral_arrow": "@e[type=minecraft:spectral_arrow]",
    "trident": "@e[type=minecraft:trident]",
    "tnt": "@e[type=minecraft:tnt]",
    "falling_block": "@e[type=minecraft:falling_block]",
    "area_effect_cloud": "@e[type=minecraft:area_effect_cloud]",
    "minecart": "@e[type=minecraft:minecart]",
}


def campaign_readback(server) -> dict:
    """Vanilla reads what ov_dedicated wrote. Read in the first second: the TNT
    burns and the sand falls as soon as the chunks tick."""
    out: dict = {}
    # The world is ours: it has no scoreboard of the oracle's.
    server.batch(["scoreboard objectives add ovcount dummy", "gamerule sendCommandFeedback true"],
                 timeout=30)
    lines = []
    for name, selector in READBACK_SELECTORS.items():
        lines.append(f"execute as {selector} run data get entity @s")
    raw = server.batch(lines, timeout=60)
    out["data_get"] = [m.group(1) for line in raw if (m := DATA.search(line))]
    nether = "execute in minecraft:the_nether run "
    end = "execute in minecraft:the_end run "
    server.batch([nether + "forceload add -32 -32 31 31", end + "forceload add -32 -32 31 31"],
                 timeout=120)
    time.sleep(4.0)
    raw = server.batch([nether + "execute as @e[type=!minecraft:player] run data get entity @s id",
                        end + "execute as @e[type=minecraft:end_crystal] run data get entity @s Pos",
                        end + "execute as @e[type=minecraft:ender_dragon] run data get entity @s Health",
                        end + "execute as @e[type=minecraft:ender_dragon] run data get entity @s DragonPhase"],
                       timeout=60)
    out["dimensions"] = [m.group(1) for line in raw if (m := DATA.search(line))]
    out["counts"] = count(server, READBACK_SELECTORS)
    riding = server.batch([f"execute as {PROBE} on vehicle run data get entity @s id"], timeout=30)
    out["riding"] = riding
    return out


def main(argv: list[str]) -> int:
    wanted = argv or ["oracle"]
    result = json.loads(OUT.read_text()) if OUT.exists() else {}
    if RUN.exists():
        shutil.rmtree(RUN)
    if wanted == ["readback"]:
        if not OURS.exists():
            print(f"no {OURS}: run check_persistence_e2e.py first")
            return 1
        RUN.mkdir(parents=True)
        shutil.copytree(OURS, RUN / "world")
        (RUN / "world" / "session.lock").unlink(missing_ok=True)
    server = Oracle(RUN, PORT)
    probe = None
    try:
        probe = KeptAlive(PORT, PROBE)
        time.sleep(3.0)
        if wanted == ["readback"]:
            time.sleep(3.0)
            result["readback"] = campaign_readback(server)
        else:
            result["oracle"] = campaign_oracle(server)
            after = campaign_after_leave(server, probe)
            probe = after.pop("probe")
            result["oracle"].update(after)
            world = RUN / "world"
            result["oracle"]["files"] = entity_files(world)
            result["oracle"]["root_vehicle_after_return"] = parse(
                (world / "playerdata" / f"{offline_uuid(PROBE)}.dat").read_bytes()).get("RootVehicle")
        OUT.write_text(json.dumps(result, indent=1, default=str))
    finally:
        if probe is not None:
            probe.close()
        server.stop()
    if wanted != ["readback"]:
        if KEEP.exists():
            shutil.rmtree(KEEP)
        shutil.copytree(RUN / "world", KEEP)
    shutil.rmtree(RUN, ignore_errors=True)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
