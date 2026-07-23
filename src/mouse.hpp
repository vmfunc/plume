// the mouse hit-test registry. the main view is a hand-rolled ftxui Renderer, not
// a Component tree, so there is nothing to route mouse events through by default.
// instead, render code wraps each clickable element with impl::hot(), which binds
// an ftxui Box via reflect() and records it; on a mouse event the router reverse-
// scans the per-frame table (last-drawn, i.e. topmost, wins) to resolve the hit.
#pragma once

#include <cstdint>

#include <ftxui/screen/box.hpp>

namespace plume {

enum class hit_kind : std::uint8_t {
	none,
	message,          // a transcript turn (index = transcript index)
	weave_node,       // a weave tree row (index = weave_order index)
	overlay_row,      // a row in an overlay list (index = visible row)
	statusbar_model,  // the model pill in the status bar
	sidebar_row,      // a conversation row (index = sidebar list index)
	sidebar_toggle,   // the collapse/expand handle
	help,             // the "? keys" hint in the status bar
	settings,         // the "settings" hint in the status bar
};

struct hit_region {
	ftxui::Box box;
	hit_kind kind = hit_kind::none;
	int index = 0;
};

}  // namespace plume
