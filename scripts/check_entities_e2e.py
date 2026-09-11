#!/usr/bin/env python3
"""One writer for entities/: a cart and a mob in the same chunk, the cart
moved to the next chunk, a save, a restart — each comes back where it was.

Before the fix two modules wrote entities/ each from its own copy of the
files, and the last writer erased the other's changes: a cart laid next to a
mob could come back where it had been read, or not at all
(docs/provenance/mobs-3.md, « un seul écrivain »). On our own server:

  1. a line of rails x = 0..30 on the superflat grass, across the border of
     chunks (0,0) and (1,0);
  2. a minecart set on the rail at x = 2, and a cow summoned from the console
     in the same chunk, (0,0);
  3. the probe rides the cart east until it is past x = 17, in chunk (1,0),
     and gets off;
  4. `stop` (the server saves): entities/r.0.0.mca is read here — exactly
     one minecart in the whole file, in chunk (1,0); the cow in chunk (0,0);
  5. the server starts again: one minecart spawns, where the file says, and
     the cow where the file says.

`readback` (under the vanilla lock) has the real 1.20.1 server open the world
step 4 left in .scratch/entities-e2e-world and list the minecarts and cows:

    python3 scripts/check_entities_e2e.py
    lockf /tmp/ov-vanilla.lock python3 scripts/check_entities_e2e.py readback

Writes .scratch/entities_e2e.json (untracked).
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from anvil_read import chunks  # noqa: E402
from capture_entity_packets import Probe  # noqa: E402
from check_rails_e2e import ENTITY_TYPES, SURFACE, Y, Hand, moves, spawns  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
SCRATCH = ROOT / ".scratch"
RUN = SCRATCH / "entities-e2e"
KEPT = SCRATCH / "entities-e2e-world"
OUT = SCRATCH / "entities_e2e.json"
PORT = int(os.environ.get("OV_E2E_PORT", "25651"))
VANILLA_PORT = int(os.environ.get("OV_VANILLA_PORT", "25652"))
Z = 4
COW = (6.5, float(Y), 9.5)


class Ours:
    def __init__(self, world: Path) -> None:
        self.log = open(RUN / "server.log", "a")
        self.process = subprocess.Popen(
            [str(BINARY), f"--world={world}", f"--port={PORT}", "--log-level=info"],
            cwd=ROOT, stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT, text=True)

    def console(self, *lines: str) -> None:
        assert self.process.stdin is not None
        for line in lines:
            self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def stop(self) -> None:
        try:
            self.console("stop")
            self.process.wait(timeout=120)
        except Exception:
            self.process.kill()
            self.process.wait()


def connect() -> Probe:
    last = None
    for _ in range(40):
        try:
            return Probe(PORT)
        except OSError as error:  # the spawn chunks take a while in Debug (pitfall 29)
            last = error
            time.sleep(3.0)
    raise SystemExit(f"could not connect: {last}")


def saved_entities(world: Path) -> list[dict]:
    """Every entity of entities/, with its chunk: {chunk, id, pos}."""
    out = []
    for region in sorted((world / "entities").glob("r.*.mca")):
        rx, rz = (int(v) for v in region.name.split(".")[1:3])
        # anvil_read gives the chunk's index inside its region, not its
        # coordinates: a cow at x = -1.26 read back as chunk 31.
        for lx, lz, root in chunks(region):
            for entity in root.get("Entities", []):
                out.append({"chunk": (rx * 32 + lx, rz * 32 + lz), "id": entity.get("id"),
                            "pos": [round(v, 4) for v in entity.get("Pos", [])]})
    return out


def check_ours(report: dict) -> list[str]:
    failures: list[str] = []
    if not BINARY.exists():
        return [f"{BINARY} not found; build ov_dedicated first"]
    shutil.rmtree(RUN, ignore_errors=True)
    RUN.mkdir(parents=True)
    world = RUN / "world"
    cart_type = ENTITY_TYPES["minecraft:minecart"]
    cow_type = ENTITY_TYPES["minecraft:cow"]

    server = Ours(world)
    try:
        probe = connect()
        hand = Hand(probe)
        probe.pump(4.0)
        probe.drain()
        # 1. The line, across the chunk border at x = 16.
        hand.hold("minecraft:rail")
        for x in range(0, 31):
            hand.use_on(x, SURFACE, Z)
            probe.pump(0.12)
        probe.pump(1.0)
        probe.drain()
        # 2. A cart in chunk (0,0), and a cow beside it — penned in a glass
        # ring two blocks high, so it is still in (0,0) when the server saves
        # (our server ignores NoAI, and a loose cow walked out in two runs).
        hand.hold("minecraft:glass")
        cx, cz = int(COW[0]), int(COW[2])
        for layer in (SURFACE, Y):
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    if dx or dz:
                        hand.use_on(cx + dx, layer, cz + dz)
                        probe.pump(0.1)
        probe.pump(0.5)
        probe.drain()
        hand.hold("minecraft:minecart")
        hand.use_on(2, Y, Z)
        server.console(f"summon minecraft:cow {COW[0]} {COW[1]} {COW[2]}")
        probe.pump(2.0)
        packets = probe.drain()
        carts = spawns(packets, cart_type)
        cows = spawns(packets, cow_type)
        report["placed_cart"] = carts
        report["summoned_cow"] = cows
        if not carts or not cows:
            failures.append(f"cart {carts} or cow {cows} did not appear")
            return failures
        cart = carts[0]["id"]
        x = carts[0]["pos"][0]
        # 3. Ride it east into chunk (1,0).
        hand.hold("minecraft:stick")
        hand.interact(cart)
        probe.pump(1.0)
        probe.drain()
        hand.face(-90.0)
        deadline = time.monotonic() + 25.0
        while time.monotonic() < deadline and x < 17.5:
            hand.input(0.98)
            probe.pump(0.05)
            x += moves(probe.drain(), cart)
        report["cart_x_on_the_wire"] = round(x, 3)
        hand.input(0.0, 0x02)
        probe.pump(2.0)
        probe.drain()
        if x < 16.0:
            failures.append(f"the cart only reached x = {x:.2f}, not chunk (1,0)")
        probe.socket.close()
    finally:
        server.stop()  # 4. saves

    saved = saved_entities(world)
    report["saved"] = saved
    saved_carts = [e for e in saved if e["id"] == "minecraft:minecart"]
    saved_cows = [e for e in saved if e["id"] == "minecraft:cow"]
    if len(saved_carts) != 1:
        failures.append(f"{len(saved_carts)} minecarts in entities/, expected 1: {saved_carts}")
    elif saved_carts[0]["chunk"] != (1, 0):
        failures.append(f"the cart was saved in chunk {saved_carts[0]['chunk']}, not (1, 0)")
    # The chunk the cart left still holds the cow, and only the cow.
    if len(saved_cows) != 1 or saved_cows[0]["chunk"] != (0, 0):
        failures.append(f"expected one cow saved in chunk (0, 0): {saved_cows}")
    if any(e["id"] == "minecraft:minecart" and e["chunk"] == (0, 0) for e in saved):
        failures.append("a minecart was left behind in chunk (0, 0)")
    if saved_carts and saved_carts[0]["chunk"] == (1, 0):
        others = [e for e in saved if e["chunk"] == (1, 0) and e["id"] != "minecraft:minecart"]
        report["chunk_1_0_besides_the_cart"] = others
    shutil.rmtree(KEPT, ignore_errors=True)
    shutil.copytree(world, KEPT)

    # 5. Restart.
    server = Ours(world)
    try:
        probe = connect()
        probe.pump(8.0)
        packets = probe.drain()
        carts = spawns(packets, cart_type)
        cows = spawns(packets, cow_type)
        report["after_restart"] = {"carts": carts, "cows": cows}
        if len(carts) != 1:
            failures.append(f"{len(carts)} minecarts after the restart, expected 1")
        elif saved_carts and any(abs(a - b) > 1e-3 for a, b in zip(carts[0]["pos"], saved_carts[0]["pos"])):
            failures.append(f"the cart came back at {carts[0]['pos']}, saved at {saved_carts[0]['pos']}")
        for cow in saved_cows:
            if not any(all(abs(a - b) < 1e-3 for a, b in zip(c["pos"], cow["pos"])) for c in cows):
                failures.append(f"the cow saved at {cow['pos']} did not come back there: {cows}")
        probe.socket.close()
    finally:
        server.stop()
    shutil.rmtree(RUN, ignore_errors=True)
    return failures


POS = re.compile(r"\[(-?[\d.]+)d, (-?[\d.]+)d, (-?[\d.]+)d\]")


def readback(report: dict) -> list[str]:
    """The real server opens the world ours saved. Run under the vanilla lock."""
    from measure_entities import Server  # noqa: E402 — the JVM driver

    if not KEPT.exists():
        return [f"{KEPT} not found: run the check without arguments first"]
    directory = SCRATCH / "entities-readback"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    shutil.copytree(KEPT, directory / "world")
    failures: list[str] = []
    server = Server(directory, port=VANILLA_PORT)
    try:
        server.batch(["forceload add -16 -16 47 31"])
        time.sleep(4.0)
        found: dict = {}
        for kind in ("minecart", "cow"):
            lines = server.batch([f"execute as @e[type=minecraft:{kind}] run data get entity @s Pos"])
            found[kind] = [[float(v) for v in m.groups()] for line in lines
                           if (m := POS.search(line))]
        report["vanilla_reads"] = found
        saved = saved_entities(KEPT)
        for kind in ("minecart", "cow"):
            expected = sorted(e["pos"] for e in saved if e["id"] == f"minecraft:{kind}")
            got = sorted([round(v, 4) for v in p] for p in found[kind])
            report[f"vanilla_{kind}s"] = {"saved": expected, "read": got}
            if len(got) != len(expected):
                failures.append(f"vanilla reads {len(got)} {kind}(s), our save holds {len(expected)}")
    finally:
        server.stop()
        shutil.rmtree(directory, ignore_errors=True)
    return failures


def main(argv: list[str]) -> int:
    report: dict = {}
    failures = readback(report) if "readback" in argv else check_ours(report)
    SCRATCH.mkdir(exist_ok=True)
    previous = json.loads(OUT.read_text()) if OUT.exists() else {}
    previous["readback" if "readback" in argv else "ours"] = report
    OUT.write_text(json.dumps(previous, indent=1, default=str))
    print(json.dumps(report, indent=1, default=str))
    for failure in failures:
        print("FAIL:", failure)
    if not failures:
        print("entities e2e:", "readback" if "readback" in argv else "ours", "— all checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
