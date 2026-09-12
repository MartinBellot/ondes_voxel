// What the client believes is standing in the world, and where it draws it.
//
// The server sends **deltas quantised to a 4096th of a block**, twenty times a
// second, and nothing in between. Drawing a mob at the last position it
// reported makes it jump twenty times a second at any frame rate above twenty —
// so this holds two positions per entity, the one before the last packet and
// the one after it, and renders between them. § "L'interpolation" of
// docs/provenance/rendu-entites.md gives the measured difference.
//
// ── entity-models ── Beyond where an entity is, this now keeps what it looks
// like: every metadata field the server sent (a sheep's fleece, a villager's
// profession, a slime's size), what it wears and holds, whether it is burning,
// hurt or dying, and what it rides. What those numbers *mean* per species is
// ov_render's table (entity_look.hpp); this file only folds the packets in and
// hands the drawing half world-space vertices.
//
// This lives in the application rather than in ov_client because it knows the
// protocol: `netclient::ClientEvents` is a layer 12 type and a renderer must
// not read one.
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/entity_renderer.hpp"
#include "ov/math/vec.hpp"
#include "ov/netclient/client.hpp"
#include "ov/registry/registries.hpp"
#include "ov/render/atlas.hpp"
#include "ov/render/block_models.hpp"
#include "ov/render/entity_atlas.hpp"
#include "ov/render/entity_look.hpp"
#include "ov/render/entity_mesh.hpp"
#include "ov/render/entity_model.hpp"
#include "ov/render/entity_pose.hpp"
#include "ov/render/font.hpp"
#include "ov/render/item_model.hpp"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ov::demo {

/// One layer of an entity's look, resolved once against the model set and the
/// atlas so a frame does no string lookups.
struct CachedLayer {
    const render::EntityModel* model{nullptr};
    render::UvRect             uv{};
    render::LookLayer          layer;
    /// For an Energy layer: its own texture in the energy renderer.
    client::EntityTexture energy{client::EntityTexture::Invalid};
};

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

    /// The stack a dropped item or an item frame carries, resolved to a name.
    std::string item;
    i32         item_count{0};

    /// Experience orbs are their own packet and carry a value rather than a
    /// stack.
    bool orb{false};

    /// True once a Move or Teleport has been seen, so that the first frame
    /// after a spawn does not interpolate from nowhere.
    bool moved{false};

    // ── entity-models ──
    render::EntityKind   kind{render::EntityKind::Unknown};
    render::EntityTraits traits;
    /// Spawn Entity's data field: a falling block's state, an orb's value.
    i32 data{0};
    /// Seconds since the client first saw the entity: drives what the game
    /// animates on time (a blaze's rods, a ghast's tentacles).
    f64 lived{0.0};
    /// Seconds of red flash left: half a second from a Damage Event.
    f64 hurt{0.0};
    /// Seconds since the death event, or negative while alive.
    f64 dying{-1.0};
    /// The server removed the entity while it was still toppling: it is kept
    /// only until the topple ends, as the game's client does.
    bool removed{false};
    /// The entity this one rides, or -1.
    i32 vehicle{-1};
    /// The collision box, from the registry: where the name tag and the fire go.
    f32 width{0.6F};
    f32 height{1.8F};

    /// The look, resolved when a metadata field or the equipment changes.
    bool                     look_dirty{true};
    render::EntityLook       look;
    std::vector<CachedLayer> layers;

    /// A dragon's last ticks of height and heading, newest first: its neck and
    /// tail follow where it has been (entity_pose.hpp, pose_dragon).
    render::DragonHistory history;
    /// A dragon's wing beat, as a fraction of a cycle, and the one before the
    /// last tick, for interpolation.
    // Half a beat: where the game's own client holds a dragon that does not
    // fly (a NoAI dragon's flap stays at 0.5, measured); a flying one moves
    // on from there.
    f32 flap{0.5F};
    f32 previous_flap{0.5F};
};

/// How an entity is drawn this frame.
struct EntityFrame {
    Vec3f position{};
    f32   body_yaw{0.0F};
};

/// Everything a Model entity's look is resolved against.
struct LookResources {
    const render::EntityModelSet* models{nullptr};
    const render::EntityAtlas*    atlas{nullptr};
    render::LookContext           context;
    /// Energy layers by texture name, uploaded to the energy renderer.
    const std::unordered_map<std::string, client::EntityTexture>* energy{nullptr};
};

/// The client's view of everything that moves.
class EntityWorld {
public:
    /// One tick, in seconds. The interpolation window: the server sends a
    /// position packet a tick, so a frame lands somewhere inside the last one.
    static constexpr f64 kTickSeconds = 0.05;

    /// Fold in one poll's worth of entity packets.
    void apply(const netclient::ClientEvents& events, const registry::Registries* registries);

    /// Advance the interpolation clock and every timer. Called once a frame
    /// with the real elapsed time — the only place in this client where a wall
    /// clock is allowed to touch a position, and it never reaches the server.
    void advance(f64 seconds);

    /// Re-resolve the look of every entity whose metadata changed since the
    /// last frame. Allocates, but only for those.
    void refresh_looks(const LookResources& resources);

    /// Where and how an entity is drawn now.
    [[nodiscard]] EntityFrame frame_of(const TrackedEntity& entity, bool interpolate) const;

