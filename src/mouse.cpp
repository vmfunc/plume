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
		else if (sb == sidebar_mode::focused)
			handle_sidebar(up ? Event::ArrowUp : Event::ArrowDown);
		else if (in_weave)
			handle_weave(up ? Event::ArrowUp : Event::ArrowDown);
		else if (!spawning && !spawn_done)
			scroll_transcript(up ? -3 : 3);
		return true;
	}

	// drag the transcript with the left button held to scroll it, touchpad-style.
	if (m.motion == Mouse::Moved && m.button == Mouse::Left && ov == overlay::none && !in_weave) {
		if (drag_y >= 0 && m.y != drag_y) scroll_transcript(drag_y - m.y);
		drag_y = m.y;
		return true;
	}
	if (m.motion == Mouse::Released) {
		drag_y = -1;
		return true;
	}

	// right-click opens the context menu on a message.
	if (m.button == Mouse::Right && m.motion == Mouse::Pressed) {
		if (const hit_region* h = hit_test(m.x, m.y); h && h->kind == hit_kind::message) {
			transcript_sel = h->index;
			follow_tail = false;
			ctx_open = true;
			ctx_x = m.x;
			ctx_y = m.y;
		}
		return true;
	}

	// only a left press acts; motion/hover/release are ignored for now.
	if (m.button != Mouse::Left || m.motion != Mouse::Pressed) return true;

	const hit_region* hit = hit_test(m.x, m.y);
	if (ctx_open) {  // a click resolves the floating context menu
		ctx_open = false;
		if (hit && hit->kind == hit_kind::msg_action) message_action(static_cast<char>(hit->index));
		return true;
	}
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
			if (ov == overlay::none && !slash_matches().empty()) {
				slash_sel = hit->index;  // click a slash suggestion
				accept_slash();
			} else if (ov == overlay::artifacts) {
				art_sel = hit->index;  // click an artifact to view it
				art_view = hit->index;
			} else if (ov == overlay::settings) {
				settings_sel = hit->index;
				handle_settings(Event::Return);  // click a setting to cycle it
			} else {
				ov_sel = hit->index;
				handle_overlay(Event::Return);  // activate the row
			}
			return true;
		case hit_kind::help: ov = overlay::cheatsheet; return true;
		case hit_kind::settings: open_settings(); return true;
		case hit_kind::statusbar_model:
			open_models();  // click the model pill to pick one
			return true;
		case hit_kind::sidebar_row: {
			const auto list = sidebar_list();  // click switches without stealing focus
			if (hit->index >= 0 && hit->index < static_cast<int>(list.size())) {
				switch_convo(list[static_cast<std::size_t>(hit->index)]->id);
				sb_cursor = hit->index;
			}
			return true;
		}
		case hit_kind::sidebar_toggle: toggle_sidebar(); return true;
		case hit_kind::msg_action: message_action(static_cast<char>(hit->index)); return true;
		case hit_kind::jump_latest: scroll_tail(); return true;
		case hit_kind::retry: retry_last(); return true;
		case hit_kind::tab:
			if (hit->index >= 0 && hit->index < static_cast<int>(tabs.size()))
				switch_convo(tabs[static_cast<std::size_t>(hit->index)]);
			return true;
		default: return true;
	}
}

}  // namespace plume
