# The renderer's data half: resource packs, block models, baking, vertex
# packing and smooth lighting.
#
# None of it needs a GPU, and that is the point. Vanilla's terrain is decided by
# JSON — blockstate to variant to model to parent chain to elements to faces —
# and every bug in that chain is a bug you can catch with a unit test instead of
# by staring at a screenshot. The Vulkan half lands in ov_rhi below it.
#
# simdjson is a PRIVATE_DEP on purpose: it is exactly the template-heavy header
# CLAUDE.md § 5 keeps out of public headers, and it appears in src/json.cpp and
# nowhere else.
# libspng is PRIVATE for the same reason, plus one of its own: it is a C header
# full of unprefixed macros, and it belongs to src/png.cpp alone. vcpkg names
# the target after the linkage it built.
find_package(SPNG CONFIG REQUIRED)
if(TARGET spng::spng)
    set(OV_SPNG_TARGET spng::spng)
else()
    set(OV_SPNG_TARGET spng::spng_static)
endif()

ov_add_library(ov_render
    LAYER 14
    SOURCES
        src/asset_path.cpp
        src/asset_source.cpp
        src/atlas.cpp
        src/bake.cpp
        src/biome_colours.cpp
        src/block_models.cpp
        src/camera.cpp
        src/entity_mesh.cpp
        src/entity_model.cpp
        src/entity_pose.cpp
        src/environment.cpp
        src/font.cpp
        src/chunk_mesher.cpp
        src/crack_mesh.cpp  # ── breaking ──
        src/block_shape.cpp  # ── breaking ──
        src/creative_items.cpp
        src/creative_tabs.cpp
        src/frustum.cpp
        src/item_model.cpp
        src/block_state_model.cpp
        src/json.cpp
        src/language.cpp
        src/mesher.cpp
        src/model.cpp
        src/png.cpp
        src/precipitation.cpp
        src/terrain_vertex.cpp
        src/text_component.cpp
        src/texture_image.cpp
        src/vertex_arena.cpp
    DEPS
        ov_io
        ov_math
        ov_registry
        ov_world
    PRIVATE_DEPS
        simdjson::simdjson
        ${OV_SPNG_TARGET}
)

ov_add_test(test_ov_render
    SOURCES
        tests/test_ambient_occlusion.cpp
        tests/test_asset_path.cpp
        tests/test_asset_source.cpp
        tests/test_atlas.cpp
        tests/test_bake.cpp
        tests/test_biome_colours.cpp
        tests/test_block_state_model.cpp
        tests/test_camera.cpp
        tests/test_entity_model.cpp
        tests/test_environment.cpp
        tests/test_font.cpp
        tests/test_chunk_mesher.cpp
        tests/test_creative_tabs.cpp
        tests/test_frustum.cpp
        tests/test_crack_mesh.cpp  # ── breaking ──
        tests/test_block_shape.cpp  # ── breaking ──
        tests/test_mesher.cpp
        tests/test_item_model.cpp
        tests/test_json.cpp
        tests/test_model.cpp
        tests/test_png.cpp
        tests/test_precipitation.cpp
        tests/test_terrain_vertex.cpp
        tests/test_text_component.cpp
        tests/test_vertex_arena.cpp
    DEPS ov_render
    LABELS unit
)
