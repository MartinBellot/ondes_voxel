#!/usr/bin/env python3
"""De bout en bout, sur **notre** serveur : la marche, la division du slime, et
qui apparaît dans quel biome.

Le client est la sonde protocole 763 de `check_husbandry_e2e.py` ; elle ne voit
que le fil. Les commandes passent par la console (stdin) d'`ov_dedicated`.

  speed    Chaque espèce invoquée sur le superplat, quatre fois ; la sonde suit
           les `Update Entity Position` et prend le plateau haut des pas — la
           même statistique que `measure_mobs2.py stroll` sur le vrai serveur.
  slime    Des slimes invoqués (taille tirée par le serveur, 1, 2 ou 4) tués à la
           console ; on compte les `Spawn Entity` de slime qui suivent chaque
           mort, et la taille lue à l'indice 16 de leur métadonnée.
  biomes   Le monde généré à la graine de référence (`OV_WORLDGEN_SEED`), la
           sonde placée aux positions que le vrai serveur a utilisées
           (`normalized/mobs2_speed.json`, campagne `biomes`), minuit, et chaque
           `Spawn Entity` compté par type.

Usage : python3 scripts/check_mobs2_e2e.py [speed] [slime] [biomes]
Écrit data/vanilla/1.20.1/normalized/mobs2_e2e.json (gitignoré).
"""
from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_husbandry_e2e import Probe as HusbandryProbe  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
NORMALIZED = ROOT / "data" / "vanilla" / "1.20.1" / "normalized"
OUT = NORMALIZED / "mobs2_e2e.json"
PORT = int(os.environ.get("OV_E2E_PORT", "25639"))

SPECIES = ["zombie", "skeleton", "creeper", "spider", "cow", "pig", "sheep", "chicken", "husk",
           "stray", "drowned", "cave_spider", "witch", "enderman", "rabbit", "wolf", "fox",
           "cat", "horse"]
SLIME_SIZE_INDEX = 16


def entity_names() -> list[str]:
    reg = json.loads((NORMALIZED / "registries.json").read_text())
    return reg["registries"]["minecraft:entity_type"]["entries"]


class Probe(HusbandryProbe):
    """The husbandry probe, keeping every position step of every entity."""

    def __init__(self, port: int, name: str) -> None:
        super().__init__(port, name)
        self.steps: dict[int, list[tuple[float, float]]] = {}

    def handle(self, pid: int, p: bytes) -> None:
        if pid in (0x2B, 0x2C):
            from vanilla_miner import read_varint  # noqa: E402
            import struct
            eid, i = read_varint(p, 0)
            dx, _, dz = struct.unpack_from(">hhh", p, i)
            self.steps.setdefault(eid, []).append(
                (time.monotonic(), math.hypot(dx / 4096.0, dz / 4096.0)))
        super().handle(pid, p)


class Ours:
    def __init__(self, world: Path, env: dict | None = None) -> None:
        shutil.rmtree(world, ignore_errors=True)
        world.mkdir(parents=True)
        self.log = open(world.parent / f"{world.name}.log", "w")
        self.process = subprocess.Popen(
            [str(BINARY), f"--world={world}", f"--port={PORT}", "--log-level=info"],
            stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT, text=True,
            env={**os.environ, **(env or {})})
        self.world = world

    def console(self, *lines: str) -> None:
        assert self.process.stdin is not None
        for line in lines:
            self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def stop(self) -> None:
        try:
            self.console("stop")
            self.process.wait(timeout=60)
        except Exception:
            self.process.kill()
        shutil.rmtree(self.world, ignore_errors=True)


def connect(name: str) -> Probe:
    for _ in range(30):
        try:
            return Probe(PORT, name)
        except (OSError, EOFError):
            time.sleep(4.0)
    raise RuntimeError("never joined")


