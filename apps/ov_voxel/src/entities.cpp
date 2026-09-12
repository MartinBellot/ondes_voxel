#define OV_LOG_CATEGORY "voxel"

#include "entities.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <string>

namespace ov::demo {

namespace {

constexpr f32 kDegrees = 180.0F / std::numbers::pi_v<f32>;

/// Below this, a move is noise from the quantiser rather than a direction.
///
/// One quantum is 1/4096 = 0.000244 blocks. Ten of them is 0.0024, which a
/// walking mob covers in a tenth of a tick and a standing one never does.
constexpr f64 kFacingThreshold = 0.0024;

/// How long a Damage Event reddens an entity: ten ticks.
constexpr f64 kHurtSeconds = 0.5;
/// How long a death topples an entity before the client lets it go: twenty
/// ticks, which is when the game's own server removes a dead mob.
constexpr f64 kDeathSeconds = 1.0;

/// The overlay texel of a hurt or dying entity, as the running client's own
/// overlay texture has it (rows 0–7: red, alpha 178 — "overlay" in
/// data/vanilla/1.20.1/entity_java_models.json).
constexpr u32 kHurtOverlay = 0xB2FF0000U;
/// A primed TNT's flash: the white row at its whitest, alpha 63 (row 10,
/// column 15 of the same texture).
constexpr u32 kWhiteOverlay = 0x3FFFFFFFU;

/// A player's eye above its feet, standing.
constexpr f64 kEyeHeight = 1.62;

[[nodiscard]] f32 yaw_towards(f64 dx, f64 dz) noexcept {
    // Minecraft's convention, and it is not atan2(dz, dx). Yaw 0 looks towards
    // +Z and grows clockwise from above, so forward is (-sin, cos).
    return static_cast<f32>(std::atan2(-dx, dz)) * kDegrees;
}

[[nodiscard]] f32 wrap_degrees(f32 degrees) noexcept {
    f32 wrapped = std::fmod(degrees, 360.0F);
    if (wrapped > 180.0F) {
        wrapped -= 360.0F;
    }
    if (wrapped < -180.0F) {
        wrapped += 360.0F;
    }
    return wrapped;
}

[[nodiscard]] f32 lerp_angle(f32 from, f32 to, f32 alpha) noexcept {
    return from + wrap_degrees(to - from) * alpha;
}

/// A custom name's plain text, from its JSON: the `text` fields in order, or a
/// bare JSON string. Translated and scored components are not flattened here —
/// they need the language table — and come out empty, which draws no tag
/// rather than a wrong one.
[[nodiscard]] std::string plain_name(std::string_view json) {
    std::string out;
    if (json.size() >= 2 && json.front() == '"' && json.back() == '"') {
        return std::string(json.substr(1, json.size() - 2));
    }
    usize at = 0;
    for (;;) {
        const usize key = json.find("\"text\"", at);
        if (key == std::string_view::npos) {
            break;
        }
        const usize open = json.find('"', json.find(':', key) + 1);
        if (open == std::string_view::npos) {
            break;
        }
        usize close = open + 1;
        while (close < json.size() && json[close] != '"') {
            close += json[close] == '\\' ? 2 : 1;
        }
        out.append(json.substr(open + 1, close - open - 1));
        at = close + 1;
    }
    return out;
}

/// Types the game never sets on fire visibly: their fire flag is ignored.
[[nodiscard]] bool fire_immune(std::string_view type) noexcept {
    for (const std::string_view immune :
         {"minecraft:blaze", "minecraft:ghast", "minecraft:magma_cube", "minecraft:strider",
          "minecraft:wither_skeleton", "minecraft:zombified_piglin", "minecraft:ender_dragon",
          "minecraft:wither", "minecraft:zoglin", "minecraft:hoglin"}) {
        if (type == immune) {
            return true;
        }
    }
    return false;
}

}  // namespace

void EntityWorld::note_unknown(std::string text) {
    if (std::find(unknown_.begin(), unknown_.end(), text) == unknown_.end()) {
        unknown_.push_back(std::move(text));
    }
}

void EntityWorld::apply(const netclient::ClientEvents& events,
                        const registry::Registries*    registries) {
    using Kind = netclient::ClientEvents::EntityChangeKind;

    if (events.player_entity_id) {  // ── entity-models ──
        local_player_ = *events.player_entity_id;
    }

    for (const netclient::ClientEvents::EntityChange& change : events.entities) {
        switch (change.kind) {
            case Kind::Spawn: {
                TrackedEntity entity;
                entity.id   = change.id;
                entity.from = change.position;
                entity.to   = change.position;
                entity.age  = EntityWorld::kTickSeconds;
                entity.data = change.data;

                if (change.type == netclient::ClientEvents::kSpawnedAsPlayer) {
                    entity.type = "minecraft:player";
                } else if (change.type ==
                           netclient::ClientEvents::kSpawnedAsExperienceOrb) {
                    entity.type = "minecraft:experience_orb";
                    entity.orb  = true;
                } else if (registries != nullptr) {
                    if (const auto types = registries->find("minecraft:entity_type")) {
                        const std::string_view name = registries->entry_of(
                            *types, static_cast<registry::ProtocolId>(change.type));
                        entity.type = std::string(name);
                        if (const auto info = registries->entity_type(
                                static_cast<registry::ProtocolId>(change.type))) {
                            entity.width  = info->width;
                            entity.height = info->height;
                        }
                    }
                }
                if (entity.type.empty()) {
                    note_unknown("entity type id " + std::to_string(change.type));
                }
                entity.kind = render::entity_kind(entity.type);
                if (entity.kind == render::EntityKind::Unknown && !entity.type.empty()) {
                    note_unknown(entity.type + " (no look in this build)");
                } else if (entity.kind == render::EntityKind::Refused) {
                    note_unknown(entity.type + ": " +
                                 std::string(render::refusal_reason(entity.type)));
                }

                entity.body_yaw          = change.yaw;
                entity.previous_body_yaw = change.yaw;
                entity.head_yaw          = change.head_yaw;
                entity.previous_head_yaw = change.head_yaw;
                entity.pitch             = change.pitch;
                entity.history.push(static_cast<f32>(change.position.y), change.yaw);
                entities_[change.id] = std::move(entity);
                break;
            }

            case Kind::Move:
            case Kind::Teleport: {
                auto found = entities_.find(change.id);
                if (found == entities_.end()) {
                    break;
                }
                TrackedEntity& entity = found->second;
                entity.from           = entity.to;
                entity.to = change.kind == Kind::Teleport
                                ? change.position
                                : Vec3d{entity.to.x + change.position.x,
                                        entity.to.y + change.position.y,
                                        entity.to.z + change.position.z};
                entity.age   = 0.0;
                entity.moved = true;

                const f64 dx = entity.to.x - entity.from.x;
                const f64 dz = entity.to.z - entity.from.z;
                const f64 horizontal = std::sqrt(dx * dx + dz * dz);
                render::advance_walk(entity.walk, static_cast<f32>(horizontal));

                entity.previous_body_yaw = entity.body_yaw;
                entity.previous_head_yaw = entity.head_yaw;
                if (change.data != 0) {
                    entity.body_yaw = change.yaw;
                    entity.head_yaw = change.head_yaw;
                    entity.pitch    = change.pitch;
                } else if (horizontal > kFacingThreshold) {
                    entity.body_yaw = yaw_towards(dx, dz);
                    entity.head_yaw = entity.body_yaw;
                }
                break;
            }

            case Kind::HeadRotation: {
                auto found = entities_.find(change.id);
                if (found == entities_.end()) {
                    break;
                }
                found->second.previous_head_yaw = found->second.head_yaw;
                found->second.head_yaw          = change.head_yaw;
                break;
            }

            case Kind::Metadata: {
                auto found = entities_.find(change.id);
                if (found == entities_.end()) {
                    break;
                }
                TrackedEntity& entity = found->second;
                if (change.stack) {
                    entity.item_count = change.stack->count;
                    entity.item.clear();
                    if (registries != nullptr && !change.stack->empty()) {
                        if (const auto items = registries->find("minecraft:item")) {
                            entity.item = std::string(registries->entry_of(
                                *items,
                                static_cast<registry::ProtocolId>(change.stack->item_id)));
                        }
                    }
                }
                // ── entity-models ── every field, kept by index; its meaning
                // is the look table's.
                for (const net::MetadataValue& value : change.metadata) {
                    if (value.index == render::meta::kCustomName) {
                        entity.traits.name =
                            value.present ? plain_name(value.text) : std::string{};
                    }
                    if (value.index >= render::kMetaSlots) {
                        continue;
                    }
                    render::MetaSlot& slot = entity.traits.slots[value.index];
                    slot.present           = true;
                    slot.integer           = value.integer;
                    slot.real              = value.real;
                    slot.villager          = value.villager;
                    slot.rotation          = {value.vector[0], value.vector[1], value.vector[2]};
                    slot.position          = value.position;
                    if (value.type == net::MetadataType::OptionalComponent ||
                        value.type == net::MetadataType::OptionalBlockPos) {
                        // Present-or-not is the value for these: a name tag,
                        // a crystal's beam target.
                        slot.integer = value.present ? 1 : 0;
                    }
                }
                entity.look_dirty = true;
                break;
            }

            case Kind::Equipment: {
                auto found = entities_.find(change.id);
                if (found == entities_.end()) {
                    break;
                }
                TrackedEntity& entity = found->second;
                for (const net::EquipmentEntry& entry : change.equipment) {
                    std::string name;
                    if (registries != nullptr && !entry.stack.empty()) {
                        if (const auto items = registries->find("minecraft:item")) {
                            name = std::string(registries->entry_of(
                                *items, static_cast<registry::ProtocolId>(entry.stack.item_id)));
                        }
                    }
                    entity.traits.equipment[static_cast<usize>(entry.slot)] = std::move(name);
                }
                entity.look_dirty = true;
                break;
            }

            case Kind::Event: {
                auto found = entities_.find(change.id);
                if (found != entities_.end() && change.status == net::entity_status::kDeath) {
                    found->second.dying = 0.0;
                }
                break;
            }

            case Kind::Hurt: {
                auto found = entities_.find(change.id);
                if (found != entities_.end()) {
                    found->second.hurt = kHurtSeconds;
                }
                break;
            }

            case Kind::Passengers: {
                // The whole list, every time: whoever rode this vehicle and is
                // not named any more has dismounted.
                for (auto& [id, entity] : entities_) {
                    if (entity.vehicle == change.id) {
                        entity.vehicle = -1;
                    }
                }
                if (local_vehicle_ == change.id) {
                    local_vehicle_ = -1;
                }
                for (const i32 rider : change.riders) {
                    if (auto found = entities_.find(rider); found != entities_.end()) {
                        found->second.vehicle = change.id;
                    }
                    if (local_player_ && rider == *local_player_) {
                        local_vehicle_ = change.id;
                    }
                }
                break;
            }

            case Kind::Remove: {
                auto found = entities_.find(change.id);
                if (found == entities_.end()) {
                    break;
                }
                // A mob removed while it is still toppling is kept until the
                // topple ends: the game's server removes it at twenty ticks, a
                // server that removes it sooner would otherwise cut the
                // animation off.
                if (found->second.dying >= 0.0 && found->second.dying < kDeathSeconds) {
                    found->second.removed = true;
                } else {
                    entities_.erase(found);
                }
                if (local_vehicle_ == change.id) {
                    local_vehicle_ = -1;
                }
                break;
            }
        }
    }
}

void EntityWorld::advance(f64 seconds) {
    tick_clock_ += seconds;
    u32 ticks = 0;
    while (tick_clock_ >= kTickSeconds) {
        tick_clock_ -= kTickSeconds;
        ++ticks;
    }

    for (auto it = entities_.begin(); it != entities_.end();) {
        TrackedEntity& entity = it->second;
        entity.age += seconds;
        entity.lived += seconds;
        entity.hurt = std::max(0.0, entity.hurt - seconds);
        if (entity.dying >= 0.0) {
            entity.dying += seconds;
        }
        // A mob that stops moving stops sending packets, and its walk state
        // must decay or its legs keep swinging while it stands.
        if (entity.age > kTickSeconds * 2.0) {
            render::advance_walk(entity.walk, 0.0F);
        }
        for (u32 tick = 0; tick < ticks; ++tick) {
            // A dragon without AI does not beat its wings: measured, its wing
            // beat reads 0.5 at every tick (scripts/entity_render_oracle.java,
            // `flap`). Mob flags 0x01 is NoAI.
            if (entity.type == "minecraft:ender_dragon" &&
                !entity.traits.flag(render::meta::kMobFlags, 0x01)) {
                const f64 dx = entity.to.x - entity.from.x;
                const f64 dz = entity.to.z - entity.from.z;
                const f64 dy = entity.to.y - entity.from.y;
                entity.history.push(static_cast<f32>(entity.to.y), entity.body_yaw);
                // The wing beat slows with speed and quickens climbing; a
                // perched dragon beats slowly. See the provenance document for
                // where these rates were read.
                const f64 speed = std::sqrt(dx * dx + dz * dz);
                f64       step  = 0.2 / (speed * 10.0 + 1.0) * std::pow(2.0, dy);
                const i64 phase = entity.traits.integer(render::meta::kDragonPhase);
                if (phase >= 5 && phase <= 7) {
                    step = 0.1;
                }
                entity.previous_flap = entity.flap;
                entity.flap += static_cast<f32>(step);
            }
        }
        if (entity.removed && entity.dying >= kDeathSeconds) {
            it = entities_.erase(it);
        } else {
            ++it;
        }
    }
}

void EntityWorld::refresh_looks(const LookResources& resources) {
    if (resources.models == nullptr) {
        return;
    }
    for (auto& [id, entity] : entities_) {
        if (!entity.look_dirty) {
            continue;
        }
        entity.look_dirty = false;
        entity.layers.clear();
        if (entity.kind != render::EntityKind::Model) {
            continue;
        }
        entity.look = render::resolve_look(entity.type, entity.traits, resources.context);
        entity.walk.baby_head_scale  = entity.look.baby_head_scale;
        entity.walk.baby_head_offset = entity.look.baby_head_offset;
        for (const render::LookLayer& layer : entity.look.layers) {
            CachedLayer cached;
            cached.layer = layer;
            cached.model = resources.models->find(layer.model);
            if (cached.model == nullptr) {
                note_unknown("model " + layer.model + " (not in the entity model file)");
                continue;
            }
            if (layer.pass == render::LayerPass::Energy) {
                if (resources.energy == nullptr) {
                    continue;
                }
                const auto found = resources.energy->find(layer.texture);
                if (found == resources.energy->end()) {
                    note_unknown("texture " + layer.texture);
                    continue;
                }
                cached.energy = found->second;
            } else {
                const auto rect =
                    resources.atlas != nullptr ? resources.atlas->find(layer.texture) : std::nullopt;
                if (!rect) {
                    note_unknown("texture " + layer.texture);
                    continue;
                }
                cached.uv = *rect;
            }
            entity.layers.push_back(std::move(cached));
        }
    }
}

EntityFrame EntityWorld::frame_of(const TrackedEntity& entity, bool interpolate) const {
    EntityFrame frame;
    if (!interpolate || !entity.moved) {
        frame.position = Vec3f{static_cast<f32>(entity.to.x), static_cast<f32>(entity.to.y),
                               static_cast<f32>(entity.to.z)};
        frame.body_yaw = entity.body_yaw;
        return frame;
    }
    const f64 alpha = std::clamp(entity.age / kTickSeconds, 0.0, 1.0);
    frame.position =
        Vec3f{static_cast<f32>(entity.from.x + (entity.to.x - entity.from.x) * alpha),
              static_cast<f32>(entity.from.y + (entity.to.y - entity.from.y) * alpha),
              static_cast<f32>(entity.from.z + (entity.to.z - entity.from.z) * alpha)};
    frame.body_yaw =
        lerp_angle(entity.previous_body_yaw, entity.body_yaw, static_cast<f32>(alpha));
    return frame;
}

std::optional<Vec3f> EntityWorld::riding_eye(bool interpolate) const {
    if (local_vehicle_ < 0) {
        return std::nullopt;
    }
    const auto found = entities_.find(local_vehicle_);
    if (found == entities_.end()) {
        return std::nullopt;
    }
    const EntityFrame vehicle = frame_of(found->second, interpolate);
    // Where a rider's feet sit relative to what it rides, then its eye above
    // them. A minecart seats its rider 0.35 blocks below the cart's position,
    // a boat 0.45; docs/provenance/rendu-entites.md says how the minecart's
    // was checked against the game.
    f64 feet = -0.35;
    if (found->second.type.ends_with("boat")) {
        feet = -0.45;
    } else if (!found->second.type.ends_with("minecart")) {
        feet = static_cast<f64>(found->second.height) * 0.75 - 0.35;
    }
    return Vec3f{vehicle.position.x,
                 static_cast<f32>(static_cast<f64>(vehicle.position.y) + feet + kEyeHeight),
                 vehicle.position.z};
}

namespace {

/// Hand vertices to a pass, and keep a copy when the run is dumping them.
bool submit(const EntityDrawContext& context, client::EntityRenderer* renderer,
            client::EntityTexture texture, const std::vector<render::EntityVertex>& vertices,
            const TrackedEntity& entity, const EntityFrame& frame, std::string_view pass) {
    // Recorded before the renderer is asked: the offline check has none.
    if (context.dump != nullptr && !vertices.empty()) {
        EntityDumpRecord& record = context.dump->emplace_back();
        record.id                = entity.id;
        record.type              = entity.type;
        record.pass              = pass;
        record.origin            = frame.position;
        record.vertices.assign(vertices.begin(), vertices.end());
    }
    if (renderer == nullptr) {
        return context.dump != nullptr && !vertices.empty();
    }
    return renderer->submit(texture, vertices);
}

[[nodiscard]] std::string_view pass_name(render::LayerPass pass) noexcept {
    switch (pass) {
        case render::LayerPass::Cutout:
            return "cutout";
        case render::LayerPass::Translucent:
            return "translucent";
        case render::LayerPass::Eyes:
            return "eyes";
        case render::LayerPass::Energy:
            return "energy";
    }
    return "cutout";
}

/// The world-space frame a camera-facing sprite is drawn in.
struct Billboard {
    Vec3f right;
    Vec3f up;
};

[[nodiscard]] Billboard facing_camera(const EntityDrawContext& context) noexcept {
    // The game turns a sprite to the camera's yaw and pitch, not to the line
    // from the sprite to the eye: every sprite on the screen is parallel.
    // Minecraft's camera: forward (−sin y cos p, −sin p, cos y cos p), right
    // (−cos y, 0, −sin y), and up = right × forward.
    const f32 yaw   = context.camera_yaw / kDegrees;
    const f32 pitch = context.camera_pitch / kDegrees;
    const Vec3f right{-std::cos(yaw), 0.0F, -std::sin(yaw)};
    const Vec3f up{-std::sin(yaw) * std::sin(pitch), std::cos(pitch),
                   std::cos(yaw) * std::sin(pitch)};
    return Billboard{right, up};
}

/// The terrain's flat face shade, which is what a block drawn as an entity
/// takes (ClientLevel.getShade, measured in docs/provenance/rendu-parite.md).
[[nodiscard]] f32 block_shade(Direction facing) noexcept {
    switch (facing) {
        case Direction::Up:
            return 1.0F;
        case Direction::Down:
            return 0.5F;
        case Direction::North:
        case Direction::South:
            return 0.8F;
        case Direction::West:
        case Direction::East:
            return 0.6F;
    }
    return 0.6F;
}

[[nodiscard]] u32 shaded(u32 rgb, f32 shade) noexcept {
    const auto channel = [&](u32 shift) {
        return static_cast<u32>(static_cast<f32>((rgb >> shift) & 0xFFU) * shade) << shift;
    };
    return 0xFF000000U | channel(16) | channel(8) | channel(0);
}

/// A block model, as the game draws a block that is an entity: each baked quad
/// through `place`, which takes a point of the block (0..1) to the world.
template <typename Place>
bool emit_block(const EntityDrawContext& context, registry::BlockStateId state, Place place,
                u32 light, u32 overlay, std::vector<render::EntityVertex>& out) {
    if (context.blocks == nullptr || context.block_sprites == nullptr) {
        return false;
    }
    const render::BlockRender& block = context.blocks->resolve(state);
    if (!block.drawable) {
        return false;
    }
    const usize before = out.size();
    for (const render::BakedQuad& quad : block.model.quads) {
        const render::SpriteUv sprite = context.block_sprites->uv(quad.sprite);
        std::array<Vec3f, 4>   corners{};
        std::array<f32, 4>     u{};
        std::array<f32, 4>     v{};
        for (usize corner = 0; corner < 4; ++corner) {
            corners[corner] = place(quad.vertices[corner].position);
            u[corner] = sprite.u0 + (sprite.u1 - sprite.u0) * quad.vertices[corner].u / 16.0F;
            v[corner] = sprite.v0 + (sprite.v1 - sprite.v0) * quad.vertices[corner].v / 16.0F;
        }
        const u32 tint = quad.tint_index >= 0 ? context.foliage_tint : 0xFFFFFFU;
        // A baked quad is wound for the terrain, which is not reflected: the
        // corners go out in reverse so that the reflection-free path here
        // shows the same side the terrain shows.
        const std::array<Vec3f, 4> back{corners[3], corners[2], corners[1], corners[0]};
        const std::array<f32, 4>   back_u{u[3], u[2], u[1], u[0]};
        const std::array<f32, 4>   back_v{v[3], v[2], v[1], v[0]};
        render::emit_quad(back, back_u, back_v,
                          quad.shade ? shaded(tint, block_shade(quad.facing)) : (0xFF000000U | tint),
                          out, light, overlay);
    }
    return out.size() > before;
}

/// A dropped stack, drawn as the picture the inventory would show of it.
///
/// ⚠ Vanilla draws a dropped item as a *solid* thing: a block item is the block
/// model, a tool is its sprite extruded a sixteenth thick. This draws the GUI
/// icon instead — the same quads, the same atlas rects, the same flat shading —
/// on one plane, turned about Y and bobbing. Named rather than presented as
/// vanilla's.
bool emit_item_icon(const EntityDrawContext& context, std::string_view item, Vec3f centre,
                    Vec3f right, Vec3f up, f32 size, u32 light,
                    std::vector<render::EntityVertex>& out) {
    if (context.items == nullptr || item.empty()) {
        return false;
    }
    const render::ItemMesh* mesh = context.items->mesh(item);
    if (mesh == nullptr || !mesh->drawable) {
        return false;
    }
    const auto place = [&](render::ItemPoint point) {
        const f32 local_x = (point.x / render::kItemCellSize - 0.5F) * size;
        const f32 local_y = (0.5F - point.y / render::kItemCellSize) * size;
        return centre + right * local_x + up * local_y;
    };
    const usize before = out.size();
    const auto  both   = [&](const std::array<Vec3f, 4>& corners, const std::array<f32, 4>& u,
                          const std::array<f32, 4>& v, u32 tint) {
        render::emit_quad(corners, u, v, tint, out, light);
        render::emit_quad({corners[3], corners[2], corners[1], corners[0]}, {u[3], u[2], u[1], u[0]},
                          {v[3], v[2], v[1], v[0]}, tint, out, light);
    };
    if (mesh->flat) {
        for (const render::SpriteUv& layer : mesh->layers) {
            both({place(render::ItemPoint{0.0F, render::kItemCellSize}),
                  place(render::ItemPoint{render::kItemCellSize, render::kItemCellSize}),
                  place(render::ItemPoint{render::kItemCellSize, 0.0F}),
                  place(render::ItemPoint{0.0F, 0.0F})},
                 {layer.u0, layer.u1, layer.u1, layer.u0}, {layer.v1, layer.v1, layer.v0, layer.v0},
                 0xFFFFFFFFU);
        }
    } else {
        for (const render::ItemQuad& quad : mesh->quads) {
            both({place(quad.position[0]), place(quad.position[1]), place(quad.position[2]),
                  place(quad.position[3])},
                 {quad.uv[0].x, quad.uv[1].x, quad.uv[2].x, quad.uv[3].x},
                 {quad.uv[0].y, quad.uv[1].y, quad.uv[2].y, quad.uv[3].y},
                 shaded(0xFFFFFFU, quad.shade));
        }
    }
    return out.size() > before;
}

/// A crystal's bob, in blocks, from its own clock in ticks: the glass's centre
/// sits 2 + j above the crystal's feet, j in [−1.4, −0.6]. The law is the one
/// the game's vertices fit: glass centres at 0.61 and ~1.35 blocks in the two
/// scenes, and a beam whose crystal end is that same centre.
[[nodiscard]] f32 crystal_bob(f32 ticks) noexcept {
    const f32 s = std::sin(ticks * 0.2F) / 2.0F + 0.5F;
    return (s * s + s) * 0.4F - 1.4F;
}

/// The crystal's three nested parts — the glass, a glass inside it at 0.875,
/// the cube at 0.875 of that — each turned once more by 60° about the (1, 0,
/// 1) diagonal and spun 3° a tick about y, as a linear map on this project's
/// model axes (EntityPlacement::model_basis), and the lift that puts the
/// glass's centre 2 + j above the feet at the crystal's scale of 2 and lift of
/// −0.5.
struct CrystalTurns {
    std::array<std::array<Vec3f, 3>, 3> basis{};
    Vec3f                               offset{};
};

[[nodiscard]] CrystalTurns crystal_turns(f32 ticks) noexcept {
    using Matrix = std::array<std::array<f32, 3>, 3>;
    const auto product = [](const Matrix& a, const Matrix& b) {
        Matrix out{};
        for (usize r = 0; r < 3; ++r) {
            for (usize c = 0; c < 3; ++c) {
                for (usize k = 0; k < 3; ++k) {
                    out[r][c] += a[r][k] * b[k][c];
                }
            }
        }
        return out;
    };
    const f32    spin = ticks * 3.0F / kDegrees;
    const Matrix turn{{{std::cos(spin), 0.0F, std::sin(spin)},
                       {0.0F, 1.0F, 0.0F},
                       {-std::sin(spin), 0.0F, std::cos(spin)}}};
    // 60° about (√½, 0, √½), by Rodrigues: cos 60° = 0.5, the axis's squares
    // ½, so the diagonal terms are 0.75 and 0.5, the corners 0.25, and
    // √½ × sin 60° = 0.6124.
    constexpr f32 kSide = 0.61237244F;
    const Matrix  tilt{{{0.75F, -kSide, 0.25F}, {kSide, 0.5F, -kSide}, {0.25F, kSide, 0.75F}}};
    Matrix        step = product(tilt, turn);
    for (auto& row : step) {
        for (f32& value : row) {
            value *= 0.875F;
        }
    }
    const Matrix outer = product(turn, tilt);
    const Matrix inner = product(outer, step);
    const Matrix cube  = product(inner, step);

    CrystalTurns out;
    // The game's crystal is drawn unflipped (y up): our model's y is flipped
    // back going in (F), and the yaw-0 placement's own flip of z undone
    // coming out (D).
    const std::array<f32, 3> flip_in{1.0F, -1.0F, 1.0F};
    const std::array<f32, 3> flip_out{1.0F, 1.0F, -1.0F};
    const std::array<const Matrix*, 3> parts{&outer, &inner, &cube};
    for (usize part = 0; part < 3; ++part) {
        for (usize c = 0; c < 3; ++c) {
            const Matrix& m = *parts[part];
            out.basis[part][c] = Vec3f{flip_out[0] * m[0][c] * flip_in[c],
                                       flip_out[1] * m[1][c] * flip_in[c],
                                       flip_out[2] * m[2][c] * flip_in[c]};
        }
    }
    out.offset = Vec3f{0.0F, 8.0F * (2.5F + crystal_bob(ticks)), 0.0F};
    return out;
}

/// What a mob or a player holds, in the hand the game puts it in.
///
/// The hand is the bottom front of the arm: the arm's own frame, a quarter
/// turn down and a half turn about, then (±1, 2, −10) sixteenths — the point
/// (∓1, 10, −2) of the arm in the game's model axes. The item is then placed by
/// its own model's `display.thirdperson_righthand`, read from the pack; the
/// left hand mirrors its y and z rotations and its x translation. The main
/// hand is the left arm on a left-handed mob (flag 0x02 of index 15).
/// ⚠ Drawn as the item's flat icon, not the game's extruded slab or block.
void emit_held_items(const EntityDrawContext& context, const TrackedEntity& entity,
                     const render::EntityPlacement& placement, const render::WalkState& state,
                     u32 light, const EntityFrame& frame, EntityScratch& scratch, bool& drawn) {
    if (context.items == nullptr || (context.cutout == nullptr && context.dump == nullptr) ||
        entity.layers.empty() || entity.layers.front().model == nullptr ||
        entity.type == "minecraft:enderman" || entity.type == "minecraft:iron_golem") {
        return;
    }
    const render::EntityModel& model = *entity.layers.front().model;
    const i32                  right = model.bone("right_arm");
    const i32                  left  = model.bone("left_arm");
    if (right < 0 || left < 0) {
        return;
    }
    const bool left_handed =
        entity.traits.flag(render::meta::kMobFlags, render::meta::kMobLeftHanded);
    std::vector<render::EntityVertex>& vertices = scratch.vertices;
    vertices.clear();
    bool posed = false;
    for (usize slot = 0; slot < 2; ++slot) {
        const std::string& item = entity.traits.equipment[slot];
        if (item.empty()) {
            continue;
        }
        const render::ItemMesh* mesh = context.items->mesh(item);
        if (mesh == nullptr || !mesh->drawable) {
            continue;
        }
        if (!posed) {
            render::pose_model(model, entity.look.animation, state, scratch.poses);
            render::apply_look_visibility(model, entity.look, scratch.poses);
            posed = true;
        }
        const bool on_left = (slot == 0) == left_handed;
        const i32  bone    = on_left ? left : right;
        if (scratch.poses[static_cast<usize>(bone)].hidden) {
            continue;
        }
        render::BoneFrame arm;
        if (!render::bone_frame(model, scratch.poses, placement, bone, arm)) {
            continue;
        }
        render::DisplayTransform display = mesh->hand.value_or(render::DisplayTransform{});
        const f32                side    = on_left ? -1.0F : 1.0F;
        if (on_left) {
            display.rotation.y    = -display.rotation.y;
            display.rotation.z    = -display.rotation.z;
            display.translation.x = -display.translation.x;
        }
        // The display rotation, x then y then z as the file composes it.
        const auto turn = [&](Vec3f v) {
            const f32 cx = std::cos(display.rotation.x / kDegrees);
            const f32 sx = std::sin(display.rotation.x / kDegrees);
            const f32 cy = std::cos(display.rotation.y / kDegrees);
            const f32 sy = std::sin(display.rotation.y / kDegrees);
            const f32 cz = std::cos(display.rotation.z / kDegrees);
            const f32 sz = std::sin(display.rotation.z / kDegrees);
            v = Vec3f{v.x * cz - v.y * sz, v.x * sz + v.y * cz, v.z};
            v = Vec3f{v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy};
            return Vec3f{v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx};
        };
        // A point of the item (blocks, about its centre) to the world: the
        // display transform, the hand's turns — (x, y, z) → (−x, −z, −y) —
        // from the hand, then into this model's axes (y up) through the arm.
        const auto place = [&](Vec3f item_point) {
            const Vec3f p = display.translation + turn(item_point);
            const Vec3f game{-side / 16.0F - p.x, 10.0F / 16.0F - p.z, -2.0F / 16.0F - p.y};
            return arm.at(Vec3f{game.x * 16.0F, -game.y * 16.0F, game.z * 16.0F});
        };
        const Vec3f centre = place(Vec3f{});
        const Vec3f across = place(Vec3f{0.5F * display.scale.x, 0.0F, 0.0F}) - centre;
        const Vec3f upward = place(Vec3f{0.0F, 0.5F * display.scale.y, 0.0F}) - centre;
        const f32   size   = across.length() * 2.0F;
        if (size <= 1.0e-4F || upward.length() <= 1.0e-6F) {
            continue;
        }
        (void)emit_item_icon(context, item, centre, across * (1.0F / across.length()),
                             upward.normalized(), size, light, vertices);
    }
    if (!vertices.empty()) {
        drawn = submit(context, context.cutout, context.block_atlas, vertices, entity, frame,
                       "held") ||
                drawn;
    }
}

/// A sprite from the entity atlas, facing the camera, its bottom edge at
/// `bottom` and `width` across.
void emit_sprite(const EntityDrawContext& context, const render::UvRect& rect, Vec3f bottom,
                 f32 width, f32 height, u32 tint, u32 light, std::vector<render::EntityVertex>& out) {
    const Billboard board = facing_camera(context);
    const Vec3f     half  = board.right * (width * 0.5F);
    const Vec3f     top   = board.up * height;
    render::emit_quad({bottom - half, bottom + half, bottom + half + top, bottom - half + top},
                      {rect.u0, rect.u1, rect.u1, rect.u0}, {rect.v1, rect.v1, rect.v0, rect.v0}, tint,
                      out, light);
}

/// The fire on a burning entity: sheets of the two fire sprites facing the
/// camera, stacked up the entity's height, each narrower than the last.
void emit_fire(const EntityDrawContext& context, const TrackedEntity& entity, Vec3f feet,
               std::vector<render::EntityVertex>& out) {
    if (context.block_sprites == nullptr) {
        return;
    }
    const render::SpriteUv fire0 = context.block_sprites->uv("minecraft:block/fire_0");
    const render::SpriteUv fire1 = context.block_sprites->uv("minecraft:block/fire_1");
    // Turned to the camera's yaw only: the sheets stay upright and their
    // depth horizontal, as the game's own fire quads do.
    const Billboard board = facing_camera(context);
    Vec3f           right{board.right.x, 0.0F, board.right.z};
    right = right.length() > 1.0e-6F ? right.normalized() : Vec3f{1.0F, 0.0F, 0.0F};
    const Vec3f up{0.0F, 1.0F, 0.0F};
    const Vec3f depth = right.cross(up);
    const f32   scale  = entity.width * 1.4F;
    f32         height = entity.height / scale;
    f32         half   = 0.5F;
    f32         rise   = 0.0F;
    // The first sheet 0.3 of the scale towards the camera, less 0.02 for each
    // whole scaled height, each next one 0.03 further back: the game's sheets
    // for a burning zombie sit 0.218, 0.193, … 0.092 blocks in front of it.
    f32 push = static_cast<f32>(static_cast<i32>(height)) * 0.02F;
    for (u32 sheet = 0; height > 0.0F && sheet < 16; ++sheet) {
        const render::SpriteUv& sprite = sheet % 2 == 0 ? fire0 : fire1;
        f32 u0 = sprite.u0;
        f32 u1 = sprite.u1;
        if ((sheet / 2) % 2 == 0) {
            std::swap(u0, u1);
        }
        const Vec3f base  = feet + up * (rise * scale) + depth * ((0.3F - push) * scale);
        const Vec3f side  = right * (half * scale);
        const Vec3f tall  = up * (1.4F * scale);
        render::emit_quad({base - side, base + side, base + side + tall, base - side + tall},
                          {u0, u1, u1, u0}, {sprite.v1, sprite.v1, sprite.v0, sprite.v0},
                          0xFFFFFFFFU, out, 0xFFFFFFU);
        height -= 0.45F;
        rise += 0.45F;
        half *= 0.9F;
        push += 0.03F;
    }
}

/// A name above an entity: the default font's glyphs on a quarter-black plate,
/// facing the camera, 0.025 blocks a GUI pixel.
void emit_name_tag(const EntityDrawContext& context, std::string_view text, Vec3f anchor,
                   u32 light, std::vector<render::EntityVertex>& glyphs,
                   std::vector<render::EntityVertex>& plate) {
    if (context.font == nullptr || context.entity_atlas == nullptr || text.empty()) {
        return;
    }
    const Billboard board = facing_camera(context);
    constexpr f32   kPixel = 0.025F;
    const f32       width  = context.font->width(text);
    // GUI pixels to the world: x across, centred; y downwards from the anchor,
    // as the game's `scale(-0.025, -0.025, 0.025)` hangs its text.
    const auto at = [&](f32 gui_x, f32 gui_y) {
        return anchor + board.right * ((gui_x - width * 0.5F) * kPixel) - board.up * (gui_y * kPixel);
    };
    // The plate: a pixel of margin, 25 % black, as the game's default text
    // background opacity has it. No culling in the entity passes, so one face.
    if (const auto white = context.entity_atlas->find("ov:white")) {
        render::emit_quad({at(-1.0F, 8.0F), at(width, 8.0F), at(width, -1.0F), at(-1.0F, -1.0F)},
                          {white->u0, white->u1, white->u1, white->u0},
                          {white->v1, white->v1, white->v0, white->v0}, 0x40000000U, plate, 0xFFFFFFU);
    }
    f32   pen    = 0.0F;
    usize offset = 0;
    std::array<char, 16> page_name{};
    while (offset < text.size()) {
        const char32_t       codepoint = render::next_codepoint(text, offset);
        const render::Glyph* glyph     = context.font->glyph(codepoint);
        if (glyph == nullptr) {
            continue;
        }
        const int length = std::snprintf(page_name.data(), page_name.size(), "ov:font/%u",
                                          static_cast<unsigned>(glyph->page));
        const auto rect  = context.entity_atlas->find(
            std::string(page_name.data(), static_cast<usize>(std::max(length, 0))));
        if (rect && glyph->width > 0.0F) {
            const f32 u0  = rect->u0 + (rect->u1 - rect->u0) * glyph->u0;
            const f32 u1  = rect->u0 + (rect->u1 - rect->u0) * glyph->u1;
            const f32 v0  = rect->v0 + (rect->v1 - rect->v0) * glyph->v0;
            const f32 v1  = rect->v0 + (rect->v1 - rect->v0) * glyph->v1;
            // The glyph's top sits `7 − ascent` pixels below the line's top.
            const f32 top = 7.0F - glyph->ascent;
            render::emit_quad({at(pen, top + glyph->height), at(pen + glyph->width, top + glyph->height),
                               at(pen + glyph->width, top), at(pen, top)},
                              {u0, u1, u1, u0}, {v1, v1, v0, v0}, 0xFFFFFFFFU, glyphs, light);
        }
        pen += glyph->advance;
    }
}

/// The world transform of an entity model's space, the placement's, applied to
/// a point given in the entity's own space after the model flip (x mirrored,
/// y up, blocks). A block in a minecart is placed in that space.
[[nodiscard]] Vec3f entity_space_to_world(const render::EntityPlacement& placement,
                                          Vec3f point) noexcept {
    const f32 roll = placement.roll / kDegrees;
    const Vec3f rolled{point.x * std::cos(roll) - point.y * std::sin(roll),
                       point.x * std::sin(roll) + point.y * std::cos(roll), point.z};
    const f32 yaw = (180.0F - placement.body_yaw) / kDegrees;
    const Vec3f turned{rolled.x * std::cos(yaw) + rolled.z * std::sin(yaw), rolled.y,
                       -rolled.x * std::sin(yaw) + rolled.z * std::cos(yaw)};
    return placement.position + Vec3f{0.0F, placement.lift, 0.0F} + turned;
}

/// A minecart's cargo: the block it shows, three quarters of a block, turned a
/// quarter and raised by the display offset.
void emit_minecart_block(const EntityDrawContext& context, const TrackedEntity& entity,
                         const render::EntityPlacement& placement, u32 light,
                         std::vector<render::EntityVertex>& out) {
    if (context.block_registry == nullptr) {
        return;
    }
    const render::EntityTraits& traits = entity.traits;
    std::optional<registry::BlockStateId> state;
    i64 offset = entity.type == "minecraft:hopper_minecart" ? 1 : 6;
    if (traits.integer(render::meta::kMinecartCustomDisplay) != 0) {
        state = registry::BlockStateId{
            static_cast<u16>(traits.integer(render::meta::kMinecartDisplayBlock))};
        offset = traits.integer(render::meta::kMinecartDisplayOffset, offset);
    } else {
        std::string_view block;
        if (entity.type == "minecraft:furnace_minecart") {
            block = "minecraft:furnace";
        } else if (entity.type == "minecraft:tnt_minecart") {
            block = "minecraft:tnt";
        } else if (entity.type == "minecraft:hopper_minecart") {
            block = "minecraft:hopper";
        } else if (entity.type == "minecraft:spawner_minecart") {
            block = "minecraft:spawner";
        } else if (entity.type == "minecraft:command_block_minecart") {
            block = "minecraft:command_block";
        }
        // A chest is a block entity whose model the block model cache does not
        // have: the chest minecart's cargo is not drawn, and is named in the
        // provenance document.
        if (!block.empty()) {
            if (const auto id = context.block_registry->find_block(block)) {
                state = context.block_registry->default_state(*id);
            }
        }
    }
    if (!state) {
        return;
    }
    const f32 raise = static_cast<f32>(offset - 8) / 16.0F;
    (void)emit_block(
        context, *state,
        [&](Vec3f block_point) {
            // Turned a quarter about Y, moved so the block's centre is over
            // the cart, then scaled to three quarters.
            const Vec3f turned{block_point.z, block_point.y, -block_point.x};
            const Vec3f moved = (turned + Vec3f{-0.5F, raise, 0.5F}) * 0.75F;
            return entity_space_to_world(placement, moved);
        },
        light, render::kNoOverlay, out);
}

}  // namespace

bool draw_entity(const EntityDrawContext& context, const TrackedEntity& entity,
                 const EntityFrame& frame, u32 light, EntityScratch& scratch) {
    std::vector<render::EntityVertex>& vertices = scratch.vertices;
    bool                               drawn    = false;
    // The dragon dies its own way (it rises for two hundred ticks, the server
    // moving it): no topple and no red, so it is never "dying" here.
    const bool dying = entity.dying >= 0.0 && !entity.type.ends_with("ender_dragon");
    const u32  overlay =
        (entity.hurt > 0.0 || dying) ? kHurtOverlay : render::kNoOverlay;

    switch (entity.kind) {
        case render::EntityKind::Model: {
            if (context.models == nullptr || entity.layers.empty()) {
                return false;
            }
            const render::EntityLook& look = entity.look;

            render::WalkState state = entity.walk;
            state.head_yaw          = std::clamp(wrap_degrees(entity.head_yaw - entity.body_yaw),
                                                 -75.0F, 75.0F);
            state.head_pitch = std::clamp(entity.pitch, -90.0F, 90.0F);
            state.age_ticks  = static_cast<f32>(entity.lived * 20.0);
            state.aggressive = entity.traits.flag(render::meta::kMobFlags, render::meta::kMobAggressive);
            for (usize limb = 0; limb < state.stand.size(); ++limb) {
                const render::MetaSlot& slot =
                    entity.traits.slots[render::meta::kArmorStandPoses + limb];
                // A limb the server never sent keeps the game's default pose.
                const std::array<f32, 3>& rest = render::kArmorStandRestPose[limb];
                state.stand[limb] = slot.present
                                        ? Vec3f{slot.rotation[0], slot.rotation[1], slot.rotation[2]}
                                        : Vec3f{rest[0], rest[1], rest[2]};
            }

            render::EntityPlacement placement;
            placement.position  = frame.position;
            placement.body_yaw  = frame.body_yaw;
            placement.origin    = look.origin;
            placement.lift      = look.lift;
            placement.model_yaw = look.model_yaw;
            placement.scale     = look.scale;
            placement.light     = light;
            placement.overlay   = look.living ? overlay : render::kNoOverlay;
            if (look.living && dying) {
                // The topple: a quarter turn onto the side over the twenty
                // ticks the corpse lasts.
                placement.roll = 90.0F * static_cast<f32>(std::min(1.0, entity.dying / kDeathSeconds));
            }
            if (look.upside_down) {
                placement.roll += 180.0F;
                placement.lift += entity.height + 0.1F;
            }
            if (entity.type.ends_with("minecart") && entity.pitch != 0.0F) {
                placement.roll -= entity.pitch;
            }

            const bool dragon  = entity.type == "minecraft:ender_dragon";
            const bool crystal = entity.type == "minecraft:end_crystal";
            render::DragonPose segments;
            if (dragon) {
                // The dragon's own frame: it faces the other way from its yaw,
                // and its body bobs with the beat (entity_pose.hpp).
                placement.body_yaw = frame.body_yaw + 180.0F;
            }

            for (const CachedLayer& layer : entity.layers) {
                client::EntityRenderer* renderer = context.cutout;
                client::EntityTexture   texture  = context.atlas_cutout;
                switch (layer.layer.pass) {
                    case render::LayerPass::Cutout:
                        break;
                    case render::LayerPass::Translucent:
                        renderer = context.translucent;
                        texture  = context.atlas_translucent;
                        break;
                    case render::LayerPass::Eyes:
                        renderer = context.eyes;
                        texture  = context.atlas_eyes;
                        break;
                    case render::LayerPass::Energy:
                        renderer = context.energy;
                        texture  = layer.energy;
                        break;
                }
                if ((renderer == nullptr && context.dump == nullptr) || layer.model == nullptr) {
                    continue;
                }
                const bool additive = layer.layer.pass == render::LayerPass::Eyes ||
                                      layer.layer.pass == render::LayerPass::Energy;
                render::EntityPlacement layer_placement = placement;
                layer_placement.tint  = layer.layer.tint;
                layer_placement.uv    = layer.uv;
                layer_placement.shade = !additive;
                if (additive) {
                    layer_placement.light   = 0xFFFFFFU;
                    layer_placement.overlay = render::kNoOverlay;
                }
                if (layer.layer.pass == render::LayerPass::Energy) {
                    // The swirl scrolls a hundredth of the sheet a tick, both
                    // ways (`creeper_armor.render_controllers.json`).
                    const f32 scroll        = static_cast<f32>(entity.lived * 20.0) * 0.01F;
                    layer_placement.uv          = render::UvRect{};
                    layer_placement.uv_scroll_u = scroll;
                    layer_placement.uv_scroll_v = scroll;
                    layer_placement.tint        = 0xFF808080U;
                }

                vertices.clear();
                if (dragon) {
                    const f32 flap =
                        entity.previous_flap +
                        (entity.flap - entity.previous_flap) *
                            static_cast<f32>(std::clamp(entity.age / EntityWorld::kTickSeconds, 0.0, 1.0));
                    // Phase 10 (hover) is the default when the server sent none.
                    // The neck bends by its segment index when perched (5–7)
                    // and when hovering: the game's own neck for a dragon that
                    // sent no phase steps down 7.5° a segment, which is that rule.
                    const i64  phase = entity.traits.integer(render::meta::kDragonPhase, 10);
                    const bool still = (phase >= 5 && phase <= 7) || phase == 10;
                    render::pose_dragon(*layer.model, flap, entity.history, still,
                                        scratch.poses, segments);
                    // Two blocks up less the bob and two blocks along its own
                    // −z: where the game's vertices put every part of a dragon
                    // at flap 0.5 — each 1.648 higher and 1.98 further along
                    // −z than the plain living origin, for a bob of 0.354.
                    layer_placement.origin =
                        look.origin + Vec3f{0.0F, (2.0F - segments.bob) * 16.0F, -32.0F};
                    // And tilted twice the bob, in degrees: the tail's error grew
                    // linearly along it (0.05 to 0.13) without.
                    layer_placement.model_pitch = segments.bob * 2.0F;
                    render::emit_entity(*layer.model, scratch.poses, layer_placement, vertices);
                    const i32 neck = layer.model->bone("neck");
                    if (neck >= 0) {
                        const auto draw_segment = [&](const render::DragonSegment& segment) {
                            // Everything hidden but the neck and the bones above
                            // it: hiding `root` hid the neck with it, and not one
                            // segment was drawn.
                            for (render::BonePose& pose : scratch.poses) {
                                pose.hidden = true;
                            }
                            for (i32 up = neck; up >= 0;
                                 up = layer.model->bones[static_cast<usize>(up)].parent) {
                                scratch.poses[static_cast<usize>(up)].hidden = false;
                            }
                            render::BonePose& pose = scratch.poses[static_cast<usize>(neck)];
                            pose.offset = segment.pivot - layer.model->bones[static_cast<usize>(neck)].pivot;
                            pose.rotation = segment.rotation;
                            render::emit_entity(*layer.model, scratch.poses, layer_placement, vertices);
                        };
                        for (const render::DragonSegment& segment : segments.neck) {
                            draw_segment(segment);
                        }
                        for (const render::DragonSegment& segment : segments.tail) {
                            draw_segment(segment);
                        }
                    }
                } else if (crystal) {
                    // The base as the model has it, then the three nested
                    // parts, each drawn alone through its own map. The game
                    // ignores a crystal's yaw.
                    layer_placement.body_yaw = 0.0F;
                    const auto only = [&](i32 bone) {
                        for (render::BonePose& pose : scratch.poses) {
                            pose        = render::BonePose{};
                            pose.hidden = true;
                        }
                        for (i32 up = bone; up >= 0;
                             up = layer.model->bones[static_cast<usize>(up)].parent) {
                            scratch.poses[static_cast<usize>(up)].hidden = false;
                        }
                    };
                    scratch.poses.assign(layer.model->bones.size(), render::BonePose{});
                    const i32  base   = layer.model->bone("base");
                    const i32  glass  = layer.model->bone("glass");
                    const i32  core   = layer.model->bone("cube");
                    const bool bottom = !entity.traits.has(render::meta::kEndCrystalBottom) ||
                                        entity.traits.integer(render::meta::kEndCrystalBottom) != 0;
                    if (base >= 0 && bottom) {
                        only(base);
                        render::emit_entity(*layer.model, scratch.poses, layer_placement, vertices);
                    }
                    const CrystalTurns turns = crystal_turns(static_cast<f32>(entity.lived * 20.0));
                    for (usize part = 0; part < 3; ++part) {
                        const i32 bone = part < 2 ? glass : core;
                        if (bone < 0) {
                            continue;
                        }
                        only(bone);
                        render::EntityPlacement nested = layer_placement;
                        nested.has_model_basis         = true;
                        nested.model_basis             = turns.basis[part];
                        nested.model_offset            = turns.offset;
                        render::emit_entity(*layer.model, scratch.poses, nested, vertices);
                    }
                } else {
                    render::pose_model(*layer.model, look.animation, state, scratch.poses);
                    render::apply_layer_visibility(*layer.model, layer.layer, scratch.poses);
                    render::apply_look_visibility(*layer.model, look, scratch.poses);
                    render::emit_entity(*layer.model, scratch.poses, layer_placement, vertices);
                }
                drawn = submit(context, renderer, texture, vertices, entity, frame,
                               pass_name(layer.layer.pass)) ||
                        drawn;
            }

            // An end crystal's beam, to the block its metadata names: an
            // eight-sided prism of `end_crystal_beam.png`, scrolling along its
            // length, 0.75 across at the crystal and 0.15 at the target, both
            // ends two blocks up — from the glass's centre (2 + j) to two
            // blocks over the target block's centre: the game's quads for a
            // crystal aimed 4 up and 3.5 along.
            if (entity.type == "minecraft:end_crystal" &&
                entity.traits.integer(render::meta::kEndCrystalBeam) != 0 &&
                context.entity_atlas != nullptr &&
                (context.cutout != nullptr || context.dump != nullptr)) {
                if (const auto beam =
                        context.entity_atlas->find("minecraft:entity/end_crystal/end_crystal_beam")) {
                    const i64 packed = entity.traits.slots[render::meta::kEndCrystalBeam].position;
                    const auto sign  = [](i64 value, u32 bits) {
                        return value >= (i64{1} << (bits - 1)) ? value - (i64{1} << bits) : value;
                    };
                    const Vec3f target{static_cast<f32>(sign((packed >> 38) & 0x3FFFFFF, 26)) + 0.5F,
                                       static_cast<f32>(sign(packed & 0xFFF, 12)) + 0.5F,
                                       static_cast<f32>(sign((packed >> 12) & 0x3FFFFFF, 26)) + 0.5F};
                    const Vec3f start =
                        frame.position +
                        Vec3f{0.0F, 2.0F + crystal_bob(static_cast<f32>(entity.lived * 20.0)), 0.0F};
                    const Vec3f aim  = target + Vec3f{0.0F, 2.0F, 0.0F};
                    const Vec3f axis = aim - start;
                    const f32   length = axis.length();
                    if (length > 0.01F) {
                        const Vec3f along = axis * (1.0F / length);
                        const Vec3f side  = std::abs(along.y) < 0.99F
                                                ? along.cross(Vec3f{0.0F, 1.0F, 0.0F}).normalized()
                                                : Vec3f{1.0F, 0.0F, 0.0F};
                        const Vec3f other = along.cross(side);
                        constexpr f32 kNear = 0.75F;
                        constexpr f32 kFar  = 0.15F;
                        const f32 scroll = static_cast<f32>(entity.lived * 20.0) * -0.01F;
                        vertices.clear();
                        for (u32 face = 0; face < 8; ++face) {
                            const f32 a0 = static_cast<f32>(face) * std::numbers::pi_v<f32> / 4.0F;
                            const f32 a1 = static_cast<f32>(face + 1) * std::numbers::pi_v<f32> / 4.0F;
                            const Vec3f r0 = side * std::cos(a0) + other * std::sin(a0);
                            const Vec3f r1 = side * std::cos(a1) + other * std::sin(a1);
                            const f32 u0 = beam->u0 + (beam->u1 - beam->u0) * static_cast<f32>(face) / 8.0F;
                            const f32 u1 = beam->u0 + (beam->u1 - beam->u0) * static_cast<f32>(face + 1) / 8.0F;
                            const f32 v0 = beam->v0 + (beam->v1 - beam->v0) * std::fmod(std::abs(scroll), 1.0F);
                            render::emit_quad({start + r0 * kNear, start + r1 * kNear,
                                               aim + r1 * kFar, aim + r0 * kFar},
                                              {u0, u1, u1, u0}, {v0, v0, beam->v1, beam->v1},
                                              0xFFFFFFFFU, vertices, 0xFFFFFFU);
                        }
                        (void)submit(context, context.cutout, context.atlas_cutout, vertices, entity,
                                     frame, "beam");
                    }
                }
            }

            emit_held_items(context, entity, placement, state, light, frame, scratch, drawn);

            if (entity.type.ends_with("minecart") &&
                (context.cutout != nullptr || context.dump != nullptr)) {
                vertices.clear();
                emit_minecart_block(context, entity, placement, light, vertices);
                (void)submit(context, context.cutout, context.block_atlas, vertices, entity, frame, "block");
            }
            break;
        }

        case render::EntityKind::DroppedItem: {
            if (context.cutout == nullptr && context.dump == nullptr) {
                return false;
            }
            // Turned about Y at the stack's own pace and bobbing, a quarter of
            // a block across: the game's size for a dropped stack.
            const f32 spin = static_cast<f32>(entity.lived * 20.0) * 4.0F / kDegrees;
            const Vec3f right{std::cos(spin), 0.0F, std::sin(spin)};
            const f32 bob = 0.0625F * std::sin(static_cast<f32>(entity.lived * 20.0) / 10.0F);
            vertices.clear();
            if (emit_item_icon(context, entity.item,
                               frame.position + Vec3f{0.0F, 0.125F + 0.1F + bob, 0.0F}, right,
                               Vec3f{0.0F, 1.0F, 0.0F}, 0.25F, light, vertices)) {
                drawn = submit(context, context.cutout, context.block_atlas, vertices, entity, frame, "block");
            }
            break;
        }

        case render::EntityKind::ThrownItem: {
            if (context.cutout == nullptr && context.dump == nullptr) {
                return false;
            }
            std::string_view item = entity.item;
            std::string       fallback;
            if (item.empty()) {
                // A projectile summoned without an item draws its type's own.
                const std::string_view type = std::string_view(entity.type).substr(10);
                if (type == "potion") {
                    fallback = "minecraft:splash_potion";
                } else if (type == "fireball" || type == "small_fireball") {
                    fallback = "minecraft:fire_charge";
                } else if (type == "eye_of_ender") {
                    fallback = "minecraft:ender_eye";
                } else {
                    fallback = entity.type;
                }
                item = fallback;
            }
            const f32 size = entity.type == "minecraft:fireball"         ? 1.5F
                             : entity.type == "minecraft:small_fireball" ? 0.375F
                                                                          : 0.5F;
            const Billboard board = facing_camera(context);
            vertices.clear();
            if (emit_item_icon(context, item, frame.position + Vec3f{0.0F, entity.height * 0.5F, 0.0F},
                               board.right, board.up, size, light, vertices)) {
                drawn = submit(context, context.cutout, context.block_atlas, vertices, entity, frame, "block");
            }
            break;
        }

        case render::EntityKind::Billboard: {
            if ((context.translucent == nullptr && context.dump == nullptr) ||
                context.entity_atlas == nullptr) {
                return false;
            }
            const auto rect = context.entity_atlas->find("minecraft:entity/enderdragon/dragon_fireball");
            if (!rect) {
                return false;
            }
            vertices.clear();
            // Two blocks square, turned to the camera, from half a block under
            // the entity's position to a block and a half over it, along the
            // camera's own up: the game's quad spans y −0.494..1.482 seen from
            // 8.8° above.
            const Billboard board = facing_camera(context);
            emit_sprite(context, *rect, frame.position - board.up * 0.5F, 2.0F, 2.0F, 0xFFFFFFFFU,
                        0xFFFFFFU, vertices);
            drawn = submit(context, context.translucent, context.atlas_translucent, vertices, entity,
                           frame, "translucent");
            break;
        }

        case render::EntityKind::ExperienceOrb: {
            if ((context.translucent == nullptr && context.dump == nullptr) ||
                context.entity_atlas == nullptr) {
                return false;
            }
            const auto sheet = context.entity_atlas->find("minecraft:entity/experience_orb");
            if (!sheet) {
                return false;
            }
            // The icon steps with the value (the sizes the wiki's experience
            // orb article lists), sixteen cells of 16×16 on a 64×64 sheet.
            constexpr std::array<i32, 10> kThresholds{3, 7, 17, 37, 73, 149, 307, 617, 1237, 2477};
            i32 icon = 0;
            for (const i32 threshold : kThresholds) {
                if (entity.data >= threshold) {
                    ++icon;
                }
            }
            const f32 cell_u = static_cast<f32>(icon % 4) / 4.0F;
            const f32 cell_v = static_cast<f32>(icon / 4) / 4.0F;
            const render::UvRect rect{sheet->u0 + (sheet->u1 - sheet->u0) * cell_u,
                                      sheet->v0 + (sheet->v1 - sheet->v0) * cell_v,
                                      sheet->u0 + (sheet->u1 - sheet->u0) * (cell_u + 0.25F),
                                      sheet->v0 + (sheet->v1 - sheet->v0) * (cell_v + 0.25F)};
            // Green shading into yellow on the orb's age.
            const f32 t = static_cast<f32>(entity.lived * 20.0) / 2.0F;
            const auto red  = static_cast<u32>((std::sin(t) + 1.0F) * 0.5F * 255.0F);
            const auto blue = static_cast<u32>((std::sin(t + 4.1887903F) + 1.0F) * 0.1F * 255.0F);
            const u32  tint = 0x80000000U | (red << 16U) | 0xFF00U | blue;
            vertices.clear();
            emit_sprite(context, rect, frame.position + Vec3f{0.0F, 0.1F - 0.075F, 0.0F}, 0.3F, 0.3F,
                        tint, light, vertices);
            drawn = submit(context, context.translucent, context.atlas_translucent, vertices, entity,
                           frame, "translucent");
            break;
        }

        case render::EntityKind::Block: {
            if ((context.cutout == nullptr && context.dump == nullptr) ||
                context.block_registry == nullptr) {
                return false;
            }
            std::optional<registry::BlockStateId> state;
            u32 block_overlay = render::kNoOverlay;
            if (entity.type == "minecraft:tnt") {
                if (const auto id = context.block_registry->find_block("minecraft:tnt")) {
                    state = context.block_registry->default_state(*id);
                }
                const i64 fuse = entity.traits.integer(render::meta::kTntFuse, 80);
                if ((fuse / 5) % 2 == 0) {
                    block_overlay = kWhiteOverlay;
                }
            } else {
                state = registry::BlockStateId{static_cast<u16>(entity.data)};
            }
            if (!state) {
                return false;
            }
            vertices.clear();
            const Vec3f corner = frame.position - Vec3f{0.5F, 0.0F, 0.5F};
            if (emit_block(
                    context, *state, [&](Vec3f point) { return corner + point; }, light,
                    block_overlay, vertices)) {
                drawn = submit(context, context.cutout, context.block_atlas, vertices, entity, frame, "block");
            }
            break;
        }

        case render::EntityKind::ItemFrame:
        case render::EntityKind::Refused:
        case render::EntityKind::Invisible:
        case render::EntityKind::Unknown:
            return false;
    }

    // What every living thing may carry on top: the fire, and its name.
    const render::EntityTraits& traits = entity.traits;
    if (drawn && (context.cutout != nullptr || context.dump != nullptr) &&
        traits.flag(render::meta::kSharedFlags, render::meta::kSharedOnFire) &&
        !fire_immune(entity.type)) {
        vertices.clear();
        emit_fire(context, entity, frame.position, vertices);
        (void)submit(context, context.cutout, context.block_atlas, vertices, entity, frame, "block");
    }
    const bool name_shown = !traits.name.empty() &&
                            (traits.integer(render::meta::kCustomNameVisible) != 0 ||
                             entity.type == "minecraft:player");
    if (drawn && name_shown &&
        ((context.cutout != nullptr && context.translucent != nullptr) || context.dump != nullptr)) {
        std::vector<render::EntityVertex>& plate = scratch.plate;
        vertices.clear();
        plate.clear();
        emit_name_tag(context, traits.name,
                      frame.position + Vec3f{0.0F, entity.height + 0.5F, 0.0F}, light, vertices, plate);
        (void)submit(context, context.cutout, context.atlas_cutout, vertices, entity, frame, "name");
        (void)submit(context, context.translucent, context.atlas_translucent, plate, entity, frame,
                     "name_plate");
    }
    return drawn;
}

bool write_entity_dump(const std::string& path, const std::vector<EntityDumpRecord>& records) {
    std::string           json = "[\n";
    std::array<char, 256> number{};
    for (usize index = 0; index < records.size(); ++index) {
        const EntityDumpRecord& entry = records[index];
        json += index == 0 ? "" : ",\n";
        json += "{\"id\": " + std::to_string(entry.id) + ", \"type\": \"" + entry.type +
                "\", \"pass\": \"" + std::string(entry.pass) + "\", \"origin\": [";
        int length = std::snprintf(number.data(), number.size(), "%.5f, %.5f, %.5f",
                                   static_cast<f64>(entry.origin.x), static_cast<f64>(entry.origin.y),
                                   static_cast<f64>(entry.origin.z));
        json.append(number.data(), static_cast<usize>(std::max(length, 0)));
        json += "], \"vertices\": [";
        for (usize v = 0; v < entry.vertices.size(); ++v) {
            const render::EntityVertex& vertex = entry.vertices[v];
            length = std::snprintf(
                number.data(), number.size(),
                "%s[%.5f, %.5f, %.5f, %.6f, %.6f, %d, %d, %d, %d, %d, %d, %d, %d]", v == 0 ? "" : ", ",
                static_cast<f64>(vertex.x - entry.origin.x), static_cast<f64>(vertex.y - entry.origin.y),
                static_cast<f64>(vertex.z - entry.origin.z), static_cast<f64>(vertex.u),
                static_cast<f64>(vertex.v), vertex.colour[0], vertex.colour[1], vertex.colour[2],
                vertex.colour[3], vertex.overlay[0], vertex.overlay[3], vertex.light[0], vertex.light[1]);
            json.append(number.data(), static_cast<usize>(std::max(length, 0)));
        }
        json += "]}";
    }
    json += "\n]\n";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    std::fwrite(json.data(), 1, json.size(), file);
    std::fclose(file);
    return true;
}

namespace {

/// One `m<index>=<value>` token of a recorded state, into a metadata slot.
void read_meta_token(std::string_view token, render::EntityTraits& traits) {
    const usize equals = token.find('=');
    if (token.size() < 3 || equals == std::string_view::npos) {
        return;
    }
    if (token[0] == 'e') {
        // `e<slot>=<item>`: the game's EquipmentSlot order — main hand, off
        // hand, feet, legs, chest, head — which is Set Equipment's numbering.
        const int slot = std::atoi(std::string(token.substr(1, equals - 1)).c_str());
        if (slot >= 0 && static_cast<usize>(slot) < traits.equipment.size()) {
            traits.equipment[static_cast<usize>(slot)] = std::string(token.substr(equals + 1));
        }
        return;
    }
    if (token[0] != 'm') {
        return;
    }
    const int index = std::atoi(std::string(token.substr(1, equals - 1)).c_str());
    const std::string_view value = token.substr(equals + 1);
    if (value.starts_with("t:")) {
        std::string name(value.substr(2));
        std::replace(name.begin(), name.end(), '_', ' ');
        if (index == render::meta::kCustomName) {
            traits.name = std::move(name);
        }
    }
    if (index < 0 || static_cast<usize>(index) >= render::kMetaSlots || value == "?") {
        return;
    }
    render::MetaSlot& slot = traits.slots[static_cast<usize>(index)];
    slot.present           = true;
    if (value.starts_with("v:")) {
        std::array<int, 3> parts{};
        std::sscanf(std::string(value.substr(2)).c_str(), "%d,%d,%d", &parts[0], &parts[1],
                    &parts[2]);
        slot.villager = {parts[0], parts[1], parts[2]};
    } else if (value.starts_with("r:")) {
        std::array<float, 3> parts{};
        std::sscanf(std::string(value.substr(2)).c_str(), "%f,%f,%f", &parts[0], &parts[1],
                    &parts[2]);
        slot.rotation = {parts[0], parts[1], parts[2]};
    } else if (value.starts_with("b:")) {
        slot.integer = std::atoll(std::string(value.substr(2)).c_str());
    } else if (value.starts_with("p:")) {
        // A present optional position (a crystal's beam target), packed.
        slot.integer  = 1;
        slot.position = std::atoll(std::string(value.substr(2)).c_str());
    } else if (value.starts_with("t:")) {
        slot.integer = 1;
    } else {
        const std::string text(value);
        slot.integer = std::atoll(text.c_str());
        slot.real    = static_cast<f32>(std::atof(text.c_str()));
    }
}

}  // namespace

usize check_entity_states(const std::string& states_dir, const std::string& out_dir,
                          const registry::Registries* registries, const LookResources& resources,
                          const EntityDrawContext& base_context) {
    std::error_code error;
    std::filesystem::create_directories(out_dir, error);
    usize written = 0;
    for (const auto& file : std::filesystem::directory_iterator(states_dir, error)) {
        if (file.path().extension() != ".state") {
            continue;
        }
        std::FILE* input = std::fopen(file.path().string().c_str(), "rb");
        if (input == nullptr) {
            continue;
        }
        EntityWorld world;
        // The scene's camera, which billboards, name tags and fire face: a
        // `camera <yaw> <pitch>` line of the state file.
        f32 camera_yaw   = base_context.camera_yaw;
        f32 camera_pitch = base_context.camera_pitch;
        std::array<char, 4096> line{};
        while (std::fgets(line.data(), static_cast<int>(line.size()), input) != nullptr) {
            std::string_view rest(line.data());
            std::vector<std::string_view> tokens;
            while (!rest.empty()) {
                const usize start = rest.find_first_not_of(" \t\r\n");
                if (start == std::string_view::npos) {
                    break;
                }
                rest              = rest.substr(start);
                const usize end   = rest.find_first_of(" \t\r\n");
                tokens.push_back(rest.substr(0, end));
                rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
            }
            if (tokens.size() >= 3 && tokens[0] == "camera") {
                camera_yaw   = static_cast<f32>(std::atof(std::string(tokens[1]).c_str()));
                camera_pitch = static_cast<f32>(std::atof(std::string(tokens[2]).c_str()));
                continue;
            }
            if (tokens.size() < 10 || tokens[0] != "entity") {
                continue;
            }
            TrackedEntity entity;
            entity.type = std::string(tokens[1]);
            entity.id   = std::atoi(std::string(tokens[2]).c_str());
            entity.to   = Vec3d{std::atof(std::string(tokens[3]).c_str()),
                              std::atof(std::string(tokens[4]).c_str()),
                              std::atof(std::string(tokens[5]).c_str())};
            entity.from = entity.to;
            entity.body_yaw = static_cast<f32>(std::atof(std::string(tokens[7]).c_str()));
            entity.head_yaw = static_cast<f32>(std::atof(std::string(tokens[8]).c_str()));
            entity.pitch    = static_cast<f32>(std::atof(std::string(tokens[9]).c_str()));
            entity.kind     = render::entity_kind(entity.type);
            entity.history.push(static_cast<f32>(entity.to.y), entity.body_yaw);
            if (registries != nullptr) {
                if (const auto types = registries->find("minecraft:entity_type")) {
                    if (const auto id = registries->protocol_id(*types, entity.type)) {
                        if (const auto info = registries->entity_type(*id)) {
                            entity.width  = info->width;
                            entity.height = info->height;
                        }
                    }
                }
            }
            for (usize index = 10; index < tokens.size(); ++index) {
                if (tokens[index].starts_with("ct=")) {
                    // A crystal's own clock, which its spin and bob read (it
                    // comes after `t=` and wins over it).
                    entity.lived =
                        (std::atof(std::string(tokens[index].substr(3)).c_str()) + 1.0) / 20.0;
                    continue;
                }
                if (tokens[index].starts_with("t=")) {
                    // Its age, which every time-driven animation reads — one
                    // tick on, because the oracle renders at a partial tick of 1.
                    entity.lived =
                        (std::atof(std::string(tokens[index].substr(2)).c_str()) + 1.0) / 20.0;
                    continue;
                }
                read_meta_token(tokens[index], entity.traits);
            }
            // A dropped item's stack travels as its own field: the check
            // leaves it empty and a dropped item draws nothing here.
            world.adopt(std::move(entity));
        }
        std::fclose(input);
        world.refresh_looks(resources);

        std::vector<EntityDumpRecord> records;
        EntityDrawContext             context = base_context;
        context.dump                          = &records;
        context.camera_yaw                    = camera_yaw;
        context.camera_pitch                  = camera_pitch;
        EntityScratch scratch;
        for (const auto& [id, entity] : world.entities()) {
            (void)draw_entity(context, entity, world.frame_of(entity, false), 0xFFFFFFU, scratch);
        }
        const std::string out = out_dir + "/" + file.path().stem().string() + ".json";
        if (write_entity_dump(out, records)) {
            ++written;
        }
    }
    return written;
}

}  // namespace ov::demo
