#define OV_LOG_CATEGORY "render"

#include "ov/render/item_model.hpp"

#include "ov/base/log.hpp"
#include "ov/render/ambient_occlusion.hpp"
#include "ov/render/baked_model.hpp"
#include "ov/render/model.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace ov::render {

namespace {

/// Rotation about X, then Y, then Z, in degrees.
///
/// The order is the format's: `display` writes `[x, y, z]` and the game builds
/// the rotation as X then Y then Z. Getting it backwards turns a block icon
/// through the wrong corner, which looks plausible until it is next to a
/// vanilla screenshot.
[[nodiscard]] Vec3f rotate_xyz(Vec3f point, Vec3f degrees) noexcept {
    constexpr f32 kToRadians = static_cast<f32>(std::numbers::pi / 180.0);
    const auto    apply      = [](f32& a, f32& b, f32 angle) {
        const f32 c  = std::cos(angle);
        const f32 s  = std::sin(angle);
        const f32 na = a * c - b * s;
        const f32 nb = a * s + b * c;
        a            = na;
        b            = nb;
    };
    // Applied innermost-last: v' = Rx(Ry(Rz(v))).
    apply(point.x, point.y, degrees.z * kToRadians);
    apply(point.z, point.x, degrees.y * kToRadians);
    apply(point.y, point.z, degrees.x * kToRadians);
    return point;
}

}  // namespace

struct ItemModelCache::Impl {
    const AssetSource* source;
    ModelLoader        loader;

    /// The resolved model of every item asked for, keyed by canonical name.
    std::map<std::string, const Model*, std::less<>> models;
    std::map<std::string, ItemMesh, std::less<>>     meshes;
    std::vector<std::string>                         sprites;
    usize                                            missing{0};

    explicit Impl(const AssetSource& assets) : source(&assets), loader(assets) {}

    void remember(const std::string& sprite) {
        const auto at = std::ranges::lower_bound(sprites, sprite);
        if (at == sprites.end() || *at != sprite) {
            sprites.insert(at, sprite);
        }
    }
};

ItemModelCache::ItemModelCache(const AssetSource& source)
    : impl_(std::make_unique<Impl>(source)) {}

ItemModelCache::ItemModelCache(ItemModelCache&&) noexcept            = default;
ItemModelCache& ItemModelCache::operator=(ItemModelCache&&) noexcept = default;
ItemModelCache::~ItemModelCache()                                    = default;

void ItemModelCache::resolve(std::string_view name) {
    const auto canonical = ResourceLocation::parse(name);
    if (!canonical) {
        return;
    }
    const std::string key = canonical->full();
    if (impl_->models.contains(key)) {
        return;
    }

    const auto model_location =
        ResourceLocation::make(canonical->name_space(),
                               std::string("item/") + std::string(canonical->path()));
    if (!model_location) {
        return;
    }
    const auto model = impl_->loader.load(*model_location);
    if (!model) {
        // Named rather than counted only: an item with no model file is a
        // missing icon in a hotbar, and the first question is always which.
        ++impl_->missing;
        impl_->models.emplace(key, nullptr);
        return;
    }
    impl_->models.emplace(key, *model);

    for (const auto& layer : (*model)->layers) {
        impl_->remember(layer);
    }
    for (const auto& element : (*model)->elements) {
        for (const auto& face : element.faces) {
            if (face) {
                impl_->remember(face->sprite);
            }
        }
    }
}

const std::vector<std::string>& ItemModelCache::sprites() const noexcept {
    return impl_->sprites;
}

