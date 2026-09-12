#include "ov/client/boss_bar_view.hpp"

#include "ov/render/language.hpp"
#include "ov/render/text_component.hpp"

#include <algorithm>
#include <cmath>

namespace ov::client {
namespace {

constexpr f32 kSheet     = 256.0F;
constexpr f32 kBarWidth  = 182.0F;
constexpr f32 kBarHeight = 5.0F;
/// The notches' rows start here in bars.png; each colour and style takes two
/// rows of five, the empty bar then the full one.
constexpr f32 kOverlayOffset = 80.0F;
constexpr i64 kLerpMs        = 100;
constexpr f32 kFirstBar      = 12.0F;
constexpr f32 kStep          = 19.0F;

}  // namespace

f32 BossBarView::Bar::progress(i64 now_ms) const noexcept {
    const f32 t = std::clamp(static_cast<f32>(now_ms - set_ms) / static_cast<f32>(kLerpMs), 0.0F, 1.0F);
    return from + (target - from) * t;
}

void BossBarView::apply(const netclient::ClientEvents::BossBarChange& change, i64 now_ms,
                        const render::Language& language) {
    const auto it = std::ranges::find_if(bars_, [&](const Bar& bar) {
        return bar.most == change.most && bar.least == change.least;
    });
    const auto title = [&] {
        RunLine runs = render::component_runs(change.title_json, language);
        encode_styles(runs);
        return runs;
    };
    switch (change.action) {
        case 0: {  // add
            Bar bar;
            bar.most     = change.most;
            bar.least    = change.least;
            bar.title    = title();
            bar.from     = change.health;
            bar.target   = change.health;
            bar.set_ms   = now_ms;
            bar.color    = change.color;
            bar.division = change.division;
            bar.flags    = change.flags;
            if (it != bars_.end()) {
                *it = std::move(bar);
            } else {
                bars_.push_back(std::move(bar));
            }
            break;
        }
        case 1:  // remove
            if (it != bars_.end()) {
                bars_.erase(it);
            }
            break;
        case 2:  // health
            if (it != bars_.end()) {
                it->from   = it->progress(now_ms);
                it->target = change.health;
                it->set_ms = now_ms;
            }
            break;
        case 3:  // title
            if (it != bars_.end()) {
                it->title = title();
            }
            break;
        case 4:  // style
            if (it != bars_.end()) {
                it->color    = change.color;
                it->division = change.division;
            }
            break;
        case 5:  // flags
            if (it != bars_.end()) {
                it->flags = change.flags;
            }
            break;
        default: break;
    }
}

void BossBarView::draw(Gui& gui, GuiTexture bars, i64 now_ms) const {
    if (bars_.empty() || bars == GuiTexture::Invalid) {
        return;
    }
    const f32 width = gui.width();
    const f32 left  = std::floor(width / 2.0F) - 91.0F;
    f32       y     = kFirstBar;
    usize     shown = 0;
    for (const Bar& bar : bars_) {
        if (y >= std::floor(gui.height() / 3.0F)) {
            break;
        }
        const f32 colour_v = static_cast<f32>(bar.color) * kBarHeight * 2.0F;
        const f32 notch_v  = kOverlayOffset + static_cast<f32>(bar.division - 1) * kBarHeight * 2.0F;
        gui.blit(bars, left, y, kBarWidth, kBarHeight, 0.0F, colour_v, kBarWidth, kBarHeight, kSheet,
                 kSheet);
        if (bar.division > 0) {
            gui.blit(bars, left, y, kBarWidth, kBarHeight, 0.0F, notch_v, kBarWidth, kBarHeight,
                     kSheet, kSheet);
        }
        const f32 filled = std::floor(bar.progress(now_ms) * (kBarWidth + 1.0F));
        if (filled > 0.0F) {
            gui.blit(bars, left, y, filled, kBarHeight, 0.0F, colour_v + kBarHeight, filled,
                     kBarHeight, kSheet, kSheet);
            if (bar.division > 0) {
                gui.blit(bars, left, y, filled, kBarHeight, 0.0F, notch_v + kBarHeight, filled,
                         kBarHeight, kSheet, kSheet);
            }
        }
        y += kStep;
        ++shown;
    }
    // The titles after every bar: the sheet, then the font, one change.
    y = kFirstBar;
    for (usize i = 0; i < shown; ++i) {
        const RunLine& title = bars_[i].title;
        const f32      w     = runs_width(title, gui.font());
        const f32      x     = std::floor(width / 2.0F) - std::floor(w / 2.0F);
        (void)draw_runs(gui, x, y - 9.0F, title, 255, true);
        y += kStep;
    }
}

}  // namespace ov::client
