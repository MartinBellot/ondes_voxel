#!/usr/bin/env python3
"""De bout en bout, contre notre serveur : la météo, la foudre, le lit.

Le même client sonde que `measure_weather.py` (qui l'a fait parler au vrai
serveur 1.20.1), cette fois contre `build/<preset>/bin/ov_dedicated`, piloté par
sa console (stdin). Rien n'est simulé : un clic de lit part en Use Item On, et
ce qui revient est ce que le serveur a envoyé.

  1. `weather thunder` : les Game Events 1, 7 et 8, les niveaux qui montent de
     0,01 par tick ;
  2. le lit de jour : « respawn point set » puis « only at night » en barre
     d'action ;
  3. le lit de nuit : la pose SLEEPING (métadonnée 6 = 2, position du lit en
     14), puis après 100 ticks la nuit sautée (Update Time ≥ 24000) et
     l'animation 2 ;
  4. se lever avant (Player Command 2) ;
  5. mourir et revenir au lit ; le lit enlevé, revenir au point du monde avec
     le Game Event 0 ;
  6. la foudre, avec OV_THUNDER_CHANCE abaissé pour qu'elle tombe en secondes :
     les Spawn Entity de lightning_bolt, et un paratonnerre qui les attire.

Usage : python3 scripts/check_weather_e2e.py
Écrit .scratch/weather-e2e.json.
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from measure_weather import (ENTITY_IDS, Watcher, parse_metadata, read_varint,  # noqa: E402
                             teleport, try_sleep)

ROOT = Path(__file__).resolve().parent.parent
PRESET = os.environ.get("OV_PRESET", "macos-debug")
BINARY = ROOT / "build" / PRESET / "bin" / "ov_dedicated"
WORLD = ROOT / ".scratch" / "e2e-weather"
OUT = ROOT / ".scratch" / "weather-e2e.json"
PORT = int(os.environ.get("OV_E2E_PORT", "25641"))


class Console:
    """Our server's console, shaped like measure_weather's vanilla Server."""

    def __init__(self, extra_env: dict[str, str] | None = None, tag: str = "main") -> None:
        if WORLD.exists():
            shutil.rmtree(WORLD)
        WORLD.mkdir(parents=True)
        env = dict(os.environ)
        env.update(extra_env or {})
        self.log = open(ROOT / ".scratch" / f"e2e-weather-{tag}.log", "w")
        self.process = subprocess.Popen(
            [str(BINARY), f"--world={WORLD}", f"--port={PORT}", "--survival", "--log-level=info"],
            stdin=subprocess.PIPE, stdout=self.log, stderr=subprocess.STDOUT, text=True, env=env)
        for _ in range(600):
            time.sleep(0.1)
            try:
                with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                    return
            except OSError:
                if self.process.poll() is not None:
                    raise RuntimeError("the server stopped before listening")
        raise RuntimeError("the server never listened")

    def batch(self, commands: list[str], timeout: float = 0.0) -> list[str]:
        assert self.process.stdin is not None
        self.process.stdin.write("".join(c + "\n" for c in commands))
        self.process.stdin.flush()
        time.sleep(0.4)
        return []

    def stop(self) -> None:
        try:
            self.batch(["stop"])
            self.process.wait(timeout=60)
        except Exception:
            self.process.kill()


def join(port: int, name: str) -> Watcher:
    # A Debug server takes more than its own login timeout to have the spawn
    # chunk ready (briefing, trap 29): knock until it answers.
    for attempt in range(12):
        try:
            w = Watcher(port, name)
            for _ in range(200):
                if w.position is not None:
                    return w
                time.sleep(0.1)
            w.close()
        except (OSError, EOFError):
            pass
        time.sleep(5.0)
    raise RuntimeError("the probe never got in")


def game_events(w: Watcher, t: float) -> list[tuple[int, float]]:
    return [(p[0], struct.unpack_from(">f", p, 1)[0]) for _, _, p in w.since(t, 0x1F)]


def main() -> int:
    if not BINARY.exists():
        print(f"no binary at {BINARY}")
        return 2
    result: dict = json.loads(OUT.read_text()) if OUT.exists() else {}
    only = os.environ.get("OV_E2E_ONLY", "")

    server = Console(tag="bed")
    w = None
    try:
        w = join(PORT, "ovprobe")
        feet = w.position
        gy = int(round(feet[1]))
        result["spawn"] = feet
        server.batch(["gamerule doDaylightCycle false", "gamerule doMobSpawning false",
                      "difficulty easy", "time set 1000"])
        if only == "respawn":
            respawn_checks(server, w, gy, result)
            return finish(result)

        # 1. Thunder.
        # A Debug server under load runs well under twenty ticks a second: long
        # enough for the levels to pass 0.2 whatever the tick rate.
        t = time.monotonic()
        server.batch(["weather thunder"])
        time.sleep(20.0)
        events = game_events(w, t)
        rain = [v for k, v in events if k == 7]
        thunder = [v for k, v in events if k == 8]
        steps = [round(b - a, 4) for a, b in zip(rain, rain[1:])]
        result["thunder"] = {"begin": sum(1 for k, _ in events if k == 1),
                             "rain_levels": len(rain), "thunder_levels": len(thunder),
                             "rain_max": max(rain) if rain else None,
                             "steps": sorted(set(steps))}
        server.batch(["weather clear"])
        time.sleep(6.0)

        # 2-4. The bed.
        head, foot = (5, gy, 0), (4, gy, 0)
        server.batch([f"setblock {head[0]} {head[1]} {head[2]} minecraft:red_bed[part=head,facing=east]",
                      f"setblock {foot[0]} {foot[1]} {foot[2]} minecraft:red_bed[part=foot,facing=east]"])
        near = (4.5, float(gy), -1.5)
        import measure_weather
        measure_weather.FOOT = foot
        measure_weather.HEAD = head

        server.batch(["time set 1000"])
        result["day"] = try_sleep(server, w, near)
        server.batch(["time set 18000"])
        result["leave"] = try_sleep(server, w, near)

        server.batch(["gamerule doDaylightCycle true", "time set 18000"])
        t = time.monotonic()
        night = try_sleep(server, w, near, wait=8.0, leave=False)
        night["update_times"] = [struct.unpack_from(">qq", p, 0) for _, _, p in w.since(t, 0x5E)]
        night["animations"] = [(read_varint(p, 0)[0], p[read_varint(p, 0)[1]])
                               for _, _, p in w.since(t, 0x04)]
        night["all_positions"] = [p for tt, p in w.positions if tt >= t]
        result["night"] = night
        server.batch(["gamerule doDaylightCycle false"])

        # 5. Respawn at the bed, then without it.
        respawn_checks(server, w, gy, result)
    finally:
        if w is not None:
            w.close()
        server.stop()

    # 6. Lightning, made frequent.
    server = Console({"OV_THUNDER_CHANCE": os.environ.get("OV_E2E_THUNDER", "4000")}, tag="bolt")
    w = None
    try:
        w = join(PORT, "ovbolt")
        feet = w.position
        gy = int(round(feet[1]))
        server.batch(["gamerule doDaylightCycle false", "gamerule doMobSpawning false",
                      "gamemode creative ovbolt", "weather thunder 1000000"])
        time.sleep(5.0)
        for phase, seconds in (("open", 40.0), ("rod", 40.0)):
            if phase == "rod":
                rx, rz = int(feet[0]) + 20, int(feet[2]) + 20
                server.batch([f"fill {rx} {gy} {rz} {rx} {gy + 3} {rz} minecraft:stone",
                              f"setblock {rx} {gy + 4} {rz} minecraft:lightning_rod"])
                result["rod"] = (rx, gy + 4, rz)
            t = time.monotonic()
            time.sleep(seconds)
            bolts = []
            for _, _, p in w.since(t, 0x01):
                eid, i = read_varint(p, 0)
                i += 16
                etype, i = read_varint(p, i)
                if etype == ENTITY_IDS["lightning_bolt"]:
                    bolts.append(struct.unpack_from(">ddd", p, i))
            removed = len(w.since(t, 0x3E))
            result[f"bolts_{phase}"] = {"count": len(bolts), "positions": bolts[:40],
                                        "remove_packets": removed}
    finally:
        if w is not None:
            w.close()
        server.stop()
        shutil.rmtree(WORLD, ignore_errors=True)

    return finish(result)


def respawn_checks(server: Console, w: Watcher, gy: int, result: dict) -> None:
    """Die with a bed as the respawn point, then with the bed gone."""
    import measure_weather
    head, foot = (5, gy, 0), (4, gy, 0)
    measure_weather.FOOT = foot
    # Survival, said: only a mortal player dies of /kill and is offered a
    # respawn — the first run of this check was creative and never came back.
    server.batch(["gamemode survival ovprobe",
                  f"setblock {head[0]} {head[1]} {head[2]} minecraft:red_bed[part=head,facing=east]",
                  f"setblock {foot[0]} {foot[1]} {foot[2]} minecraft:red_bed[part=foot,facing=east]",
                  "time set 1000"])
    # A click by day sets the respawn point and sleeps no one.
    set_spawn = try_sleep(server, w, (4.5, float(gy), -1.5))
    teleport(server, w, (-6.5, float(gy), -6.5))
    for label, remove in (("respawn_bed", False), ("respawn_no_bed", True)):
        if remove:
            server.batch([f"setblock {head[0]} {head[1]} {head[2]} minecraft:air",
                          f"setblock {foot[0]} {foot[1]} {foot[2]} minecraft:air"])
            teleport(server, w, (-6.5, float(gy), -6.5))
        t = time.monotonic()
        server.batch(["kill ovprobe"])
        time.sleep(2.0)
        w.respawn()
        time.sleep(4.0)
        result[label] = {"positions": [p for tt, p in w.positions if tt >= t],
                         "game_events": game_events(w, t),
                         "respawn_packets": len(w.since(t, 0x41)),
                         "chat": w.chat(t),
                         "health": [struct.unpack_from(">f", p, 0)[0] for _, _, p in w.since(t, 0x57)]}
    result["respawn_set_spawn"] = set_spawn["chat"]


def finish(result: dict) -> int:
    OUT.write_text(json.dumps(result, indent=1, default=str))
    print(json.dumps({k: (v if not isinstance(v, dict) else {kk: vv for kk, vv in v.items()
                                                                if kk not in ("metadata", "positions")})
                      for k, v in result.items()}, indent=1, default=str)[:6000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
