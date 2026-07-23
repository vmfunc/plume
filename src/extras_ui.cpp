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

}  // namespace plume
