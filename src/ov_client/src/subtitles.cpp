#include "ov/client/subtitles.hpp"

#include "ov/client/gui.hpp"

#include <algorithm>
#include <cmath>

namespace ov::client {

void SubtitleOverlay::heard(std::string_view key, Vec3d position, bool relative) {
    if (key.empty()) {
        return;
    }
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->key == key) {
            Entry refreshed    = std::move(*it);
            refreshed.position = position;
            refreshed.relative = relative;
            refreshed.time     = now_;
            entries_.erase(it);
            entries_.push_back(std::move(refreshed));
            return;
        }
    }
    entries_.push_back(Entry{std::string{key}, position, relative, now_});
}

void SubtitleOverlay::prune() {
    std::erase_if(entries_, [this](const Entry& entry) { return now_ - entry.time >= display_; });
}

std::vector<SubtitleOverlay::Line> SubtitleOverlay::lines(const audio::Listener& listener) const {
    std::vector<Line> out;
    out.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        const f64 age = now_ - entry.time;
        if (age >= display_) {
            continue;
        }
        Line line;
        line.key       = entry.key;
        line.freshness = static_cast<f32>(1.0 - age / display_);
        if (!entry.relative) {
            const f32 pan = audio::SoundEngine::pan(listener, entry.position);
            line.arrow    = pan > kArrowPan ? 1 : (pan < -kArrowPan ? -1 : 0);
        }
        out.push_back(std::move(line));
    }
    return out;
}

void SubtitleOverlay::draw(Gui& gui, const std::function<std::string(std::string_view)>& translate,
                           const audio::Listener& listener) const {
    const std::vector<Line> shown = lines(listener);
    if (shown.empty()) {
        return;
    }
    std::vector<std::string> texts;
    texts.reserve(shown.size());
    f32 widest = 0.0F;
    for (const Line& line : shown) {
        texts.push_back(translate(line.key));
        widest = std::max(widest, gui.font().width(texts.back()));
    }
    const f32 arrow_width = gui.font().width(">");
    // Ours: a line of 10 GUI pixels, a margin of 1 around the text, the box's
    // bottom 35 pixels above the window's (clear of the hotbar) and 2 in from
    // its right edge.
    constexpr f32 kLine   = 10.0F;
    const f32 box_width   = widest + 2.0F * arrow_width + 8.0F;
    const f32 right       = gui.width() - 2.0F;
    const f32 left        = right - box_width;
    const f32 bottom      = gui.height() - 35.0F;
    const f32 centre      = left + box_width / 2.0F;
    for (usize i = 0; i < shown.size(); ++i) {
        const usize from_bottom = shown.size() - 1 - i;
        const f32   y           = bottom - static_cast<f32>(from_bottom + 1) * kLine;
        gui.fill(left, y, box_width, kLine, 0xCC000000U);
        // White fading to a grey of 75: "becoming less white". The grey is ours.
        const f32 level = 75.0F + 180.0F * shown[i].freshness;
        const u32 grey  = static_cast<u32>(std::lround(level));
        const u32 argb  = 0xFF000000U | (grey << 16U) | (grey << 8U) | grey;
        (void)gui.text_centred(centre, y + 1.0F, texts[i], argb, false);
        if (shown[i].arrow < 0) {
            (void)gui.text(left + 1.0F, y + 1.0F, "<", argb, false);
        } else if (shown[i].arrow > 0) {
            (void)gui.text_right(right - 1.0F, y + 1.0F, ">", argb, false);
        }
    }
}

}  // namespace ov::client
