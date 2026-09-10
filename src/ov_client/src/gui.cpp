#define OV_LOG_CATEGORY "client"

#include "ov/client/gui.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ov::client {

namespace {

constexpr u32 kVerticesPerQuad = 6;

/// sRGB to linear, the exact transfer function rather than a 2.2 power.
///
/// The swapchain is sRGB and the GPU re-encodes on write, so a tint has to
/// arrive linear for the pixel that comes out to be the number vanilla names.
/// A 2.2 approximation misses by up to three levels in the dark end, which on
/// the black background of an inventory is visible as a slightly wrong grey.
[[nodiscard]] f32 srgb_to_linear(f32 value) noexcept {
    return value <= 0.04045F ? value / 12.92F
                             : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] std::array<u8, 4> unpack_colour(u32 argb) noexcept {
    const auto channel = [](u32 byte) {
        const f32 linear = srgb_to_linear(static_cast<f32>(byte) / 255.0F);
        return static_cast<u8>(std::lround(std::clamp(linear, 0.0F, 1.0F) * 255.0F));
    };
    return std::array<u8, 4>{channel((argb >> 16) & 0xFFU), channel((argb >> 8) & 0xFFU),
                             channel(argb & 0xFFU),
                             static_cast<u8>((argb >> 24) & 0xFFU)};
}

/// The section sign in UTF-8, spelled out for the same reason it is in font.cpp.
constexpr char kSectionFirst  = static_cast<char>(0xC2);
constexpr char kSectionSecond = static_cast<char>(0xA7);

/// Vanilla's shadow: every colour channel divided by four, alpha kept.
[[nodiscard]] u32 shadow_colour(u32 argb) noexcept {
    const u32 alpha = argb & 0xFF000000U;
    const u32 rgb   = ((argb & 0x00FCFCFCU) >> 2);
    return alpha | rgb;
}

}  // namespace

u32 auto_gui_scale(u32 width, u32 height, u32 maximum) noexcept {
    u32 scale = 1;
    while (scale < 20) {
        const u32 next = scale + 1;
        if (maximum != 0 && next > maximum) {
            break;
        }
        if (width / next < 320 || height / next < 240) {
            break;
        }
        scale = next;
    }
    return scale;
}

Gui::~Gui() {
    if (device_ == nullptr) {
        return;
    }
    for (auto buffer : vertices_) {
        device_->destroy(buffer);
    }
    for (const auto& texture : textures_) {
        if (texture.owned) {
            device_->destroy(texture.image);
        }
    }
    if (sampler_.valid()) {
        device_->destroy(sampler_);
    }
    if (pipeline_.valid()) {
        device_->destroy(pipeline_);
    }
}

std::expected<std::unique_ptr<Gui>, rhi::RhiError> Gui::create(rhi::Device& device,
                                                               rhi::Format  colour_format) {
    std::unique_ptr<Gui> self(new Gui);
    self->device_ = &device;

    rhi::VertexBinding binding;
    binding.stride = sizeof(Vertex);
    binding.attributes.push_back(rhi::VertexAttribute{0, rhi::Format::Rg32Float, 0});
    binding.attributes.push_back(rhi::VertexAttribute{1, rhi::Format::Rg32Float, 8});
    binding.attributes.push_back(rhi::VertexAttribute{2, rhi::Format::Rgba8Unorm, 16});

    rhi::GraphicsPipelineDesc pipeline;
    pipeline.vertex_shader                = "gui.vert.spv";
    pipeline.fragment_shader              = "gui.frag.spv";
    pipeline.vertex_bindings              = {binding};
    pipeline.layout.sampled_image_count   = 1;
    pipeline.layout.push_constant_size    = sizeof(f32) * 4;
    pipeline.colour_format                = colour_format;
    // The interface is drawn in its own pass, with no depth attachment at all:
    // it is on top of everything by definition, and a depth test against the
    // terrain would put the hotbar inside a wall.
    pipeline.depth_test  = false;
    pipeline.depth_write = false;
    pipeline.cull_mode   = rhi::CullMode::None;
    pipeline.blend       = rhi::BlendMode::Alpha;
    pipeline.topology    = rhi::PrimitiveTopology::TriangleList;
    pipeline.debug_name  = "gui";

    auto created = device.create_graphics_pipeline(pipeline);
    if (!created) {
        return std::unexpected(created.error());
    }
    self->pipeline_ = *created;

    // Nearest in both directions. The GUI is pixel art at an integer scale, and
    // a linear filter turns a one-pixel border into a two-pixel smear.
    auto sampler = device.create_sampler(rhi::SamplerDesc{rhi::Filter::Nearest,
                                                          rhi::Filter::Nearest,
                                                          rhi::MipFilter::Nearest,
                                                          rhi::AddressMode::ClampToEdge, 1.0F,
                                                          1.0F});
    if (!sampler) {
        return std::unexpected(sampler.error());
    }
    self->sampler_ = *sampler;

    const u32 ring = rhi::Device::frames_in_flight();
    for (u32 i = 0; i < ring; ++i) {
        auto buffer = device.create_buffer(rhi::BufferDesc{
            static_cast<usize>(kMaxQuads) * kVerticesPerQuad * sizeof(Vertex),
            rhi::BufferUsage::Vertex, "gui vertices", true});
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        self->vertices_.push_back(*buffer);
    }

    render::TextureImage white;
    white.width  = 1;
    white.height = 1;
    white.rgba   = {0xFF, 0xFF, 0xFF, 0xFF};
    auto handle  = self->add_texture(white, "gui white");
    if (!handle) {
        return std::unexpected(handle.error());
    }
    self->white_ = *handle;

    self->scratch_.reserve(static_cast<usize>(kMaxQuads) * kVerticesPerQuad);
    self->batches_.reserve(64);
    return self;
}