def plateau(steps: list[float]) -> float | None:
    moving = sorted(s for s in steps if s > 0.02)
    if len(moving) < 5:
        return None
    top = moving[min(len(moving) - 1, int(0.9 * len(moving)))]
    band = [s for s in moving if abs(s - top) <= 0.04 * top]
    return band[len(band) // 2] if band else top


def check_speed(names: list[str]) -> dict:
    server = Ours(ROOT / ".scratch" / "e2e-mobs2-flat")
    out: dict = {}
    try:
        time.sleep(3.0)
        probe = connect("OndeWalker")
        probe.settle(6.0)
        server.console("gamerule doMobSpawning false", "time set midnight")
        x0, y0, z0 = probe.position if hasattr(probe, "position") else (0.0, -60.0, 0.0)
        for index, kind in enumerate(SPECIES):
            for k in range(4):
                server.console(f"summon minecraft:{kind} {x0 + 12 + 10 * k:.1f} {y0:.1f} "
                               f"{z0 - 60 + 7 * index:.1f}")
        probe.settle(90.0)
        by_type: dict[str, list[float]] = {}
        for eid, steps in probe.steps.items():
            etype = probe.types.get(eid)
            if etype is None:
                continue
            by_type.setdefault(names[etype], []).extend(s for _, s in steps)
        for name, steps in sorted(by_type.items()):
            out[name] = {"cruise": plateau(steps), "n": len(steps)}
            print(f"  {name:24} cruise {out[name]['cruise']}  n={len(steps)}", flush=True)
    finally:
        server.stop()
    return out


def check_slime(names: list[str]) -> dict:
    server = Ours(ROOT / ".scratch" / "e2e-mobs2-slime")
    slime = names.index("minecraft:slime")
    out: dict = {"kills": []}
    try:
        time.sleep(3.0)
        probe = connect("OndeSlimer")
        probe.settle(6.0)
        server.console("gamerule doMobSpawning false")
        x0, y0, z0 = probe.position if hasattr(probe, "position") else (0.0, -60.0, 0.0)
        for trial in range(40):
            server.console("kill @e[type=minecraft:slime]")
            probe.settle(1.0)
            server.console("kill @e[type=minecraft:slime]")
            probe.settle(1.0)
            before = {e for e, t in probe.types.items() if t == slime}
            server.console(f"summon minecraft:slime {x0 + 6:.1f} {y0:.1f} {z0:.1f}")
            probe.settle(1.0)
            parent = [e for e, t in probe.types.items() if t == slime and e not in before]
            if not parent:
                continue
            size = probe.field(parent[0], SLIME_SIZE_INDEX)
            known = {e for e, t in probe.types.items() if t == slime}
            server.console("kill @e[type=minecraft:slime]")
            probe.settle(1.5)
            children = [e for e, t in probe.types.items() if t == slime and e not in known]
            child_sizes = sorted({probe.field(c, SLIME_SIZE_INDEX) for c in children})
            out["kills"].append({"size": size, "children": len(children),
                                 "child_sizes": child_sizes})
        by_size: dict = {}
        for kill in out["kills"]:
            by_size.setdefault(str(kill["size"]), []).append(kill["children"])
        out["by_size"] = {s: {str(n): c.count(n) for n in sorted(set(c))}
                          for s, c in by_size.items()}
        print(f"  slime: children by parent size {out['by_size']}", flush=True)
    finally:
        server.stop()
    return out


def check_biomes(names: list[str]) -> dict:
    vanilla = json.loads((NORMALIZED / "mobs2_speed.json").read_text()).get("biomes", {})
    out: dict = {}
    server = Ours(ROOT / ".scratch" / "e2e-mobs2-world", {"OV_WORLDGEN_SEED": "1234567890"})
    try:
        time.sleep(5.0)
        probe = connect("OndeBiome")
        probe.settle(10.0)
        server.console("gamemode creative OndeBiome", "gamerule doMobSpawning false",
                       "gamerule doDaylightCycle false")
        for biome, row in vanilla.items():
            if "probe" not in row:
                continue
            px, py, pz = row["probe"]
            server.console(f"tp OndeBiome {px:.1f} {py + 1:.1f} {pz:.1f}")
            # Debug generation: the chunks around a new position take a while.
            probe.settle(60.0)
            server.console("kill @e[type=!minecraft:player]", "time set midnight")
            probe.settle(2.0)
            known = set(probe.types)
            server.console("gamerule doMobSpawning true")
            probe.settle(240.0)
            server.console("gamerule doMobSpawning false")
            counts: dict[str, int] = {}
            for eid, etype in probe.types.items():
                if eid in known:
                    continue
                name = names[etype]
                if name in ("minecraft:item", "minecraft:experience_orb", "minecraft:arrow"):
                    continue
                w = probe.where.get(eid)
                if w is None or math.hypot(w[0] - px, w[2] - pz) > 128:
                    continue
                counts[name] = counts.get(name, 0) + 1
            out[biome] = {"probe": [px, py, pz], "counts": counts}
            print(f"  {biome}: {counts}", flush=True)
    finally:
        server.stop()
    return out


def main(argv: list[str]) -> int:
    wanted = argv[1:] or ["speed", "slime", "biomes"]
    names = entity_names()
    result = json.loads(OUT.read_text()) if OUT.exists() else {}
    for name, fn in (("speed", check_speed), ("slime", check_slime), ("biomes", check_biomes)):
        if name in wanted:
            print(f"── {name}", flush=True)
            result[name] = fn(names)
            OUT.write_text(json.dumps(result, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
