// Villagers: identity against the registries, the thirteen job blocks, the
// day, and — in a real entity world — claiming a lectern, taking the
// profession, losing it, and running from a zombie.
#include "ov/gameplay/breeding.hpp"
#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/trading.hpp"
#include "ov/gameplay/villager.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <utility>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] std::filesystem::path data_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1";
}

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(data_path() / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(data_path() / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

/// Grass below y = 0, air above, and a few placed blocks.
class Village final : public world::LevelView {
public:
    explicit Village(const registry::BlockRegistry& registry) : registry_{&registry} {
        grass_ = registry.default_state(registry.find_block("minecraft:grass_block").value());
    }
    void place(BlockPos pos, std::string_view block) {
        placed_.emplace_back(pos, registry_->default_state(registry_->find_block(block).value()));
    }
    void clear(BlockPos pos) {
        std::erase_if(placed_, [&](const auto& p) { return p.first == pos; });
    }
    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        for (const auto& [at, state] : placed_) {
            if (at == pos) {
                return state;
            }
        }
        return pos.y < 0 ? grass_ : registry::kAirState;
    }
    [[nodiscard]] bool              is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

private:
    const registry::BlockRegistry*                          registry_;
    registry::BlockStateId                                  grass_{};
    std::vector<std::pair<BlockPos, registry::BlockStateId>> placed_;
};

struct Floor {
    registry::BlockStateId grass{};
    static registry::BlockStateId look_up(void* context, i32, i32 y, i32) {
        return y < 0 ? static_cast<const Floor*>(context)->grass : registry::BlockStateId{0};
    }
};

struct Town {
    Floor               floor;
    CollisionWorld      collisions;
    Village             level;
    entity::EntityWorld world;
    VillagerWorld       time;
    std::vector<HostileSight> hostiles;
    i64                 tick{0};

    Town()
        : floor{blocks()->default_state(blocks()->find_block("minecraft:grass_block").value())},
          collisions{*blocks(), &Floor::look_up, &floor},
          level{*blocks()},
          world{*registries()} {
        time.day_time = 6000;  // noon: work
    }

    entity::EntityHandle villager(Vec3d at) {
        const auto handle = world.spawn("minecraft:villager", at, net::Uuid{}).value();
        entity::EntityState* state = world.mutable_state(handle);
        world.set_logic(handle, std::make_unique<Mob>(*mob_kind("minecraft:villager"), state->width,
                                                      state->height, state->network_id));
        return handle;
    }
    VillagerState& state_of(entity::EntityHandle handle) {
        return dynamic_cast<Mob*>(world.logic(handle))->mutable_brain().villager;
    }
    void step(i32 ticks = 1) {
        for (i32 i = 0; i < ticks; ++i) {
            time.game_time = tick;
            time.hostiles  = hostiles;
            MobContext context{&collisions, &level, true};
            context.villagers = &time;
            world.tick(entity::TickContext{tick++, &context});
        }
    }
};

}  // namespace

TEST_CASE("villager types and professions are in registry order", "[villager][parity]") {
    const registry::Registries* r = registries();
    REQUIRE(r != nullptr);
    const auto professions = r->find("minecraft:villager_profession");
    const auto types       = r->find("minecraft:villager_type");
    REQUIRE(professions);
    REQUIRE(types);
    for (usize p = 0; p < kProfessionCount; ++p) {
        const std::string_view name = profession_name(static_cast<Profession>(p));
        CAPTURE(name);
        CHECK(r->protocol_id(*professions, name) == static_cast<registry::ProtocolId>(p));
        CHECK(profession_from_name(name) == static_cast<Profession>(p));
    }
    for (usize t = 0; t < kVillagerTypeCount; ++t) {
        const std::string_view name = villager_type_name(static_cast<VillagerType>(t));
        CAPTURE(name);
        CHECK(r->protocol_id(*types, name) == static_cast<registry::ProtocolId>(t));
    }
}