std::expected<GuiTexture, rhi::RhiError> Gui::add_texture(const render::TextureImage& image,
                                                          std::string_view            name) {
    if (image.empty()) {
        return std::unexpected(rhi::RhiError::InvalidArgument);
    }
    // sRGB, like the block atlas: the pack's PNGs are sRGB and the swapchain
    // expects the shader to have worked in linear.
    auto created = device_->create_image(
        rhi::ImageDesc{image.width, image.height, 1, rhi::Format::Rgba8Srgb, true, false, name});
    if (!created) {
        return std::unexpected(created.error());
    }
    if (!device_->upload_image(*created, image.rgba, image.width, image.height, 0)) {
        device_->destroy(*created);
        return std::unexpected(rhi::RhiError::OutOfMemory);
    }
    textures_.push_back(Texture{*created, image.width, image.height, true});
    return static_cast<GuiTexture>(textures_.size() - 1);
}

GuiTexture Gui::borrow_texture(rhi::ImageHandle image, u32 width, u32 height) {
    textures_.push_back(Texture{image, width, height, false});
    return static_cast<GuiTexture>(textures_.size() - 1);
}

std::expected<void, rhi::RhiError> Gui::set_font(render::Font font) {
    font_ = std::move(font);
    font_pages_.clear();
    for (const auto& page : font_.pages()) {
        auto handle = add_texture(page.image, page.name);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        font_pages_.push_back(*handle);
    }
    return {};
}

const Gui::Texture* Gui::lookup(GuiTexture texture) const noexcept {
    const auto index = static_cast<usize>(texture);
    return index < textures_.size() ? &textures_[index] : nullptr;
}

void Gui::begin(u32 framebuffer_width, u32 framebuffer_height, u32 scale) {
    scale_ = std::max(1U, scale);
    // Vanilla's rule, not a float: the scaled size is an integer, rounded
    // *up* when the framebuffer does not divide. 2560 at scale 3 is 854, which
    // is what the running 1.20.1 client reports, and every centred panel's
    // origin is (854 − w) / 2 in integers. A float 853.33 gave the same origin
    // at that size by luck and a different one at others.
    const u32 gui_width  = framebuffer_width / scale_ + (framebuffer_width % scale_ != 0 ? 1U : 0U);
    const u32 gui_height = framebuffer_height / scale_ + (framebuffer_height % scale_ != 0 ? 1U : 0U);
    width_  = static_cast<f32>(gui_width);
    height_ = static_cast<f32>(gui_height);
    // The projection uses the framebuffer itself: 854 × 3 is 2562, two pixels
    // wider than the 2560 it draws into, and projecting with it would squash
    // the whole interface by that much.
    framebuffer_width_  = static_cast<f32>(framebuffer_width);
    framebuffer_height_ = static_cast<f32>(framebuffer_height);
    scratch_.clear();
    batches_.clear();
    stats_.quads     = 0;
    stats_.draws     = 0;
    stats_.vertices  = 0;
}

