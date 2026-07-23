// phase-8 extras: the right-click context menu (a floating action list at the
// pointer) and the /inspect nerd-stats overlay. out-of-line members of app::impl.
#include <array>
#include <utility>

#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

Element app::impl::ctx_menu_view() {
	static constexpr std::array<std::pair<char, const char*>, 7> kItems = {{{'y', "copy"},
	                                                                        {'c', "copy code"},
	                                                                        {'e', "edit as new"},
	                                                                        {'r', "regenerate"},
	                                                                        {'b', "branch three"},
	                                                                        {'q', "quote"},
	                                                                        {'x', "prune"}}};
	Elements items;
	for (const auto& [a, label] : kItems)
		items.push_back(hot(text(" " + std::string(label) + " ") | color(col(th.p.text)),
		                    hit_kind::msg_action, static_cast<int>(a)));
	Element menu = vbox(std::move(items)) | borderRounded | color(col(th.p.hl_high)) |
	               bgcolor(col(th.p.surface));
	// nudge left/up if the click was near the right/bottom edge so it stays on screen.
	const int x = std::min(ctx_x, std::max(0, last_width - 16));
	return vbox({text("") | size(HEIGHT, EQUAL, ctx_y),
	             hbox({text("") | size(WIDTH, EQUAL, x), menu, filler()}), filler()});
}

Element app::impl::inspect_view() {
	const bool have = transcript_sel >= 0 && transcript_sel < static_cast<int>(transcript.size());
	const node& n = have ? transcript[static_cast<std::size_t>(transcript_sel)]
	                     : (transcript.empty() ? node{} : transcript.back());
	auto row = [&](const std::string& k, const std::string& v) {
		return hbox({text("  " + k) | color(col(th.p.muted)) | size(WIDTH, EQUAL, 12),
		             text(v) | color(col(th.p.foam))});
	};
	std::string state = n.state == node_state::error       ? "error"
	                    : n.state == node_state::streaming ? "streaming"
	                                                       : "complete";
	Elements rows = {row("role", std::string(to_string(n.role))),
	                 row("model", n.model.empty() ? "(default)" : n.model),
	                 row("tokens", "in " + std::to_string(n.tokens_in) + "  out " +
	                                   std::to_string(n.tokens_out)),
	                 row("state", state),
	                 row("id", n.id.str().substr(0, 20)),
	                 text(""),
	                 text("  raw content") | color(col(th.p.gold)) | dim,
	                 paragraph(n.content_json.substr(0, 1200)) | color(col(th.p.subtle))};
	return ui::overlay(th, "inspect", vbox(std::move(rows)) | yframe | size(HEIGHT, LESS_THAN, 24));
}

Element app::impl::picker_view() {
	std::vector<const conversation*> list;
	for (const auto& c : picker_convos) {
		const std::string t = c.title.empty() ? "(untitled)" : c.title;
		if (ov_filter.empty() || fuzzy(ov_filter, t)) list.push_back(&c);
	}
	const std::int64_t now = now_ms();
	auto group = [&](std::int64_t created) -> const char* {
		const std::int64_t d = created <= 0 ? 999 : (now - created) / 86400000;
		return d < 1 ? "today" : d < 2 ? "yesterday" : d < 7 ? "this week" : "earlier";
	};

	Elements rows = {
	    hbox({text("› ") | color(col(th.p.iris)) | bold, text(ov_filter) | color(col(th.p.text)),
	          text("▏") | color(col(th.p.iris))}),
	    text("")};
	if (list.empty()) rows.push_back(text("  no conversations") | color(col(th.p.muted)) | dim);
	std::string cur;
	for (int i = 0; i < static_cast<int>(list.size()); ++i) {
		if (const std::string g = group(list[static_cast<std::size_t>(i)]->created_at); g != cur) {
			rows.push_back(text("  " + g) | color(col(th.p.gold)) | dim);
			cur = g;
		}
		const bool on = i == ov_sel;
		std::string title = list[static_cast<std::size_t>(i)]->title;
		if (title.empty()) title = "(untitled)";
		Element row = hbox({text(on ? "  ▸ " : "    ") | color(col(on ? th.p.iris : th.p.muted)),
		                    text(title) | color(col(on ? th.p.text : th.p.subtle)) | flex});
		if (list[static_cast<std::size_t>(i)]->source != "local")
			row = hbox({row, text("↪ ") | color(col(th.p.foam)) | dim});
		if (on) row = row | bgcolor(col(th.p.hl_low));
		rows.push_back(hot(std::move(row), hit_kind::overlay_row, i));
	}
	Element left = vbox(std::move(rows)) | yframe | flex;

	Element right = text("");  // preview of the highlighted conversation
	if (db && ov_sel >= 0 && ov_sel < static_cast<int>(list.size())) {
		const conversation* c = list[static_cast<std::size_t>(ov_sel)];
		int turns = 0;
		std::string model;
		if (auto ns = db->nodes_of(c->id)) {
			turns = static_cast<int>(ns->size());
			for (auto it = ns->rbegin(); it != ns->rend(); ++it)
				if (it->role == role::assistant && !it->model.empty()) {
					model = it->model;
					break;
				}
		}
		std::string tags;
		if (auto tg = db->tags_of(c->id))
			for (const auto& t : *tg) tags += (tags.empty() ? "" : ", ") + t;
		Elements pv = {
		    text(c->title.empty() ? "(untitled)" : c->title) | bold | color(col(th.p.text)),
		    text(""), text("turns  " + std::to_string(turns)) | color(col(th.p.foam)),
		    text("model  " + (model.empty() ? "-" : model)) | color(col(th.p.foam)),
		    text("source " + c->source) | color(col(th.p.subtle))};
		if (!tags.empty()) pv.push_back(text("tags   " + tags) | color(col(th.p.rose)));
		right = vbox(std::move(pv));
	}
	return ui::overlay(
	    th, "conversations",
	    hbox({left | size(WIDTH, EQUAL, 34), separator() | color(col(th.p.hl_med)), right | flex}) |
	        size(HEIGHT, LESS_THAN, 22));
}

}  // namespace plume
