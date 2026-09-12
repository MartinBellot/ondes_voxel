// The boss bars at the top of the screen.
//
// Every Boss Bar packet (0x0B), applied in arrival order: add, remove, health,
// title, style, flags. Measured on the real 1.20.1 client
// (docs/provenance/hud.md § boss) with the seven colours and the five styles
// on screen at once: `gui/bars.png`, 182 wide, the first bar at y 12 and one
// every 19 pixels, its title centred 9 pixels above it, in the order the bars
// were added, and no bar at or below a third of the screen's height. A change
// of health slides over 100 ms of wall time (the running client's
// LerpingBossEvent.LERP_MILLISECONDS).
#pragma once

#include "ov/base/types.hpp"
#include "ov/client/chat.hpp"
#include "ov/client/gui.hpp"
#include "ov/netclient/client.hpp"

#include <string>
#include <vector>

namespace ov::render {
class Language;
}

namespace ov::client {

class BossBarView {
public:
    struct Bar {
        u64     most{0};
        u64     least{0};
        RunLine title;
        /// The lerp: where it was, where it goes, since when (ms).
        f32 from{0.0F};
        f32 target{0.0F};
        i64 set_ms{0};
        i32 color{0};
        i32 division{0};
        u8  flags{0};

        [[nodiscard]] f32 progress(i64 now_ms) const noexcept;
    };

    void apply(const netclient::ClientEvents::BossBarChange& change, i64 now_ms,
               const render::Language& language);
    void clear() { bars_.clear(); }

    [[nodiscard]] const std::vector<Bar>& bars() const noexcept { return bars_; }

    /// Draw every bar that fits. `bars` is `gui/bars`.
    void draw(Gui& gui, GuiTexture bars, i64 now_ms) const;

private:
    std::vector<Bar> bars_;
};

}  // namespace ov::client