void Gui::push_quad(GuiTexture texture, const std::array<GuiPoint, 4>& corners,
                    const std::array<GuiPoint, 4>& uvs, u32 argb) {
    const Texture* entry = lookup(texture);
    if (entry == nullptr) {
        return;
    }
    if (scratch_.size() + kVerticesPerQuad >
        static_cast<usize>(kMaxQuads) * kVerticesPerQuad) {
        OV_LOG_WARN("gui vertex buffer full: {} quads this frame", stats_.quads);
        return;
    }
    if (batches_.empty() || batches_.back().image != entry->image) {
        batches_.push_back(Batch{entry->image, static_cast<u32>(scratch_.size()), 0});
    }

    const auto        colour = unpack_colour(argb);
    const auto        s      = static_cast<f32>(scale_);
    const auto        emit   = [&](usize index) {
        scratch_.push_back(Vertex{corners[index].x * s, corners[index].y * s, uvs[index].x,
                                  uvs[index].y, colour});
    };
    // Two triangles, counter-clockwise in a y-down space. Culling is off, so
    // the winding only has to be consistent, not correct.
    emit(0);
    emit(1);
    emit(2);
    emit(0);
    emit(2);
    emit(3);

    batches_.back().count += kVerticesPerQuad;
    ++stats_.quads;
}

void Gui::quad(GuiTexture texture, f32 x, f32 y, f32 w, f32 h, f32 u0, f32 v0, f32 u1, f32 v1,
                u32 argb) {
    const std::array<GuiPoint, 4> corners{GuiPoint{x, y}, GuiPoint{x, y + h},
                                          GuiPoint{x + w, y + h}, GuiPoint{x + w, y}};
    const std::array<GuiPoint, 4> uvs{GuiPoint{u0, v0}, GuiPoint{u0, v1}, GuiPoint{u1, v1},
                                      GuiPoint{u1, v0}};
    push_quad(texture, corners, uvs, argb);
}

void Gui::quad_corners(GuiTexture texture, const std::array<GuiPoint, 4>& corners,
                       const std::array<GuiPoint, 4>& uvs, u32 argb) {
    push_quad(texture, corners, uvs, argb);
}

void Gui::blit(GuiTexture texture, f32 x, f32 y, f32 w, f32 h, f32 sx, f32 sy, f32 sw, f32 sh,
               f32 sheet_width, f32 sheet_height, u32 argb) {
    quad(texture, x, y, w, h, sx / sheet_width, sy / sheet_height, (sx + sw) / sheet_width,
         (sy + sh) / sheet_height, argb);
}

void Gui::fill(f32 x, f32 y, f32 w, f32 h, u32 argb) {
    quad(white_, x, y, w, h, 0.0F, 0.0F, 1.0F, 1.0F, argb);
}

void Gui::gradient(f32 x, f32 y, f32 w, f32 h, u32 top_argb, u32 bottom_argb) {
    const Texture* entry = lookup(white_);
    if (entry == nullptr) {
        return;
    }
    if (scratch_.size() + kVerticesPerQuad >
        static_cast<usize>(kMaxQuads) * kVerticesPerQuad) {
        return;
    }
    if (batches_.empty() || batches_.back().image != entry->image) {
        batches_.push_back(Batch{entry->image, static_cast<u32>(scratch_.size()), 0});
    }
    const auto top    = unpack_colour(top_argb);
    const auto bottom = unpack_colour(bottom_argb);
    const auto s      = static_cast<f32>(scale_);
    const auto emit   = [&](f32 px, f32 py, const std::array<u8, 4>& colour) {
        scratch_.push_back(Vertex{px * s, py * s, 0.0F, 0.0F, colour});
    };
    emit(x, y, top);
    emit(x, y + h, bottom);
    emit(x + w, y + h, bottom);
    emit(x, y, top);
    emit(x + w, y + h, bottom);
    emit(x + w, y, top);
    batches_.back().count += kVerticesPerQuad;
    ++stats_.quads;
}

