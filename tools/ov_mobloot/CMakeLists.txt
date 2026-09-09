# Rolls entity loot tables, many times, and prints what came out.
#
# Exists so that scripts/check_entity_loot.py has something to compare the real
# server against. Not part of the game; links only the rules and the registry.
ov_add_executable(ov_mobloot
    SOURCES
        src/main.cpp
    DEPS
        ov_gameplay
        ov_registry
)
