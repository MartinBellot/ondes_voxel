# The server: the tick loop, the world it owns, and the protocol it speaks.
#
# A library rather than an application, so that the client can host one on a
# thread. Nothing above the rendering frontier appears here, and the CI job
# that builds without a Vulkan SDK is what keeps it that way.
ov_add_library(ov_server
    LAYER 12
    SOURCES
        src/agriculture.cpp
        src/async_chunk_source.cpp
        src/block_container.cpp
        src/item_transport.cpp
        src/generated_world.cpp
        src/generation_check.cpp
        src/husbandry.cpp
        src/natural_spawning.cpp
        src/nether_travel.cpp
        src/end_travel.cpp
        src/end_fight.cpp
        src/player_inventory.cpp
        src/server.cpp
        src/combat_session.cpp
        src/effect_session.cpp
        src/mob_combat.cpp
        src/player_level.cpp
        src/player_data.cpp
        src/projectiles.cpp
        src/sounds.cpp
        src/survival_session.cpp
        src/tnt_gravity.cpp
        src/workbench.cpp
        src/world_ticks.cpp
        # ── commands and chat ──
        src/commands/arguments.cpp
        src/commands/console.cpp
        src/commands/context.cpp
        src/commands/dispatcher.cpp
        src/commands/env.cpp
        src/commands/game_commands.cpp
        src/commands/game_rules.cpp
        src/commands/json.cpp
        src/commands/names.cpp
        src/commands/ops.cpp
        src/commands/selector.cpp
        src/commands/service.cpp
        src/commands/snbt.cpp
        src/commands/string_reader.cpp
        src/commands/suggestions.cpp
        src/commands/text.cpp
        src/commands/world_state.cpp
    DEPS
        ov_base
        ov_gameplay
        ov_io
        ov_math
        ov_nbt
        ov_protocol
        ov_registry
        ov_world
        ov_worldgen
    PRIVATE_DEPS
        fmt::fmt
        # simdjson stays private, as everywhere else: a template-heavy header
        # must not reach an installed one. Only natural_spawning.cpp uses it,
        # to read the biome spawner lists out of the data generator output.
        simdjson::simdjson
)

# The units that can be tested without a socket. The server itself is a loop
# around a world and cannot be unit-tested; the pieces this wave added to it —
# the player's own window and the day's clock — can be, and both of them were
# bugs found the hard way.
ov_add_test(test_ov_server
    SOURCES
        tests/test_agriculture.cpp
        tests/test_block_container.cpp
        tests/test_player_inventory.cpp
        tests/test_player_level.cpp
        tests/test_player_data.cpp
        tests/test_sounds.cpp
        tests/test_nether_travel.cpp
        tests/test_end_fight.cpp
    DEPS ov_server
    LABELS unit
)

# ── commands and chat ──
# The command engine, without a socket: parsers, selectors, the dispatcher's
# errors and cursors, and every command run against a fake server.
ov_add_test(test_ov_commands
    SOURCES
        tests/test_commands.cpp
    DEPS ov_server
    LABELS unit
)

# Where the locally generated vanilla data lives. Passed in rather than searched
# for at runtime: the server must fail loudly with a path in the message, not
# quietly with an empty world.
target_compile_definitions(ov_server PRIVATE OV_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
