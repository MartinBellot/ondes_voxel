# Rules the game plays by: breaking, placing, inventories.
#
# Depends on the registries and on nothing above them. A block behaviour that
# reached for the server's Level would make client-side prediction impossible,
# which is why this module cannot see one.
ov_add_library(ov_gameplay
    LAYER 9
    SOURCES
        src/breaking.cpp
        src/collision.cpp
        src/entity_physics.cpp
        src/mob_logic.cpp
        src/physics.cpp
        src/connections.cpp
        src/loot.cpp
    DEPS
        ov_entity
        ov_registry
        ov_math
)

ov_add_test(test_ov_gameplay
    SOURCES
        tests/test_breaking.cpp
        tests/test_collision.cpp
        tests/test_entity_physics.cpp
        tests/test_mob_logic.cpp
        tests/test_fluid_physics.cpp
        tests/test_physics.cpp
        tests/test_connections.cpp
        tests/test_loot.cpp
    DEPS ov_gameplay
    LABELS unit
)
