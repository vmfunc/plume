// the conversation sidebar: a closable pane listing threads with an active
// marker, source badge and relative time. keyboard-navigable when focused,
// click-to-switch always. out-of-line members of app::impl.
#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

namespace {

std::string rel_time(std::int64_t created, std::int64_t now) {
	if (created <= 0) return "";
	const std::int64_t s = (now - created) / 1000;
	if (s < 60) return "now";
	if (s < 3600) return std::to_string(s / 60) + "m";
	if (s < 86400) return std::to_string(s / 3600) + "h";
	return std::to_string(s / 86400) + "d";
}

}  // namespace

void app::impl::toggle_sidebar() {
	switch (sb) {
		case sidebar_mode::hidden:  // open, grab the keyboard
			sb = sidebar_mode::focused;
			refresh_sidebar();
			sb_cursor = 0;
			sb_filtering = false;
			sb_renaming = false;
			sb_confirm_delete = -1;
			break;
		case sidebar_mode::focused:  // stay visible, hand the keyboard back
			sb = sidebar_mode::open;
			break;
		case sidebar_mode::open:  // close
			sb = sidebar_mode::hidden;
			break;
	}
}

void app::impl::refresh_sidebar() {
	if (db)
		if (auto l = db->conversations()) sb_convos = *l;
}

std::vector<const conversation*> app::impl::sidebar_list() const {
	std::vector<const conversation*> out;
	for (const auto& c : sb_convos) {
		const std::string t = c.title.empty() ? "(untitled)" : c.title;
		if (sb_filter.empty() || fuzzy(sb_filter, t)) out.push_back(&c);
	}
	return out;
}

Element app::impl::sidebar_view() {
	const bool focused = sb == sidebar_mode::focused;
	const auto list = sidebar_list();
	const std::int64_t now = now_ms();

	Elements rows;
	rows.push_back(hot(
	    hbox({text(" chats ") | bold | color(col(th.p.base)) | bgcolor(col(th.p.iris)), filler(),
	          text(std::to_string(list.size()) + " ") | color(col(th.p.muted)) | dim}),
	    hit_kind::sidebar_toggle));
	rows.push_back(separator() | color(col(th.p.hl_med)));
	if (sb_filtering || !sb_filter.empty())
		rows.push_back(
		    hbox({text(" / ") | color(col(th.p.iris)), text(sb_filter) | color(col(th.p.text)),
		          focused && sb_filtering ? text("▏") | color(col(th.p.iris)) : text("")}));
	if (list.empty()) rows.push_back(text("  no conversations") | color(col(th.p.muted)) | dim);

	int i = 0;
	for (const auto* c : list) {
		const bool on = focused && i == sb_cursor;
		const bool active = c->id == convo;
		Element row;
		if (sb_confirm_delete == i) {
			row = hbox({text("  delete? ") | color(col(th.p.love)),
			            text("y / n") | color(col(th.p.gold))});
		} else if (sb_renaming && on) {
			row = hbox({text(" ▸ ") | color(col(th.p.iris)),
			            text(sb_rename_buf) | color(col(th.p.gold)),
			            text("▏") | color(col(th.p.iris))});
		} else {
			std::string title = c->title.empty() ? "(untitled)" : c->title;
			if (title.size() > 24) title = title.substr(0, 23) + "…";
			Elements cells = {
			    text(active ? "▏" : " ") | color(col(active ? th.p.iris : th.p.muted)),
			    text(title) | color(col(active || on ? th.p.text : th.p.subtle)) | flex};
			if (c->source == "claude-export" || c->source == "claude-live")
				cells.push_back(text("↪ ") | color(col(th.p.foam)) | dim);
			cells.push_back(text(rel_time(c->created_at, now) + " ") | color(col(th.p.muted)) |
			                dim);
			row = hbox(std::move(cells));
		}
		if (on) row = row | bgcolor(col(th.p.hl_low));
		rows.push_back(hot(std::move(row), hit_kind::sidebar_row, i));
		++i;
	}

	Elements out = {vbox(std::move(rows)) | yframe | flex};
	if (focused)
		out.push_back(text(" n new · r rename · d del · / find") | color(col(th.p.subtle)) | dim);
	return vbox(std::move(out)) | flex;
}

bool app::impl::handle_sidebar(const Event& e) {
	const auto list = sidebar_list();
	const int max = static_cast<int>(list.size()) - 1;

	if (sb_renaming) {
		if (e == Event::Return) {
			if (db && sb_cursor >= 0 && sb_cursor <= max && !sb_rename_buf.empty()) {
				conversation c = *list[static_cast<std::size_t>(sb_cursor)];
				c.title = sb_rename_buf;
				static_cast<void>(db->put_conversation(c));
				if (c.id == convo) convo_title = c.title;
				refresh_sidebar();
			}
			sb_renaming = false;
		} else if (e == Event::Escape) {
			sb_renaming = false;
		} else if (e == Event::Backspace) {
			if (!sb_rename_buf.empty()) sb_rename_buf.pop_back();
		} else if (e.is_character()) {
			sb_rename_buf += e.character();
		}
		return true;
	}

	if (sb_confirm_delete >= 0) {
		if (e == Event::Character("y") && db && sb_confirm_delete <= max) {
			const convo_id gone = list[static_cast<std::size_t>(sb_confirm_delete)]->id;
			static_cast<void>(db->delete_conversation(gone));
			refresh_sidebar();
			if (gone == convo) {  // dropped the live thread; land on another
				if (!sb_convos.empty())
					switch_convo(sb_convos.front().id);
				else
					new_conversation();
			}
			sb_cursor =
			    std::clamp(sb_cursor, 0, std::max(0, static_cast<int>(sidebar_list().size()) - 1));
		}
		sb_confirm_delete = -1;
		return true;
	}

	if (sb_filtering) {
		if (e == Event::Return || e == Event::Escape) {
			sb_filtering = false;
			if (e == Event::Escape) sb_filter.clear();
			sb_cursor = 0;
		} else if (e == Event::Backspace) {
			if (!sb_filter.empty()) sb_filter.pop_back();
		} else if (e.is_character()) {
			sb_filter += e.character();
			sb_cursor = 0;
		}
		return true;
	}

	if (e == Event::Escape) return sb = sidebar_mode::open, true;  // unfocus, stay open
	if (e == Event::Character("j") || e == Event::ArrowDown)
		return sb_cursor = std::min(sb_cursor + 1, std::max(0, max)), true;
	if (e == Event::Character("k") || e == Event::ArrowUp)
		return sb_cursor = std::max(sb_cursor - 1, 0), true;
	if (e == Event::Return || e == Event::Character("o")) {
		if (sb_cursor >= 0 && sb_cursor <= max) {
			switch_convo(list[static_cast<std::size_t>(sb_cursor)]->id);
			sb = sidebar_mode::open;
		}
		return true;
	}
	if (e == Event::Character("n")) {
		new_conversation();
		refresh_sidebar();
		sb_cursor = 0;
		sb = sidebar_mode::open;
		return true;
	}
	if (e == Event::Character("r") && max >= 0) {
		sb_renaming = true;
		sb_rename_buf = list[static_cast<std::size_t>(sb_cursor)]->title;
		return true;
	}
	if (e == Event::Character("d") && max >= 0) return sb_confirm_delete = sb_cursor, true;
	if (e == Event::Character("/")) return sb_filtering = true, sb_filter.clear(), true;
	return true;  // focused sidebar swallows the rest
}

}  // namespace plume
