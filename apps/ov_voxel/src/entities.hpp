// What the client believes is standing in the world, and where it draws it.
//
// The server sends **deltas quantised to a 4096th of a block**, twenty times a
// second, and nothing in between. Drawing a mob at the last position it
// reported makes it jump twenty times a second at any frame rate above twenty —
// so this holds two positions per entity, the one before the last packet and
// the one after it, and renders between them. § "L'interpolation" of
// docs/provenance/rendu-entites.md gives the measured difference.
//
// Two things the server does **not** send, named here rather than invented
// quietly further down:
//
//   • **A mob's rotation.** `ov_server` sends Spawn Entity once and then only
//     position deltas and teleports; no 0x2C, no 0x2D, and head rotation only
//     for players. A mob's facing is therefore derived here from the direction
//     it is moving in, which is right whenever it is walking and holds its last
//     value when it stops. A vanilla server would send the real angle.
//   • **A mob's head.** Same reason. A mob's head is drawn in line with its
//     body; a player's follows Entity Head Rotation, which the server does
//     send.
//
// This lives in the application rather than in ov_client because it knows the
// protocol: `netclient::ClientEvents` is a layer 12 type and a renderer must
// not read one. The renderer is handed world-space vertices.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/entity_renderer.hpp"
#include "ov/math/vec.hpp"
#include "ov/netclient/client.hpp"
#include "ov/registry/registries.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/entity_mesh.hpp"
#include "ov/render/entity_model.hpp"
#include "ov/render/entity_pose.hpp"
#include "ov/render/environment.hpp"
#include "ov/render/item_model.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ov::demo {

/// One entity, as this client believes it to be.
struct TrackedEntity {
    i32 id{0};
    /// Resolved once at spawn: `minecraft:zombie`. Empty when the type id was
    /// not in the registry, which is reported rather than drawn as something.
    std::string type;

    /// The two ends of the interpolation. `from` is where the entity was drawn
    /// when the last packet arrived; `to` is where that packet put it.
    Vec3d from{};
    Vec3d to{};
    /// Seconds since the packet that set `to`.
    f64 age{0.0};

    f32 body_yaw{0.0F};
    f32 previous_body_yaw{0.0F};
    f32 head_yaw{0.0F};
    f32 previous_head_yaw{0.0F};
    f32 pitch{0.0F};

    render::WalkState walk;

    /// The stack a dropped item carries, resolved to an item name.
    std::string item;
    i32         item_count{0};

    /// Experience orbs are their own packet and carry a value rather than a
    /// stack.
    bool orb{false};

    /// True once a Move or Teleport has been seen, so that the first frame
    /// after a spawn does not interpolate from nowhere.
    bool moved{false};
};

/// How an entity is drawn this frame.
struct EntityFrame {
    Vec3f position{};
    f32   body_yaw{0.0F};
};

/// The client's view of everything that moves.
class EntityWorld {
public:
    /// One tick, in seconds. The interpolation window: the server sends a
    /// position packet a tick, so a frame lands somewhere inside the last one.
    static constexpr f64 kTickSeconds = 0.05;

    /// Fold in one poll's worth of entity packets.
    void apply(const netclient::ClientEvents& events, const registry::Registries* registries);

    /// Advance the interpolation clock. Called once a frame with the real
    /// elapsed time — the only place in this client where a wall clock is
    /// allowed to touch a position, and it never reaches the server.
    void advance(f64 seconds);

    /// Where and how an entity is drawn now.
    ///
    /// With `interpolate` false the entity is drawn at the last packet's
    /// position, which is what makes the measurement in the provenance
    /// document possible: the same run, the same packets, one number each way.
    [[nodiscard]] EntityFrame frame_of(const TrackedEntity& entity, bool interpolate) const;

    [[nodiscard]] const std::unordered_map<i32, TrackedEntity>& entities() const noexcept {
        return entities_;
    }

    /// Entity type ids the registry knew nothing about, and types with no model
    /// in this build. Reported once at the end of a run rather than per frame.
    [[nodiscard]] const std::vector<std::string>& unknown_types() const noexcept {
        return unknown_;
    }

private:
    void note_unknown(std::string text);

    std::unordered_map<i32, TrackedEntity> entities_;
    std::vector<std::string>               unknown_;
};

/// Everything the drawing half needs, gathered once at startup.
struct EntityDrawContext {
    const render::EntityModelSet* models{nullptr};
    /// Model name to the texture uploaded for it.
    const std::unordered_map<std::string, client::EntityTexture>* textures{nullptr};
    /// The block atlas, borrowed: a dropped stack is drawn from the same sheet
    /// the terrain is, because that is where its sprites were stitched.
    client::EntityTexture         atlas{client::EntityTexture::Invalid};
    const render::ItemModelCache* items{nullptr};
    /// The tint a grass or leaf face takes. One value, as the interface does.
    u32 foliage_tint{0xFFFFFFU};
};

/// The two buffers the drawing half reuses between entities.
///
/// A member of the caller rather than a static: a frame draws a hundred
/// entities and neither of these may allocate after the first one, and a
/// function-local static would be a mutable global with a lock on it.
struct EntityScratch {
    std::vector<render::EntityVertex> vertices;
    std::vector<render::BonePose>     poses;
};

/// Build the quads for one entity and hand them to the renderer.
///
/// `light` is the lightmap colour at the entity's block, already sampled: an
/// entity is lit as a whole, which is what vanilla does and what keeps this to
/// one lookup per entity rather than one per fragment.
///
/// Returns false when nothing was drawn — no model, no texture, or a full ring.
bool draw_entity(client::EntityRenderer& renderer, const EntityDrawContext& context,
                 const TrackedEntity& entity, const EntityFrame& frame,
                 std::array<u8, 3> light, EntityScratch& scratch);

}  // namespace ov::demo
