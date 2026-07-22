// rendering toolkit: color and motion helpers plus the restyled building blocks
// the chat, weave and wizard share. motion is seasoning — every animated helper
// takes a millisecond clock and a reduce_motion flag, and falls still when it is
// set. src-local.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "plume/color.hpp"
#include "plume/message.hpp"
#include "plume/theme.hpp"

namespace plume::ui {

ftxui::Color col(rgb c);
rgb lerp(rgb a, rgb b, float t);

// a smooth 0..1..0 over period_ms, for a breathing glyph.
float breathe(std::int64_t ms, int period_ms);
// a braille spinner frame chosen by the clock.
std::string spinner(std::int64_t ms);

// the streaming caret; brightness eases between text and accent unless still.
ftxui::Element caret(const theme&, std::int64_t ms, bool reduce_motion);

// a thin gradient meter (pine → gold → love) filled to frac across width cells.
ftxui::Element meter(const theme&, float frac, int width);

// a small chip.
ftxui::Element pill(const theme&, const std::string& label, rgb fg);

// one settled message: a left accent rule, a role label with meta, the body,
// and dim thinking. compact tightens the spacing.
ftxui::Element message_card(const theme&, plume::role, const std::string& body,
                            const std::vector<std::string>& thinking, bool show_think, bool compact,
                            const std::string& model, std::int64_t tokens_out);

// the assistant turn while it is still arriving.
ftxui::Element streaming_card(const theme&, const std::string& text, const std::string& thinking,
                              bool show_think, bool compact, std::int64_t ms, bool reduce_motion);

}  // namespace plume::ui
