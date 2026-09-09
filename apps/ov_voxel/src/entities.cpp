#define OV_LOG_CATEGORY "voxel"

#include "entities.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ov::demo {

namespace {

constexpr f32 kDegrees = 180.0F / std::numbers::pi_v<f32>;

/// Below this, a move is noise from the quantiser rather than a direction.
///
/// One quantum is 1/4096 = 0.000244 blocks. Ten of them is 0.0024, which a
/// walking mob covers in a tenth of a tick and a standing one never does: the
/// server's own dead zone before it sends anything at all is half a quantum.
constexpr f64 kFacingThreshold = 0.0024;

[[nodiscard]] f32 yaw_towards(f64 dx, f64 dz) noexcept {
    // Minecraft's convention, and it is not atan2(dz, dx). Yaw 0 looks towards
    // +Z and grows clockwise from above, so forward is (-sin, cos): the
    // arguments are (-dx, dz) and in that order.
    return static_cast<f32>(std::atan2(-dx, dz)) * kDegrees;
}

/// The signed difference between two angles, in (-180, 180].
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

/// The shorter way round between two angles, so that a mob turning past south
/// does not spin the long way.
[[nodiscard]] f32 lerp_angle(f32 from, f32 to, f32 alpha) noexcept {
    return from + wrap_degrees(to - from) * alpha;
}

[[nodiscard]] u32 tint_from_light(std::array<u8, 3> light) noexcept {
    return 0xFF000000U | (static_cast<u32>(light[0]) << 16U) |
           (static_cast<u32>(light[1]) << 8U) | static_cast<u32>(light[2]);
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

    for (const netclient::ClientEvents::EntityChange& change : events.entities) {
        switch (change.kind) {
            case Kind::Spawn: {
                TrackedEntity entity;
                entity.id   = change.id;
                entity.from = change.position;
                entity.to   = change.position;
                entity.age  = EntityWorld::kTickSeconds;

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
                    }
                }
                if (entity.type.empty()) {
                    note_unknown("entity type id " + std::to_string(change.type));
                }

                entity.body_yaw          = change.yaw;
                entity.previous_body_yaw = change.yaw;
                entity.head_yaw          = change.head_yaw;
                entity.previous_head_yaw = change.head_yaw;
                entity.pitch             = change.pitch;
                entities_[change.id]     = std::move(entity);
                break;
            }

            case Kind::Move:
            case Kind::Teleport: {
                auto found = entities_.find(change.id);
                if (found == entities_.end()) {
                    // A move for something never spawned. Vanilla ignores it
                    // too; naming it would flood the log on every join.
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
                    // The packet carried real angles.
                    entity.body_yaw = change.yaw;
                    entity.head_yaw = change.head_yaw;
                    entity.pitch    = change.pitch;
                } else if (horizontal > kFacingThreshold) {
                    // Derived, because this server sends no rotation for a mob.
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
                if (found == entities_.end() || !change.stack) {
                    break;
                }
                TrackedEntity& entity = found->second;
                entity.item_count     = change.stack->count;
                entity.item.clear();
                if (registries != nullptr && !change.stack->empty()) {
                    if (const auto items = registries->find("minecraft:item")) {
                        entity.item = std::string(registries->entry_of(
                            *items, static_cast<registry::ProtocolId>(change.stack->item_id)));
                    }
                }
                break;
            }

            case Kind::Remove:
                entities_.erase(change.id);
                break;
        }
    }
}