f32 Gui::text(f32 x, f32 y, std::string_view utf8, u32 argb, bool shadow) {
    if (font_pages_.empty()) {
        return x;
    }
    // The shadow is a whole second pass over the string, drawn first. Drawing
    // glyph and shadow together would put a glyph under the next glyph's
    // shadow, which is exactly the artefact vanilla avoids by doing the same.
    // `k` is the text size (1 everywhere but titles, see text_scaled): every
    // offset below is in the font's own pixels, times k.
    const f32 k = text_size_;
    if (shadow) {
        (void)text(x + render::Font::kShadowOffset * k, y + render::Font::kShadowOffset * k, utf8,
                   shadow_colour(argb), false);
    }

    render::TextStyle style;
    const u32         base_alpha = argb & 0xFF000000U;
    f32               pen        = x;
    usize             offset     = 0;
    while (offset < utf8.size()) {
        if (utf8[offset] == kSectionFirst && offset + 2 < utf8.size() &&
            utf8[offset + 1] == kSectionSecond) {
            style.apply(utf8[offset + 2]);
            offset += 3;
            continue;
        }
        const char32_t       codepoint = render::next_codepoint(utf8, offset);
        const render::Glyph* glyph     = font_.glyph(codepoint);
        if (glyph == nullptr) {
            continue;
        }
        const u32 colour = style.has_colour ? (base_alpha | style.colour) : argb;

        if (glyph->width > 0.0F) {
            // The baseline is the anchor: a glyph is drawn `ascent` pixels
            // above it, which is what puts a `p` below the line and an `A` on
            // it.
            const f32 top  = y + (render::Font::kBaseline - glyph->ascent) * k;
            const f32 left = pen;
            // Italic is vanilla's shear: the top edge moves one pixel right.
            const f32 shear = style.italic ? k : 0.0F;
            const f32 gw    = glyph->width * k;
            const f32 gh    = glyph->height * k;

            const auto draw_at = [&](f32 dx) {
                const std::array<GuiPoint, 4> corners{
                    GuiPoint{left + dx + shear, top},
                    GuiPoint{left + dx, top + gh},
                    GuiPoint{left + dx + gw, top + gh},
                    GuiPoint{left + dx + gw + shear, top}};
                const std::array<GuiPoint, 4> uvs{GuiPoint{glyph->u0, glyph->v0},
                                                  GuiPoint{glyph->u0, glyph->v1},
                                                  GuiPoint{glyph->u1, glyph->v1},
                                                  GuiPoint{glyph->u1, glyph->v0}};
                push_quad(font_pages_[glyph->page], corners, uvs, colour);
            };
            draw_at(0.0F);
            if (style.bold) {
                // Bold is the same glyph again, one pixel to the right. That is
                // why it costs one pixel of advance and not a second font.
                draw_at(k);
            }
        }

        const f32 advance =
            (glyph->advance + (style.bold ? render::Font::kBoldExtraAdvance : 0.0F)) * k;
        if (style.strikethrough) {
            fill(pen, y + 3.0F * k, advance, k, style.has_colour ? (base_alpha | style.colour)
                                                                 : argb);
        }
        if (style.underline) {
            fill(pen - k, y + 8.0F * k, advance + k,
                 k, style.has_colour ? (base_alpha | style.colour) : argb);
        }
        pen += advance;
    }
    return pen;
}

f32 Gui::text_scaled(f32 x, f32 y, std::string_view utf8, u32 argb, f32 size, bool shadow) {
    const f32 previous = text_size_;
    text_size_         = size;
    const f32 pen      = text(x, y, utf8, argb, shadow);
    text_size_         = previous;
    return pen;
}

f32 Gui::text_centred(f32 x, f32 y, std::string_view utf8, u32 argb, bool shadow) {
    return text(std::round(x - font_.width(utf8) * 0.5F), y, utf8, argb, shadow);
}

f32 Gui::text_right(f32 x, f32 y, std::string_view utf8, u32 argb, bool shadow) {
    return text(x - font_.width(utf8), y, utf8, argb, shadow);
}

void Gui::flush(rhi::CommandList& cmd) {
    if (scratch_.empty()) {
        return;
    }
    const u32 slot   = ring_ % static_cast<u32>(vertices_.size());
    auto*     mapped = static_cast<Vertex*>(device_->map(vertices_[slot]));
    if (mapped == nullptr) {
        return;
    }
    std::memcpy(mapped, scratch_.data(), scratch_.size() * sizeof(Vertex));

    const std::array<f32, 4> push{2.0F / framebuffer_width_, 2.0F / framebuffer_height_, 0.0F,
                                  0.0F};
    cmd.bind_pipeline(pipeline_);
    cmd.push_constants(pipeline_, push.data(), sizeof(push));
    cmd.bind_vertex_buffer(0, vertices_[slot]);

    const std::array<rhi::SamplerHandle, 1> samplers{sampler_};
    for (const Batch& batch : batches_) {
        if (batch.count == 0) {
            continue;
        }
        const std::array<rhi::ImageHandle, 1> images{batch.image};
        cmd.bind_textures(pipeline_, images, samplers);
        cmd.draw(batch.count, 1, batch.first, 0);
        ++stats_.draws;
    }

    stats_.vertices      = static_cast<u32>(scratch_.size());
    stats_.peak_vertices = std::max(stats_.peak_vertices, stats_.vertices);
    ring_                = (ring_ + 1) % static_cast<u32>(vertices_.size());
}

}  // namespace ov::client