    [[nodiscard]] const std::unordered_map<i32, TrackedEntity>& entities() const noexcept {
        return entities_;
    }

    /// Entity type ids the registry knew nothing about, and types with no model
    /// in this build. Reported once at the end of a run rather than per frame.
    [[nodiscard]] const std::vector<std::string>& unknown_types() const noexcept {
        return unknown_;
    }

    /// Where this client's own eye is while it rides something, or nullopt
    /// while it stands on its own feet.
    [[nodiscard]] std::optional<Vec3f> riding_eye(bool interpolate) const;

    /// This client's own entity id, from Login (play).
    [[nodiscard]] std::optional<i32> local_player() const noexcept { return local_player_; }

    /// Take an entity as it is, without a packet: the offline check feeds the
    /// states the real client recorded through here.
    void adopt(TrackedEntity entity) { entities_[entity.id] = std::move(entity); }

private:
    void note_unknown(std::string text);

    std::unordered_map<i32, TrackedEntity> entities_;
    std::vector<std::string>               unknown_;
    std::optional<i32>                     local_player_;
    /// What this client's own player rides. Kept apart from the entity table
    /// because a server never spawns a player's own entity to it.
    i32 local_vehicle_{-1};
    /// Seconds towards the next tick of the dragons' histories and wing beats.
    f64 tick_clock_{0.0};
};

/// One submission, kept for `--entity-dump`: what the parity script holds
/// against the vertices the game itself emitted for the same entity.
struct EntityDumpRecord {
    i32                               id{0};
    std::string                       type;
    std::string_view                  pass;
    Vec3f                             origin{};
    std::vector<render::EntityVertex> vertices;
};

/// Everything the drawing half needs, gathered once at startup.
struct EntityDrawContext {
    /// When set, every submission is also appended here.
    std::vector<EntityDumpRecord>* dump{nullptr};
    const render::EntityModelSet* models{nullptr};
    /// The four entity passes. Any may be null: that pass is then not drawn.
    client::EntityRenderer* cutout{nullptr};
    client::EntityRenderer* translucent{nullptr};
    client::EntityRenderer* eyes{nullptr};
    client::EntityRenderer* energy{nullptr};
    /// The entity atlas, as uploaded to each of the three passes that read it.
    client::EntityTexture atlas_cutout{client::EntityTexture::Invalid};
    client::EntityTexture atlas_translucent{client::EntityTexture::Invalid};
    client::EntityTexture atlas_eyes{client::EntityTexture::Invalid};
    /// Where the name tags' white background texel and glyphs lie on it.
    const render::EntityAtlas* entity_atlas{nullptr};
    const render::Font*        font{nullptr};
    /// The block atlas, borrowed by the cutout pass: a dropped stack, a block
    /// in a minecart, a primed TNT and the fire on a burning mob are all drawn
    /// from the sprites the terrain already uses.
    client::EntityTexture        block_atlas{client::EntityTexture::Invalid};
    const render::TextureAtlas*  block_sprites{nullptr};
    render::BlockModelCache*     blocks{nullptr};
    const registry::BlockRegistry* block_registry{nullptr};
    const render::ItemModelCache* items{nullptr};
    /// The tint a grass or leaf face takes. One value, as the interface does.
    u32 foliage_tint{0xFFFFFFU};
    /// Where the camera is and which way it looks, for what faces it.
    Vec3f camera{};
    f32   camera_yaw{0.0F};
    f32   camera_pitch{0.0F};
};

/// The buffers the drawing half reuses between entities.
///
/// A member of the caller rather than a static: a frame draws a hundred
/// entities and none of these may allocate after the first one, and a
/// function-local static would be a mutable global with a lock on it.
struct EntityScratch {
    std::vector<render::EntityVertex> vertices;
    std::vector<render::BonePose>     poses;
    /// A name tag's plate, drawn in the translucent pass under its glyphs.
    std::vector<render::EntityVertex> plate;
};

/// Build the quads for one entity and hand them to the renderers.
///
/// `light` is the lightmap colour at the entity's block, already sampled, as
/// 0xRRGGBB: an entity is lit as a whole, which is what vanilla does and what
/// keeps this to one lookup per entity rather than one per fragment.
///
/// Returns false when nothing was drawn — no model, no texture, a full ring,
/// or a kind this build does not draw.
bool draw_entity(const EntityDrawContext& context, const TrackedEntity& entity,
                 const EntityFrame& frame, u32 light, EntityScratch& scratch);

/// Write dump records as JSON: per submission the entity, the pass, and every
/// vertex relative to the entity's feet — the form scripts/compare_entity_render.py
/// reads.
bool write_entity_dump(const std::string& path, const std::vector<EntityDumpRecord>& records);

/// The offline geometry check. Reads the entity states the real client
/// recorded (`<scene>.state`, one `entity …` line each, from
/// scripts/entity_render_oracle.java) in `states_dir`, draws each through the
/// same look, pose and placement a frame uses — with no renderer attached, only
/// the dump — and writes `<scene>.json` into `out_dir`. What it checks is the
/// geometry, against the vertices the game emitted for the same entity, with
/// no server in the loop. Returns the number of scenes written.
usize check_entity_states(const std::string& states_dir, const std::string& out_dir,
                          const registry::Registries* registries, const LookResources& resources,
                          const EntityDrawContext& base_context);

}  // namespace ov::demo