void EntityWorld::advance(f64 seconds) {
    for (auto& [id, entity] : entities_) {
        entity.age += seconds;
        // A mob that stops moving stops sending packets, and its walk state
        // must decay or its legs keep swinging while it stands. The server's
        // own dead zone means silence *is* the message.
        if (entity.age > kTickSeconds * 2.0) {
            render::advance_walk(entity.walk, 0.0F);
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

    // Clamped at one: a frame that arrives more than a tick after the last
    // packet has nothing left to interpolate towards, and extrapolating would
    // send a mob through a wall the moment the network hiccups.
    const f64 alpha = std::clamp(entity.age / kTickSeconds, 0.0, 1.0);
    frame.position =
        Vec3f{static_cast<f32>(entity.from.x + (entity.to.x - entity.from.x) * alpha),
              static_cast<f32>(entity.from.y + (entity.to.y - entity.from.y) * alpha),
              static_cast<f32>(entity.from.z + (entity.to.z - entity.from.z) * alpha)};
    frame.body_yaw =
        lerp_angle(entity.previous_body_yaw, entity.body_yaw, static_cast<f32>(alpha));
    return frame;
}

namespace {

/// A dropped stack, drawn as the picture the inventory would show of it.
///
/// ⚠ Vanilla draws a dropped item as a *solid* thing: a block item is the block
/// model, a tool is its sprite extruded a sixteenth thick. This draws the GUI
/// icon instead — the same quads, the same atlas rects, the same flat shading —
/// on one plane, turned about Y and bobbing. The silhouette and the textures
/// are the item's own; the thickness is not there. It is named here rather than
/// presented as vanilla's.
bool emit_item(const EntityDrawContext& context, const TrackedEntity& entity,
               const EntityFrame& frame, u32 tint,
               std::vector<render::EntityVertex>& out) {
    if (context.items == nullptr || entity.item.empty()) {
        return false;
    }
    const render::ItemMesh* mesh = context.items->mesh(entity.item);
    if (mesh == nullptr || !mesh->drawable) {
        return false;
    }

    // A quarter of a block, which is vanilla's size for a dropped stack.
    constexpr f32 kScale  = 0.25F;
    constexpr f32 kBobAmp = 0.0625F;

    const f32 yaw   = frame.body_yaw;
    const f32 sin_y = std::sin(yaw / kDegrees);
    const f32 cos_y = std::cos(yaw / kDegrees);
    // The bob rides the same clock the spin does, a quarter turn behind, so a
    // stack is at its highest when it faces you.
    const f32 bob = kBobAmp * std::sin(yaw / kDegrees);

    // Cell pixels (0..16, y downward, origin top-left) to a plane through the
    // entity, facing -Z before the yaw is applied.
    const auto place = [&](render::ItemPoint point) {
        const f32 local_x = (point.x / render::kItemCellSize - 0.5F) * kScale;
        const f32 local_y = (0.5F - point.y / render::kItemCellSize) * kScale;
        return Vec3f{frame.position.x + local_x * cos_y,
                     frame.position.y + kScale * 0.5F + local_y + bob,
                     frame.position.z + local_x * sin_y};
    };

    const usize before = out.size();

    if (mesh->flat) {
        for (const render::SpriteUv& layer : mesh->layers) {
            const std::array<Vec3f, 4> corners{
                place(render::ItemPoint{0.0F, render::kItemCellSize}),
                place(render::ItemPoint{render::kItemCellSize, render::kItemCellSize}),
                place(render::ItemPoint{render::kItemCellSize, 0.0F}),
                place(render::ItemPoint{0.0F, 0.0F})};
            const std::array<f32, 4> u{layer.u0, layer.u1, layer.u1, layer.u0};
            const std::array<f32, 4> v{layer.v1, layer.v1, layer.v0, layer.v0};
            render::emit_quad(corners, u, v, tint, out);
            // Both facings: a flat icon seen from behind must not vanish, and
            // the pipeline culls back faces.
            const std::array<Vec3f, 4> back{corners[3], corners[2], corners[1], corners[0]};
            const std::array<f32, 4>   back_u{u[3], u[2], u[1], u[0]};
            const std::array<f32, 4>   back_v{v[3], v[2], v[1], v[0]};
            render::emit_quad(back, back_u, back_v, tint, out);
        }
    } else {
        for (const render::ItemQuad& quad : mesh->quads) {
            const u32 shaded = 0xFF000000U |
                               (static_cast<u32>(static_cast<f32>((tint >> 16U) & 0xFFU) *
                                                 quad.shade) << 16U) |
                               (static_cast<u32>(static_cast<f32>((tint >> 8U) & 0xFFU) *
                                                 quad.shade) << 8U) |
                               static_cast<u32>(static_cast<f32>(tint & 0xFFU) * quad.shade);
            const std::array<Vec3f, 4> corners{place(quad.position[0]), place(quad.position[1]),
                                               place(quad.position[2]),
                                               place(quad.position[3])};
            const std::array<f32, 4>   u{quad.uv[0].x, quad.uv[1].x, quad.uv[2].x, quad.uv[3].x};
            const std::array<f32, 4>   v{quad.uv[0].y, quad.uv[1].y, quad.uv[2].y, quad.uv[3].y};
            render::emit_quad(corners, u, v, shaded, out);
            const std::array<Vec3f, 4> back{corners[3], corners[2], corners[1], corners[0]};
            const std::array<f32, 4>   back_u{u[3], u[2], u[1], u[0]};
            const std::array<f32, 4>   back_v{v[3], v[2], v[1], v[0]};
            render::emit_quad(back, back_u, back_v, shaded, out);
        }
    }

    return out.size() > before;
}

}  // namespace

bool draw_entity(client::EntityRenderer& renderer, const EntityDrawContext& context,
                 const TrackedEntity& entity, const EntityFrame& frame,
                 std::array<u8, 3> light, EntityScratch& scratch) {
    const u32 tint = tint_from_light(light);
    scratch.vertices.clear();

    // A dropped stack and an orb are not models: they have no bones.
    if (entity.type == "minecraft:item") {
        if (!emit_item(context, entity, frame, tint, scratch.vertices)) {
            return false;
        }
        return renderer.submit(context.atlas, scratch.vertices);
    }
    if (entity.orb) {
        // Refused by name. The orb's texture is an animated strip in
        // `entity/experience_orb.png` and its size steps with the value it
        // carries; neither is measured here, and a green square would be a
        // different thing wearing its name.
        return false;
    }

    const std::string_view model_name = render::entity_model_name(entity.type);
    if (model_name.empty() || context.models == nullptr) {
        return false;
    }

    render::WalkState state = entity.walk;
    // The head is given relative to the body, which is what the model wants,
    // and clamped the way vanilla clamps a mob's neck.
    state.head_yaw   = std::clamp(wrap_degrees(entity.head_yaw - entity.body_yaw),
                                  -75.0F, 75.0F);
    state.head_pitch = std::clamp(entity.pitch, -90.0F, 90.0F);

    render::EntityPlacement placement;
    placement.position = frame.position;
    placement.body_yaw = frame.body_yaw;
    placement.tint     = tint;

    // A sheep is two models rendered one over the other, exactly as vanilla
    // does it: the skin, then the wool on its own texture. Everything else is
    // one.
    const std::array<std::string_view, 2> layers{
        model_name, entity.type == "minecraft:sheep" ? std::string_view{"sheep_fur"}
                                                     : std::string_view{}};

    bool drawn = false;
    for (const std::string_view layer : layers) {
        if (layer.empty()) {
            continue;
        }
        const render::EntityModel* model = context.models->find(layer);
        if (model == nullptr) {
            continue;
        }
        const auto texture = context.textures->find(std::string(layer));
        if (texture == context.textures->end()) {
            continue;
        }
        render::pose_model(*model, render::entity_animation(entity.type), state, scratch.poses);
        scratch.vertices.clear();
        render::emit_entity(*model, scratch.poses, placement, scratch.vertices);
        drawn = renderer.submit(texture->second, scratch.vertices) || drawn;
    }
    return drawn;
}

}  // namespace ov::demo
