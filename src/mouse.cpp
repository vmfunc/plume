// the mouse router: resolve a pointer event against the per-frame hit-test table
// and dispatch. wheel scrolls by current mode; clicks by the region under the
// pointer. out-of-line member of app::impl.
#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

const hit_region* app::impl::hit_test(int x, int y) const {
	// reverse scan: the last region drawn sits on top (dbox / overlay order).
	for (auto it = regions_.rbegin(); it != regions_.rend(); ++it)
		if (it->box.Contain(x, y)) return &*it;
	return nullptr;
}

bool app::impl::handle_mouse(const Mouse& m) {
	// wheel scrolls whatever mode owns the screen, no hit-test needed.
	if (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown) {
		const bool up = m.button == Mouse::WheelUp;
		if (ov != overlay::none)
			handle_overlay(up ? Event::ArrowUp : Event::ArrowDown);
		else if (in_weave)
			handle_weave(up ? Event::ArrowUp : Event::ArrowDown);
		else if (!spawning && !spawn_done)
			scroll_transcript(up ? -3 : 3);
		return true;
	}

	// only a left press acts; motion/hover/release are ignored for now.
	if (m.button != Mouse::Left || m.motion != Mouse::Pressed) return true;

	const hit_region* hit = hit_test(m.x, m.y);
	if (!hit) {
		if (ov != overlay::none) ov = overlay::none;  // click-away closes an overlay
		return true;
	}

	const std::int64_t t = now_ms();
	const bool dbl =
	    hit->kind == last_click_kind && hit->index == last_click_index && (t - last_click_ms) < 350;
	last_click_ms = t;
	last_click_kind = hit->kind;
	last_click_index = hit->index;

	switch (hit->kind) {
		case hit_kind::message:
			transcript_sel = hit->index;  // select the turn; drop the live tail
			follow_tail = false;
			return true;
		case hit_kind::weave_node:
			weave_cursor = hit->index;
			if (dbl) handle_weave(Event::Return);  // double-click adopts
			return true;
		case hit_kind::overlay_row:
			ov_sel = hit->index;
			handle_overlay(Event::Return);  // activate the row
			return true;
		case hit_kind::statusbar_model:
			// the model picker (phase 3) hangs off this region.
			return true;
		default: return true;
	}
}

}  // namespace plume
