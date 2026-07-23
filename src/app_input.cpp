// the weave keymap, the compare view, and autoweave. split out of app_impl.hpp
// to keep it under the length limit.
#include <algorithm>
#include <set>

#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

bool app::impl::handle_weave(const Event& e) {
	weave w(*db);
	auto move = [&](int d) {
		weave_cursor =
		    std::max(0, std::min<int>(weave_cursor + d, static_cast<int>(weave_order.size()) - 1));
	};
	if (e == Event::Character("j") || e == Event::ArrowDown) return move(1), true;
	if (e == Event::Character("k") || e == Event::ArrowUp) return move(-1), true;
	const node_id sel = weave_selected();
	if (sel.empty()) return false;
	if (e == Event::Return) {
		static_cast<void>(w.adopt(convo, sel));
		reload_transcript();
		in_weave = false;
		return true;
	}
	if (e == Event::Character("p")) return static_cast<void>(w.prune(sel)), true;
	if (e == Event::Character("P")) return static_cast<void>(w.restore(sel)), true;
	if (e == Event::Character("s")) {
		spawn_siblings(sel, 3);  // three branches, streamed in parallel
		return true;
	}
	if (e == Event::Character("r")) {
		spawn_siblings(sel, 1);  // regenerate: one fresh alternative
		return true;
	}
	if (e == Event::Character("t")) {
		show_think = !show_think;
		weave_note = show_think ? "thinking shown" : "thinking hidden";
		return true;
	}
	if (e == Event::Character("c")) {
		// two-step: first c marks a leaf, second c opens the side-by-side diff.
		if (cmp_a.empty()) {
			cmp_a = sel;
			weave_note = "compare: pick a second node, then c";
		} else {
			cmp_b = sel;
			comparing = true;
			weave_note.clear();
		}
		return true;
	}
	if (e == Event::Character("y")) {
		if (auto n = db->node_of(sel)) {
			osc52_copy(last_code_block(node_text(*n)));
			weave_note = "yanked to clipboard";
		}
		return true;
	}
	if (e == Event::Character("e")) {
		// pull a turn back into the composer; the next send forks a sibling of it.
		if (auto n = db->node_of(sel)) {
			comp.set_text(node_text(*n));
			refork_parent = n->parent ? std::optional<node_id>(*n->parent) : std::nullopt;
			in_weave = false;
			weave_note.clear();
		}
		return true;
	}
	if (e == Event::Character("m")) {
		static_cast<void>(w.set_bookmark(sel, !w.bookmarked(sel).value_or(false)));
		return true;
	}
	if (e == Event::Character("g")) {
		// two-step: first g marks a source subtree, second g grafts it here.
		if (graft_source.empty()) {
			graft_source = sel;
			weave_note = "graft: pick a new parent, then g";
		} else {
			auto r = w.graft(graft_source, sel);
			weave_note = r ? "grafted" : ("graft refused: " + r.error().detail);
			graft_source = {};
		}
		return true;
	}
	if (e == Event::Character("x")) {
		if (auto dot = w.to_dot(convo)) {
			const std::string path = cfg.state_dir + "/" + convo.str() + ".dot";
			std::error_code ec;
			std::filesystem::create_directories(cfg.state_dir, ec);
			std::ofstream(path) << *dot;
			weave_note = "wrote " + path;
		}
		return true;
	}
	return false;
}

namespace {

std::vector<std::string> split_lines(const std::string& s) {
	std::vector<std::string> out;
	std::size_t start = 0;
	while (start <= s.size()) {
		const std::size_t nl = s.find('\n', start);
		out.push_back(s.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
	return out;
}

}  // namespace

Element app::impl::compare_view() {
	auto na = db->node_of(cmp_a);
	auto nb = db->node_of(cmp_b);
	const std::string ta = na ? node_text(*na) : "";
	const std::string tb = nb ? node_text(*nb) : "";
	const auto la = split_lines(ta);
	const auto lb = split_lines(tb);
	// a set-based line diff: a line only in one side is tinted (removed love / added
	// foam); shared lines stay muted. cheap, order-insensitive, readable.
	const std::set<std::string> set_a(la.begin(), la.end());
	const std::set<std::string> set_b(lb.begin(), lb.end());

	auto column = [&](const std::vector<std::string>& lines, const std::set<std::string>& other,
	                  rgb only_tint, const std::string& head, rgb head_tint) {
		Elements rows = {text(head) | bold | color(col(head_tint)),
		                 separator() | color(col(th.p.hl_med))};
		for (const auto& ln : lines) {
			const bool shared = other.contains(ln);
			rows.push_back(paragraph(ln.empty() ? " " : ln) |
			               color(col(shared ? th.p.subtle : only_tint)));
		}
		return vbox(std::move(rows)) | yframe | flex | borderRounded | color(col(head_tint));
	};

	Element cols =
	    hbox({column(la, set_b, th.p.love, "A  " + cmp_a.str().substr(0, 12), th.p.rose),
	          column(lb, set_a, th.p.foam, "B  " + cmp_b.str().substr(0, 12), th.p.iris)});
	return vbox(
	           {hbox({ui::gradient_text("compare", {th.p.love, th.p.iris, th.p.foam}) | bold,
	                  text("  lines unique to a side are tinted") | color(col(th.p.subtle)) | dim}),
	            separator() | color(col(th.p.hl_med)), cols | flex,
	            text(" esc / q closes · love = only in A · foam = only in B") |
	                color(col(th.p.muted)) | dim}) |
	       flex;
}

void app::impl::maybe_autoweave(const node_id& assistant_leaf) {
	if (!autoweave || spawning || streaming) return;
	if (cost_of(last_usage) > autoweave_cap) {  // hard spend cap
		autoweave = false;
		toast = "autoweave stopped: spend cap reached";
		return;
	}
	spawn_siblings(assistant_leaf, autoweave_fan);
}

}  // namespace plume
