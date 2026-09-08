"""Place every block on a real 1.20.1 server and read back the heightmaps it wrote.

The quantity we need is "does this block stop movement" — the predicate behind
MOTION_BLOCKING and OCEAN_FLOOR. Rather than infer it, we let the game answer:
put the block on top of a column, let the server save, and read which of its own
heightmaps it decided the column's top belongs to.

Four zones, because a block has to survive being placed before it can be read:
  A  bare on grass          — everything that stands on its own
  B  spaced out, wet        — the blocks that carry water with them, which would
                              otherwise flood their neighbours for seven blocks
  C  walled in stone        — wall torches, ladders, buttons, signs
  D  on a chosen substrate  — crops on farmland, cactus on sand, chorus on end stone
"""
import json, sys, time

FIFO, WORLD = (sys.argv[1], sys.argv[2]) if len(sys.argv) > 2 else (None, None)
REG = json.load(open('data/vanilla/1.20.1/normalized/registries.json'))
BLOCKS = REG['registries']['minecraft:block']['entries']

# Blocks that are a fluid, or that rewrite their surroundings when placed. They
# are not skipped for convenience: a flowing block does not stay where it was
# put, so the column it was meant to answer for is not the column that gets read.
DYNAMIC = {"minecraft:water", "minecraft:lava", "minecraft:fire", "minecraft:soul_fire",
           "minecraft:bubble_column", "minecraft:moving_piston", "minecraft:nether_portal",
           "minecraft:end_portal", "minecraft:end_gateway",
           "minecraft:air", "minecraft:cave_air", "minecraft:void_air"}

# Blocks whose own state contains water. Measured, not assumed: these are exactly
# the ones zone A found with MOTION_BLOCKING above OCEAN_FLOOR.
WET = {"minecraft:seagrass", "minecraft:tall_seagrass", "minecraft:kelp", "minecraft:kelp_plant",
       "minecraft:sea_pickle"} | {
    f"minecraft:{c}_coral{s}" for c in ("tube", "brain", "bubble", "fire", "horn")
    for s in ("", "_fan")}

SUBSTRATE = {
    "minecraft:dead_bush": "minecraft:sand",
    "minecraft:cactus": "minecraft:sand",
    "minecraft:sugar_cane": "minecraft:sand",
    "minecraft:brown_mushroom": "minecraft:podzol",
    "minecraft:red_mushroom": "minecraft:podzol",
    "minecraft:nether_wart": "minecraft:soul_sand",
    "minecraft:chorus_plant": "minecraft:end_stone",
    "minecraft:chorus_flower": "minecraft:end_stone",
    "minecraft:small_dripleaf": "minecraft:moss_block",
    "minecraft:big_dripleaf_stem": "minecraft:moss_block",
    "minecraft:bamboo_sapling": "minecraft:sand",
    "minecraft:twisting_vines": "minecraft:warped_nylium",
    "minecraft:twisting_vines_plant": "minecraft:warped_nylium",
    "minecraft:crimson_roots": "minecraft:crimson_nylium",
}
for crop in ("wheat", "carrots", "potatoes", "beetroots", "pumpkin_stem", "melon_stem",
             "attached_pumpkin_stem", "attached_melon_stem", "torchflower_crop",
             "pitcher_crop"):
    SUBSTRATE[f"minecraft:{crop}"] = "minecraft:farmland"

def send(lines):
    with open(FIFO, 'w') as f:
        f.write("".join(l + "\n" for l in lines))

def wait_saved(log, count):
    for _ in range(600):
        if open(log, errors='ignore').read().count("Saved the game") >= count:
            return True
        time.sleep(0.5)
    return False

LOG = (WORLD + "/../log.txt") if WORLD else None

def zone(name, origin_x, spacing, names, decorate):
    """Lay one zone out and return {block: (x, z)}."""
    plan, cmds = {}, []
    side = 40
    for i, b in enumerate(names):
        x = origin_x + (i % side) * spacing
        z = (i // side) * spacing
        plan[b] = (x, z)
        cmds += decorate(b, x, z)
        cmds.append(f"setblock {x} -60 {z} {b} replace")
    return plan, cmds

def main():
    dry = [b for b in BLOCKS if b not in DYNAMIC and b not in WET]
    wet = [b for b in BLOCKS if b in WET]

    send(["gamerule randomTickSpeed 0", "gamerule doFireTick false",
          "gamerule doDaylightCycle false", "gamerule doMobSpawning false",
          "gamerule doWeatherCycle false", "time set noon"])
    time.sleep(1)

    plans = {}
    plans['A'], a = zone('A', 0, 2, dry, lambda b, x, z: [])
    plans['B'], b_ = zone('B', 3000, 16, wet, lambda b, x, z: [])
    plans['C'], c = zone('C', 6000, 4, dry, lambda b, x, z: [
        f"setblock {x+1} -60 {z} minecraft:stone replace",
        f"setblock {x-1} -60 {z} minecraft:stone replace",
        f"setblock {x} -60 {z+1} minecraft:stone replace",
        f"setblock {x} -60 {z-1} minecraft:stone replace"])
    subs = [b for b in BLOCKS if b in SUBSTRATE]
    plans['D'], d = zone('D', 9000, 4, subs,
                         lambda b, x, z: [f"setblock {x} -61 {z} {SUBSTRATE[b]} replace"])

    for label, cmds in (('A', a), ('B', b_), ('C', c), ('D', d)):
        # forceload, because a zone three thousand blocks out is nowhere near
        # any player and would otherwise never be ticked, saved, or even created.
        xs = [p[0] for p in plans[label].values()]
        zs = [p[1] for p in plans[label].values()]
        send([f"forceload add {min(xs)-2} {min(zs)-2} {max(xs)+2} {max(zs)+2}"])
        time.sleep(3)
        for i in range(0, len(cmds), 400):
            send(cmds[i:i + 400])
            time.sleep(0.4)
        send([f"forceload remove {min(xs)-2} {min(zs)-2} {max(xs)+2} {max(zs)+2}"])
        print(f"zone {label}: {len(plans[label])} blocs, {len(cmds)} commandes", flush=True)

    send(["save-all flush"])
    print("sauvegarde :", wait_saved(LOG, 1))
    json.dump({k: {b: list(v) for b, v in p.items()} for k, p in plans.items()},
              open(WORLD + "/../plan.json", "w"))

if __name__ == "__main__":
    main()
