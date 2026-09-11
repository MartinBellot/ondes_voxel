"""The entity scenes: one species a cell, summoned the same way for both clients.

Each cell is 32 blocks from the next along x, on a vanilla superflat world
(ground at y = −60), under a glass roof at y = −48 so that nothing that burns in
daylight catches fire while sky light stays 15. Every mob is summoned with NoAI,
facing south (yaw 0), towards a camera standing `distance` blocks south of it
and looking north.
"""

import math

GROUND = -60.0
EYE = 1.62
SPACING = 32

# name, summon (type and NBT, position filled in), distance, centre height the
# camera aims at, whether it expires before our client gets to it.
_MOB = "NoAI:1b,PersistenceRequired:1b"

RAW = [
    ("zombie", "zombie {%s}" % _MOB, 3.2, 1.0, False),
    ("zombie_armor", "zombie {%s,ArmorItems:[{id:\"iron_boots\",Count:1b},"
                     "{id:\"leather_leggings\",Count:1b},"
                     "{id:\"diamond_chestplate\",Count:1b},"
                     "{id:\"golden_helmet\",Count:1b}],"
                     "HandItems:[{id:\"iron_sword\",Count:1b},{}]}" % _MOB, 3.2, 1.0, False),
    ("baby_zombie", "zombie {%s,IsBaby:1b}" % _MOB, 2.4, 0.5, False),
    ("named_zombie", "zombie {%s,CustomName:'\"Hello\"',CustomNameVisible:1b}" % _MOB, 3.6, 1.4, False),
    ("dinnerbone", "zombie {%s,CustomName:'\"Dinnerbone\"'}" % _MOB, 3.2, 1.0, False),
    ("burning_zombie", "zombie {%s,HasVisualFire:1b}" % _MOB, 3.2, 1.0, False),
    ("husk", "husk {%s}" % _MOB, 3.2, 1.0, False),
    ("drowned", "drowned {%s}" % _MOB, 3.2, 1.0, False),
    ("skeleton", "skeleton {%s}" % _MOB, 3.2, 1.0, False),
    ("stray", "stray {%s}" % _MOB, 3.2, 1.0, False),
    ("wither_skeleton", "wither_skeleton {%s}" % _MOB, 3.8, 1.2, False),
    ("creeper", "creeper {%s}" % _MOB, 3.0, 0.9, False),
    ("charged_creeper", "creeper {%s,powered:1b}" % _MOB, 3.0, 0.9, False),
    ("spider", "spider {%s}" % _MOB, 3.6, 0.5, False),
    ("cave_spider", "cave_spider {%s}" % _MOB, 2.6, 0.3, False),
    ("enderman", "enderman {%s}" % _MOB, 4.6, 1.5, False),
    ("witch", "witch {%s}" % _MOB, 3.4, 1.1, False),
    ("slime", "slime {%s,Size:1}" % _MOB, 3.0, 0.5, False),
    ("magma_cube", "magma_cube {%s,Size:1}" % _MOB, 3.0, 0.5, False),
    ("cow", "cow {%s}" % _MOB, 3.4, 0.8, False),
    ("baby_cow", "cow {%s,Age:-24000}" % _MOB, 2.4, 0.4, False),
    ("pig", "pig {%s,Saddle:1b}" % _MOB, 3.0, 0.5, False),
    ("sheep_lime", "sheep {%s,Color:5b}" % _MOB, 3.2, 0.7, False),
    ("sheep_sheared", "sheep {%s,Sheared:1b}" % _MOB, 3.2, 0.7, False),
    ("chicken", "chicken {%s}" % _MOB, 2.4, 0.4, False),
    ("wolf", "wolf {%s}" % _MOB, 2.8, 0.5, False),
    ("cat", "cat {%s,variant:\"jellie\"}" % _MOB, 2.4, 0.4, False),
    ("fox", "fox {%s,Type:\"snow\"}" % _MOB, 2.6, 0.4, False),
    ("rabbit", "rabbit {%s,RabbitType:3}" % _MOB, 2.2, 0.3, False),
    ("horse", "horse {%s,Variant:515}" % _MOB, 4.8, 1.1, False),
    ("villager", "villager {%s,VillagerData:{type:\"plains\","
                 "profession:\"librarian\",level:3}}" % _MOB, 3.2, 1.0, False),
    ("villager_desert", "villager {%s,VillagerData:{type:\"desert\","
                        "profession:\"armorer\",level:5}}" % _MOB, 3.2, 1.0, False),
    ("baby_villager", "villager {%s,Age:-24000}" % _MOB, 2.4, 0.5, False),
    ("zombie_villager", "zombie_villager {%s,VillagerData:{type:\"taiga\","
                        "profession:\"farmer\",level:2}}" % _MOB, 3.2, 1.0, False),
    ("piglin", "piglin {%s,IsImmuneToZombification:1b}" % _MOB, 3.2, 1.0, False),
    ("zombified_piglin", "zombified_piglin {%s}" % _MOB, 3.2, 1.0, False),
    ("hoglin", "hoglin {%s,IsImmuneToZombification:1b}" % _MOB, 4.4, 0.8, False),
    ("blaze", "blaze {%s}" % _MOB, 3.6, 1.0, False),
    ("ghast", "ghast {%s}" % _MOB, 11.0, 2.0, False),
    ("strider", "strider {%s}" % _MOB, 3.6, 0.9, False),
    ("iron_golem", "iron_golem {%s}" % _MOB, 4.6, 1.4, False),
    ("minecart", "minecart {Rotation:[0f,0f]}", 2.8, 0.4, False),
    ("chest_minecart", "chest_minecart {Rotation:[0f,0f]}", 2.8, 0.5, False),
    ("furnace_minecart", "furnace_minecart {Rotation:[0f,0f]}", 2.8, 0.5, False),
    ("tnt_minecart", "tnt_minecart {Rotation:[0f,0f]}", 2.8, 0.5, False),
    ("hopper_minecart", "hopper_minecart {Rotation:[0f,0f]}", 2.8, 0.5, False),
    ("boat", "boat {Type:\"oak\",Rotation:[0f,0f]}", 3.6, 0.3, False),
    ("chest_boat", "chest_boat {Type:\"cherry\",Rotation:[0f,0f]}", 3.6, 0.4, False),
    ("armor_stand", "armor_stand {ShowArms:1b,Rotation:[0f,0f],"
                    "ArmorItems:[{},{},{},{id:\"iron_helmet\",Count:1b}]}", 3.2, 1.0, False),
    ("end_crystal", "end_crystal {ShowBottom:1b}", 3.6, 1.0, False),
    ("end_crystal_beam", "end_crystal {ShowBottom:0b,BeamTarget:{X:%d,Y:-56,Z:-3}}", 5.0, 2.0,
     False),
    ("tnt", "tnt {Fuse:32767s}", 2.6, 0.5, False),
    ("falling_block", "falling_block {BlockState:{Name:\"sand\"},NoGravity:1b,Time:1}",
     2.6, 0.5, True),
    ("experience_orb", "experience_orb {Value:20s,Age:0s}", 1.6, 0.2, True),
    ("item", "item {Item:{id:\"diamond\",Count:1b},PickupDelay:32767s,Age:-32768s}",
     1.6, 0.2, False),
    ("snowball", "snowball {NoGravity:1b,Motion:[0.0,0.0,0.0]}", 1.8, 0.3, True),
    ("dragon_fireball", "dragon_fireball {power:[0.0,0.0,0.0],Motion:[0.0,0.0,0.0]}", 4.0, 1.0, True),
    ("ender_dragon", "ender_dragon {NoAI:1b,Silent:1b,Rotation:[0f,0f]}", 22.0, 3.0, False),
    # Babies of every species with a head of its own, and a second held item.
    ("baby_pig", "pig {%s,Age:-24000}" % _MOB, 2.2, 0.3, False),
    ("baby_sheep", "sheep {%s,Age:-24000}" % _MOB, 2.2, 0.4, False),
    ("baby_chicken", "chicken {%s,Age:-24000}" % _MOB, 1.8, 0.2, False),
    ("baby_wolf", "wolf {%s,Age:-24000}" % _MOB, 2.2, 0.3, False),
    ("baby_cat", "cat {%s,Age:-24000,variant:\"jellie\"}" % _MOB, 1.8, 0.2, False),
    ("baby_fox", "fox {%s,Age:-24000}" % _MOB, 2.0, 0.2, False),
    ("baby_horse", "horse {%s,Age:-24000,Variant:515}" % _MOB, 3.0, 0.6, False),
    ("baby_rabbit", "rabbit {%s,Age:-24000,RabbitType:3}" % _MOB, 1.8, 0.2, False),
    ("baby_hoglin", "hoglin {%s,Age:-24000,IsImmuneToZombification:1b}" % _MOB, 2.8, 0.5, False),
    ("skeleton_bow", "skeleton {%s,HandItems:[{id:\"bow\",Count:1b},{}]}" % _MOB, 3.2, 1.0, False),
]


