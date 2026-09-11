#!/usr/bin/env python3
"""Read the columns world generation froze or snowed on, out of a vanilla world.

The game's last feature step, `freeze_top_layer`, walks every column of a
chunk: at the top of MOTION_BLOCKING it puts ice on water that is cold enough
and snow on ground that is cold enough — the same two rules the weather's
chunk tick uses, minus the "only at the edge" condition on water. So a world
the real game generated is a map of where its temperature is under 0.15, and
that is the oracle for `ClimateNoise`: the frozen patches of a frozen ocean are
the frozen-temperature noise, and the snow line on a mountain is the height
noise.

This script only extracts. For every `full` chunk (briefing, trap 4) it writes
one line per interesting column:

    x z top block_top block_below biome uniform light_top light_below

`top` is the first free block above MOTION_BLOCKING; `biome` is the stored
4x4x4 cell at `top`; `uniform` is 1 when the 27 cells around it are the same
biome — the game looks biomes up through a seeded zoom that this project does
not reproduce, and a column in a uniform neighbourhood gets the same biome
either way. Cold and frozen biomes, and any column above y 100, are written in
full; one column in sixteen of the rest as a control.

`src/ov_gameplay/tests/test_climate_oracle.cpp` reads the file named by
OV_CLIMATE_ORACLE and puts our rules against it.

Usage: python3 scripts/measure_climate.py run/reference-1234567890/world out.tsv
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anvil_read  # noqa: E402

MIN_Y = -64
COLD = {
    "minecraft:frozen_ocean", "minecraft:deep_frozen_ocean", "minecraft:frozen_river",
    "minecraft:snowy_taiga", "minecraft:snowy_plains", "minecraft:snowy_beach",
    "minecraft:grove", "minecraft:snowy_slopes", "minecraft:frozen_peaks",
    "minecraft:jagged_peaks", "minecraft:ice_spikes", "minecraft:stony_shore",
    "minecraft:cold_ocean", "minecraft:deep_cold_ocean", "minecraft:taiga",
    "minecraft:old_growth_pine_taiga", "minecraft:old_growth_spruce_taiga",
    "minecraft:windswept_hills", "minecraft:windswept_gravelly_hills",
    "minecraft:windswept_forest", "minecraft:stony_peaks", "minecraft:meadow",
}


def unpack(longs, bits, count):
    per = 64 // bits
    mask = (1 << bits) - 1
    out = []
    for word in longs:
        word &= (1 << 64) - 1
        for k in range(per):
            if len(out) == count:
                return out
            out.append((word >> (k * bits)) & mask)
    return out


def state_name(entry):
    name = entry["Name"]
    props = entry.get("Properties")
    if props:
        name += "[" + ",".join(f"{k}={v}" for k, v in sorted(props.items())) + "]"
    return name


class Column:
    def __init__(self, chunk):
        self.sections = {s["Y"]: s for s in chunk.get("sections", [])}
        self.blocks = {}
        self.biomes = {}
        self.light = {}

    def section_blocks(self, sy):
        if sy not in self.blocks:
            s = self.sections.get(sy)
            states = s.get("block_states") if s else None
            if not states:
                self.blocks[sy] = None
            else:
                palette = [state_name(e) for e in states["palette"]]
                if len(palette) == 1:
                    self.blocks[sy] = [palette[0]] * 4096
                else:
                    bits = max(4, (len(palette) - 1).bit_length())
                    self.blocks[sy] = [palette[i] for i in unpack(states["data"], bits, 4096)]
        return self.blocks[sy]

    def block(self, x, y, z):
        b = self.section_blocks(y >> 4)
        return "minecraft:air" if b is None else b[((y & 15) * 16 + z) * 16 + x]

    def biome(self, qx, qy, qz):
        sy = qy >> 2
        if sy not in self.biomes:
            s = self.sections.get(sy)
            bio = s.get("biomes") if s else None
            if not bio:
                self.biomes[sy] = None
            else:
                palette = bio["palette"]
                if len(palette) == 1:
                    self.biomes[sy] = [palette[0]] * 64
                else:
                    bits = (len(palette) - 1).bit_length()
                    self.biomes[sy] = [palette[i] for i in unpack(bio["data"], bits, 64)]
        b = self.biomes[sy]
        if b is None:
            return None
        return b[((qy & 3) * 4 + qz) * 4 + qx]

    def block_light(self, x, y, z):
        sy = y >> 4
        if sy not in self.light:
            s = self.sections.get(sy)
            arr = s.get("BlockLight") if s else None
            self.light[sy] = arr
        arr = self.light[sy]
        if not arr:
            return 0
        i = ((y & 15) * 16 + z) * 16 + x
        byte = arr[i >> 1] & 0xFF
        return (byte >> 4) if (i & 1) else (byte & 15)


def main():
    world, out_path = sys.argv[1], sys.argv[2]
    written = 0
    with open(out_path, "w") as out:
        for path in sorted(glob.glob(os.path.join(world, "region", "*.mca"))):
            rx, rz = map(int, os.path.basename(path).split(".")[1:3])
            for lx, lz, chunk in anvil_read.chunks(path):
                if chunk.get("Status") != "minecraft:full":
                    continue
                cx, cz = rx * 32 + lx, rz * 32 + lz
                heights = chunk.get("Heightmaps", {}).get("MOTION_BLOCKING")
                if not heights:
                    continue
                tops = unpack(heights, 9, 256)
                col = Column(chunk)
                for i, v in enumerate(tops):
                    x, z = i & 15, i >> 4
                    top = MIN_Y + v
                    qx, qy, qz = x >> 2, (top - MIN_Y) >> 2, z >> 2
                    # Section-relative quart y: the biome array is per section.
                    biome = col.biome(qx, (top >> 2), qz)
                    if biome is None:
                        continue
                    if biome not in COLD and top <= 100 and (x + z * 16) % 16 != 0:
                        continue
                    uniform = 1
                    for dqx in (-1, 0, 1):
                        for dqz in (-1, 0, 1):
                            for dqy in (-1, 0, 1):
                                ax, az = qx + dqx, qz + dqz
                                if not (0 <= ax < 4 and 0 <= az < 4):
                                    uniform = 0  # the neighbour chunk is not read; be strict
                                    continue
                                if col.biome(ax, (top >> 2) + dqy, az) != biome:
                                    uniform = 0
                    out.write(f"{cx * 16 + x}\t{cz * 16 + z}\t{top}\t{col.block(x, top, z)}\t"
                              f"{col.block(x, top - 1, z)}\t{biome}\t{uniform}\t"
                              f"{col.block_light(x, top, z)}\t{col.block_light(x, top - 1, z)}\n")
                    written += 1
    print(f"{written} columns written to {out_path}")


if __name__ == "__main__":
    main()
