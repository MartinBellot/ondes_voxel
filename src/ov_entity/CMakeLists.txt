# Entity storage and behaviour.
#
# Depends on ov_registry for the entity type table — hitbox, eye height and
# attribute base values, all measured on a real 1.20.1 server — and on
# ov_protocol for the wire types an entity carries (its uuid).
#
# EnTT is a PRIVATE_DEP and stays one. It is a template library heavy enough
# that putting a registry in a public header would land its instantiation in
# every translation unit that spawns anything; see risk R5 in
# docs/ARCHITECTURE.md. The registry lives behind a PIMPL in world.cpp, and
# nothing above this module can name it.
ov_add_library(ov_entity
    LAYER 8
    SOURCES
        src/world.cpp
    DEPS
        ov_math
        ov_protocol
        ov_registry
    PRIVATE_DEPS
        EnTT::EnTT
)

ov_add_test(test_ov_entity
    SOURCES
        tests/test_world.cpp
        tests/test_entity_types.cpp
    DEPS ov_entity simdjson::simdjson
    LABELS unit
)