class Scene:
    def __init__(self, index, name, summon, distance, centre, ephemeral):
        self.index = index
        self.name = name
        self.x = SPACING * index + 0.5
        self.z = 0.5
        self.summon_nbt = summon
        self.distance = distance
        self.centre = centre
        self.ephemeral = ephemeral

    @property
    def summon(self):
        entity, _, nbt = self.summon_nbt.partition(" ")
        if "%d" in nbt:
            # A position inside the NBT (a crystal's beam target) is the cell's.
            nbt = nbt % int(self.x)
        command = "summon %s %.1f %.1f %.1f %s" % (entity, self.x, GROUND, self.z, nbt)
        # A player's command is at most 256 characters: past that the client
        # refuses to encode it and drops its own connection (the first session
        # lost every scene after the armoured zombie to exactly that).
        assert len(command) < 250, "summon too long for a player command: " + self.name
        return command

    @property
    def camera(self):
        """Feet position and angles: south of the cell, looking north and down
        at the entity's centre."""
        pitch = math.degrees(math.atan2(EYE - self.centre, self.distance))
        return (self.x, GROUND, self.z + self.distance, 180.0, round(pitch, 2))


SCENES = [Scene(i, *row) for i, row in enumerate(RAW)]


def by_name():
    return {scene.name: scene for scene in SCENES}
