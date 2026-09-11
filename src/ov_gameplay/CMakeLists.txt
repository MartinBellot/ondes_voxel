# Rules the game plays by: breaking, placing, inventories.
#
# Depends on the registries and on nothing above them. A block behaviour that
# reached for the server's Level would make client-side prediction impossible,
# which is why this module cannot see one.
ov_add_library(ov_gameplay
    LAYER 9
    SOURCES
        src/attributes.cpp
        src/effects.cpp
        src/brewing.cpp
        src/breaking.cpp
        src/dig_controller.cpp
        src/combat.cpp
        src/collision.cpp
        src/damage.cpp
        src/durability.cpp
        src/enchanting.cpp
        src/entity_physics.cpp
        src/experience.cpp
        src/explosion.cpp
        src/falling_block.cpp
        src/primed_tnt.cpp
        src/projectile.cpp
        src/fluid.cpp
        src/item_use.cpp
        src/nether_portal.cpp
        src/end_portal.cpp
        src/dragon.cpp
        src/food.cpp
        src/mob_logic.cpp
        src/mob_species.cpp
        src/nether_mobs.cpp  # ── nether-2 ──
        src/mob_attack.cpp  # ── mobs-3 ──
        src/breeding.cpp
        src/tame.cpp  # ── tame ──
        src/pathfinding.cpp
        src/goals.cpp
        src/spawning.cpp
        src/spawn_rules.cpp
        src/slime.cpp
        src/conversion.cpp
        src/physics.cpp
        src/connections.cpp
        src/signal.cpp
        src/redstone.cpp
        src/rails.cpp
        src/minecart.cpp
        src/hopper.cpp
        src/dispenser.cpp
        src/piston.cpp
        src/plants.cpp
        src/fire.cpp
        src/loot.cpp
        src/recipe.cpp
        src/crafting.cpp
        src/smelting.cpp
        src/weather.cpp
        src/sleep.cpp
        src/villager.cpp
        src/trading.cpp
    DEPS
        ov_world
        ov_entity
        ov_registry
        ov_math
        ov_nbt
        ov_protocol
)

ov_add_test(test_ov_gameplay
    SOURCES
        tests/test_attributes.cpp
        tests/test_effects.cpp
        tests/test_brewing.cpp
        tests/test_breaking.cpp
        tests/test_dig_controller.cpp
        tests/test_combat.cpp
        tests/test_entity_loot.cpp
        tests/test_collision.cpp
        tests/test_damage.cpp
        tests/test_durability.cpp
        tests/test_enchanting.cpp
        tests/test_entity_physics.cpp
        tests/test_experience.cpp
        tests/test_nether_portal.cpp
        tests/test_dragon.cpp
        tests/test_explosion.cpp
        tests/test_falling_block.cpp
        tests/test_primed_tnt.cpp
        tests/test_projectile.cpp
        tests/test_fluid.cpp
        tests/test_item_use.cpp
        tests/test_food.cpp
        tests/test_mob_logic.cpp
        tests/test_mobs2.cpp
        tests/test_nether_mobs.cpp  # ── nether-2 ──
        tests/test_mobs3.cpp  # ── mobs-3 ──
        tests/test_breeding.cpp
        tests/test_tame.cpp  # ── tame ──
        tests/test_pathfinding.cpp
        tests/test_goals.cpp
        tests/test_spawning.cpp
        tests/test_fluid_physics.cpp
        tests/test_physics.cpp
        tests/test_connections.cpp
        tests/test_redstone.cpp
        tests/test_rails.cpp
        tests/test_minecart.cpp
        tests/test_hopper.cpp
        tests/test_dispenser.cpp
        tests/test_piston.cpp
        tests/test_plants.cpp
        tests/test_fire.cpp
        tests/test_loot.cpp
        tests/test_crafting.cpp
        tests/test_crafting_parity.cpp
        tests/test_smelting.cpp
        tests/test_weather.cpp
        tests/test_climate_oracle.cpp
        tests/test_end_portal.cpp
        tests/test_villager.cpp
        tests/test_trading.cpp
    DEPS ov_gameplay simdjson::simdjson
    LABELS unit
)
