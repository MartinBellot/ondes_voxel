# Reads a player file the way the server does, and writes it back.
#
# Exists so that scripts/measure_player_data.py can hand a file the real server
# wrote to our reader and writer, and give the result back to the real server.
# Not part of the game.
ov_add_executable(ov_playerdata
    SOURCES
        src/main.cpp
    DEPS
        ov_server
)
