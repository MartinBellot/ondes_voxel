#include "ov/world/level_dat.hpp"

#include "ov/io/compression.hpp"
#include "ov/world/chunk_storage.hpp"

namespace ov::world {
namespace {

/// The Anvil level format's own version number, unchanged since 1.9. Not the
/// same as DataVersion, and a file carrying only one of the two is rejected.
constexpr i32 kAnvilLevelVersion = 19133;

void put(nbt::Tag& compound, std::string name, nbt::Tag value) {
    compound.compound()->push_back(nbt::CompoundEntry{std::move(name), std::move(value)});
}

[[nodiscard]] nbt::Tag flat_generator(const LevelSettings& settings) {
    nbt::Tag layers = nbt::Tag::make_list(nbt::TagType::Compound);
    for (const FlatLayer& layer : settings.layers) {
        nbt::Tag entry = nbt::Tag::make_compound();
        put(entry, "block", nbt::Tag{layer.block});
        put(entry, "height", nbt::Tag{layer.height});
        layers.list()->push_back(std::move(entry));
    }

    nbt::Tag flat = nbt::Tag::make_compound();
    put(flat, "biome", nbt::Tag{settings.biome});
    put(flat, "features", nbt::Tag::make_bool(false));
    put(flat, "lakes", nbt::Tag::make_bool(false));
    put(flat, "layers", std::move(layers));
    put(flat, "structure_overrides", nbt::Tag::make_list(nbt::TagType::String));

    nbt::Tag generator = nbt::Tag::make_compound();
    put(generator, "type", nbt::Tag{std::string{"minecraft:flat"}});
    put(generator, "settings", std::move(flat));
    return generator;
}

/// One dimension entry. The generator has to match what actually produced the
/// stored chunks: a flat world declared as noise grows ordinary terrain the
/// moment a player walks past the saved region, with a visible wall where the
/// two meet.
[[nodiscard]] nbt::Tag dimension(std::string_view type, nbt::Tag generator) {
    nbt::Tag entry = nbt::Tag::make_compound();
    put(entry, "type", nbt::Tag{std::string{type}});
    put(entry, "generator", std::move(generator));
    return entry;
}

[[nodiscard]] nbt::Tag noise_generator(std::string_view preset) {
    nbt::Tag generator = nbt::Tag::make_compound();
    put(generator, "type", nbt::Tag{std::string{"minecraft:noise"}});
    put(generator, "settings", nbt::Tag{std::string{preset}});

    nbt::Tag source = nbt::Tag::make_compound();
    put(source, "type", nbt::Tag{std::string{"minecraft:fixed"}});
    put(source, "biome", nbt::Tag{std::string{"minecraft:plains"}});
    put(generator, "biome_source", std::move(source));
    return generator;
}

}  // namespace

nbt::Document make_level_dat(const LevelSettings& settings) {
    nbt::Document document;
    document.name = "";
    document.root = nbt::Tag::make_compound();

    nbt::Tag data = nbt::Tag::make_compound();

    // Both versions are required and they are different things: one names the
    // Anvil layout, the other the game build.
    put(data, "version", nbt::Tag{kAnvilLevelVersion});
    put(data, "DataVersion", nbt::Tag{kDataVersion1201});

    nbt::Tag version = nbt::Tag::make_compound();
    put(version, "Id", nbt::Tag{kDataVersion1201});
    put(version, "Name", nbt::Tag{std::string{"1.20.1"}});
    put(version, "Series", nbt::Tag{std::string{"main"}});
    put(version, "Snapshot", nbt::Tag::make_bool(false));
    put(data, "Version", std::move(version));

    put(data, "LevelName", nbt::Tag{settings.name});
    put(data, "LastPlayed", nbt::Tag{i64{0}});
    put(data, "GameType", nbt::Tag{settings.game_type});
    put(data, "Difficulty", nbt::Tag{i8{2}});
    put(data, "DifficultyLocked", nbt::Tag::make_bool(false));
    put(data, "hardcore", nbt::Tag::make_bool(false));
    put(data, "allowCommands", nbt::Tag::make_bool(true));

    // Without this the game treats the world as freshly created and runs its
    // own spawn search, which for a saved world means moving the player.
    put(data, "initialized", nbt::Tag::make_bool(true));

    put(data, "SpawnX", nbt::Tag{settings.spawn_x});
    put(data, "SpawnY", nbt::Tag{settings.spawn_y});
    put(data, "SpawnZ", nbt::Tag{settings.spawn_z});
    put(data, "SpawnAngle", nbt::Tag{0.0F});

    put(data, "Time", nbt::Tag{i64{0}});
    put(data, "DayTime", nbt::Tag{i64{1000}});
    put(data, "clearWeatherTime", nbt::Tag{i32{0}});
    put(data, "rainTime", nbt::Tag{i32{0}});
    put(data, "thunderTime", nbt::Tag{i32{0}});
    put(data, "raining", nbt::Tag::make_bool(false));
    put(data, "thundering", nbt::Tag::make_bool(false));
    put(data, "WanderingTraderSpawnChance", nbt::Tag{i32{0}});
    put(data, "WanderingTraderSpawnDelay", nbt::Tag{i32{0}});

    // The world border. Its defaults are not zero, and a border of size zero
    // kills anything that spawns.
    put(data, "BorderCenterX", nbt::Tag{0.0});
    put(data, "BorderCenterZ", nbt::Tag{0.0});
    put(data, "BorderSize", nbt::Tag{59999968.0});
    put(data, "BorderSizeLerpTarget", nbt::Tag{59999968.0});
    put(data, "BorderSizeLerpTime", nbt::Tag{i64{0}});
    put(data, "BorderSafeZone", nbt::Tag{5.0});
    put(data, "BorderWarningBlocks", nbt::Tag{5.0});
    put(data, "BorderWarningTime", nbt::Tag{15.0});
    put(data, "BorderDamagePerBlock", nbt::Tag{0.2});

    nbt::Tag dimensions = nbt::Tag::make_compound();
    put(dimensions, "minecraft:overworld",
        dimension("minecraft:overworld", flat_generator(settings)));
    put(dimensions, "minecraft:the_nether",
        dimension("minecraft:the_nether", noise_generator("minecraft:nether")));
    put(dimensions, "minecraft:the_end",
        dimension("minecraft:the_end", noise_generator("minecraft:end")));

    nbt::Tag worldgen = nbt::Tag::make_compound();
    put(worldgen, "seed", nbt::Tag{settings.seed});
    put(worldgen, "generate_features", nbt::Tag::make_bool(false));
    put(worldgen, "bonus_chest", nbt::Tag::make_bool(false));
    put(worldgen, "dimensions", std::move(dimensions));
    put(data, "WorldGenSettings", std::move(worldgen));

    nbt::Tag packs   = nbt::Tag::make_compound();
    nbt::Tag enabled = nbt::Tag::make_list(nbt::TagType::String);
    enabled.list()->push_back(nbt::Tag{std::string{"vanilla"}});
    put(packs, "Enabled", std::move(enabled));
    put(packs, "Disabled", nbt::Tag::make_list(nbt::TagType::String));
    put(data, "DataPacks", std::move(packs));

    // Required even in a world with no End. The vanilla server named this one
    // itself — "key missing: DragonFight" — which is a far better error than
    // most, and worth the reminder that the game is the cheapest test oracle
    // available.
    nbt::Tag dragon = nbt::Tag::make_compound();
    put(dragon, "Gateways", nbt::Tag{nbt::Tag::IntArray{}});
    put(dragon, "DragonKilled", nbt::Tag::make_bool(false));
    put(dragon, "PreviouslyKilled", nbt::Tag::make_bool(false));
    put(dragon, "NeedsStateScanning", nbt::Tag::make_bool(true));
    put(data, "DragonFight", std::move(dragon));

    put(data, "GameRules", nbt::Tag::make_compound());
    put(data, "ServerBrands", nbt::Tag::make_list(nbt::TagType::String));
    put(data, "ScheduledEvents", nbt::Tag::make_list(nbt::TagType::Compound));

    put(document.root, "Data", std::move(data));
    return document;
}

std::vector<u8> encode_level_dat(const LevelSettings& settings) {
    // gzip, not zlib. Chunks inside a region use zlib and nothing announces the
    // difference; a level.dat written with the wrong container reads as corrupt.
    auto compressed = io::gzip_compress(nbt::write(make_level_dat(settings)));
    return compressed ? std::move(*compressed) : std::vector<u8>{};
}

}  // namespace ov::world