TEST_CASE("the thirteen job blocks, each measured", "[villager][parity]") {
    // measure_villagers.py `claim`: a jobless villager three blocks from each
    // took exactly this profession, 13 of 13.
    const std::array<std::pair<std::string_view, Profession>, 13> kMeasured{{
        {"minecraft:blast_furnace", Profession::Armorer},
        {"minecraft:smoker", Profession::Butcher},
        {"minecraft:cartography_table", Profession::Cartographer},
        {"minecraft:brewing_stand", Profession::Cleric},
        {"minecraft:composter", Profession::Farmer},
        {"minecraft:barrel", Profession::Fisherman},
        {"minecraft:fletching_table", Profession::Fletcher},
        {"minecraft:cauldron", Profession::Leatherworker},
        {"minecraft:lectern", Profession::Librarian},
        {"minecraft:stonecutter", Profession::Mason},
        {"minecraft:loom", Profession::Shepherd},
        {"minecraft:smithing_table", Profession::Toolsmith},
        {"minecraft:grindstone", Profession::Weaponsmith},
    }};
    for (const auto& [block, profession] : kMeasured) {
        CAPTURE(block);
        CHECK(profession_of_job_block(block) == profession);
        CHECK(blocks()->find_block(block).has_value());
    }
    CHECK(profession_of_job_block("minecraft:water_cauldron") == Profession::Leatherworker);
    CHECK_FALSE(profession_of_job_block("minecraft:crafting_table").has_value());
    CHECK(job_block_of(Profession::Nitwit).empty());
}

TEST_CASE("a villager's day", "[villager]") {
    CHECK(activity_at(0) == Activity::Rest);
    CHECK(activity_at(10) == Activity::Idle);
    CHECK(activity_at(2000) == Activity::Work);
    CHECK(activity_at(8999) == Activity::Work);
    CHECK(activity_at(9000) == Activity::Meet);
    CHECK(activity_at(11000) == Activity::Idle);
    CHECK(activity_at(12000) == Activity::Rest);
    CHECK(activity_at(24000 + 3000) == Activity::Work);
}

TEST_CASE("a change of profession undraws the offers", "[villager]") {
    VillagerState v;
    v.profession = Profession::Librarian;
    ensure_offers(v);
    REQUIRE_FALSE(v.offers.empty());
    const u32 before = v.revision;
    set_profession(v, Profession::Farmer);
    CHECK(v.offers.empty());
    CHECK_FALSE(v.offers_drawn);
    CHECK(v.revision == before + 1);
}

TEST_CASE("a jobless villager claims a lectern and becomes a librarian", "[villager]") {
    Town town;
    town.level.place(BlockPos{3, 0, 0}, "minecraft:lectern");
    const auto handle = town.villager(Vec3d{0.5, 0.0, 0.5});
    VillagerState& v  = town.state_of(handle);
    CHECK(v.active);
    CHECK(v.profession == Profession::None);
    i32 ticks = 0;
    while (v.profession == Profession::None && ticks < 600) {
        town.step();
        ++ticks;
    }
    // Measured on the real server: at three blocks, employed within 35 ticks.
    CHECK(v.profession == Profession::Librarian);
    CHECK(v.claims.job_site == BlockPos{3, 0, 0});
    CHECK(ticks < 200);

    // A second villager does not take the same lectern.
    const auto other = town.villager(Vec3d{0.5, 0.0, 4.5});
    town.step(300);
    CHECK(town.state_of(other).profession == Profession::None);
}

TEST_CASE("the job is lost with its block, unless something was traded", "[villager][parity]") {
    Town town;
    town.level.place(BlockPos{3, 0, 0}, "minecraft:lectern");
    town.level.place(BlockPos{3, 0, 20}, "minecraft:lectern");
    const auto a = town.villager(Vec3d{0.5, 0.0, 0.5});
    const auto b = town.villager(Vec3d{0.5, 0.0, 20.5});
    town.step(300);
    REQUIRE(town.state_of(a).profession == Profession::Librarian);
    REQUIRE(town.state_of(b).profession == Profession::Librarian);
    town.state_of(b).xp = 5;
    town.level.clear(BlockPos{3, 0, 0});
    town.level.clear(BlockPos{3, 0, 20});
    town.step(2);
    // Measured: the untraded one had no job 3 ticks after its lectern went;
    // the one with Xp 5 kept it for the minute watched.
    CHECK(town.state_of(a).profession == Profession::None);
    CHECK(town.state_of(b).profession == Profession::Librarian);
}

TEST_CASE("a villager runs from a zombie within eight blocks", "[villager]") {
    Town town;
    const auto zombie_type = registries()->protocol_id(
        registries()->find("minecraft:entity_type").value(), "minecraft:zombie");
    REQUIRE(zombie_type);
    town.hostiles.push_back(HostileSight{static_cast<i32>(*zombie_type), 8.0F});
    const auto villager = town.villager(Vec3d{0.5, 0.0, 0.5});
    // A zombie with no behaviour: it stands where it is put.
    (void)town.world.spawn("minecraft:zombie", Vec3d{5.5, 0.0, 0.5}, net::Uuid{}).value();
    town.step(60);
    const entity::EntityState* s = town.world.state(villager);
    REQUIRE(s != nullptr);
    // Measured: from 5 blocks, 12 blocks away in 7 seconds, straight off.
    CHECK(s->position.x < -2.0);
}