void ItemModelCache::bake(const TextureAtlas& atlas) {
    impl_->meshes.clear();

    for (const auto& [name, model] : impl_->models) {
        ItemMesh mesh;
        if (model == nullptr) {
            impl_->meshes.emplace(name, std::move(mesh));
            continue;
        }

        mesh.hand = model->hand_display;

        // A generated model is a picture, not a solid. Vanilla extrudes the
        // icon into a thin slab so it has sides in the hand; in a GUI cell
        // nothing but the front face is ever visible, so the icon is the front
        // face and the slab is not built.
        if (model->generated || model->elements.empty()) {
            mesh.flat = true;
            for (const std::string& layer : model->layers) {
                mesh.layers.push_back(atlas.uv(layer));
            }
            mesh.drawable = !mesh.layers.empty();
            if (!mesh.drawable) {
                ++impl_->missing;
            }
            impl_->meshes.emplace(name, std::move(mesh));
            continue;
        }

        // Bake with no blockstate rotation: an item model is already the model
        // the item wants, and the only rotation that applies is the display
        // transform below.
        const ModelVariant variant{*ResourceLocation::parse("minecraft:air"), 0, 0, false, 1};
        const BakedModel   baked = ::ov::render::bake(*model, variant);

        const DisplayTransform display =
            model->gui_display.value_or(DisplayTransform{});

        for (const BakedQuad& quad : baked.quads) {
            const SpriteUv sprite = atlas.uv(quad.sprite);

            // Model space is 0..1 across the block; the transform turns about
            // the block's centre, which is what `translate(-0.5)` after the
            // display transform means in vanilla's own composition.
            std::array<Vec3f, 4> transformed{};
            for (usize i = 0; i < 4; ++i) {
                Vec3f point = quad.vertices[i].position - Vec3f{0.5F, 0.5F, 0.5F};
                point.x *= display.scale.x;
                point.y *= display.scale.y;
                point.z *= display.scale.z;
                point = rotate_xyz(point, display.rotation);
                point += display.translation;
                transformed[i] = point;
            }

            // The face's true normal, from its own winding — not from `facing`,
            // which is snapped to the nearest axis and would keep both halves
            // of a cross model or drop the wrong one.
            const Vec3f edge1  = transformed[1] - transformed[0];
            const Vec3f edge2  = transformed[2] - transformed[0];
            const Vec3f normal = edge1.cross(edge2);
            if (normal.z <= 0.0F) {
                // Facing away. Vanilla culls these too: a block icon is three
                // faces, not six, and drawing the far ones first only to cover
                // them costs twice the fill for nothing.
                continue;
            }

            ItemQuad out;
            out.shade      = quad.shade ? face_shade(quad.facing) : 1.0F;
            out.tint_index = quad.tint_index;

            f32 depth = 0.0F;
            for (usize i = 0; i < 4; ++i) {
                // Sixteen GUI pixels to the block, centred in the cell, with y
                // flipped: the model's +y is up and the screen's is down.
                out.position[i] = ItemPoint{kItemCellSize * 0.5F + transformed[i].x * kItemCellSize,
                                        kItemCellSize * 0.5F - transformed[i].y * kItemCellSize};
                // The model writes uv in 0..16 of the sprite; the atlas rect
                // turns that into a place on the sheet.
                const f32 u = quad.vertices[i].u / 16.0F;
                const f32 v = quad.vertices[i].v / 16.0F;
                out.uv[i]   = ItemPoint{sprite.u0 + (sprite.u1 - sprite.u0) * u,
                                    sprite.v0 + (sprite.v1 - sprite.v0) * v};
                depth += transformed[i].z;
            }
            out.depth = depth * 0.25F;
            mesh.quads.push_back(out);
        }

        // Back to front. A cube's three visible faces never overlap, but a
        // torch, a stair or a chest are several boxes and do.
        std::ranges::stable_sort(mesh.quads,
                                 [](const ItemQuad& a, const ItemQuad& b) {
                                     return a.depth < b.depth;
                                 });
        mesh.drawable = !mesh.quads.empty();
        if (!mesh.drawable) {
            ++impl_->missing;
        }
        impl_->meshes.emplace(name, std::move(mesh));
    }

    OV_LOG_INFO("items: {} models, {} sprites, {} without geometry", impl_->meshes.size(),
                impl_->sprites.size(), impl_->missing);
}

const ItemMesh* ItemModelCache::mesh(std::string_view name) const noexcept {
    const auto canonical = ResourceLocation::parse(name);
    if (!canonical) {
        return nullptr;
    }
    const auto found = impl_->meshes.find(canonical->full());
    return found == impl_->meshes.end() ? nullptr : &found->second;
}

usize ItemModelCache::resolved_count() const noexcept { return impl_->models.size(); }

usize ItemModelCache::missing_count() const noexcept { return impl_->missing; }

}  // namespace ov::render
