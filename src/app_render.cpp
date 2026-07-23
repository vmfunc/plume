// the app's rendering methods, split out of app_impl.hpp to keep it under the
// length limit. these are out-of-line members of app::impl.
#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

double app::impl::cost_of(const usage& u) const {
	if (auto it = cfg.prices.find(model_id()); it != cfg.prices.end())
		return (u.input * it->second.input + u.output * it->second.output +
		        u.cache_read * it->second.cache_read + u.cache_creation * it->second.cache_write) /
		       1e6;
	return 0.0;
}

Element app::impl::header() {
	return vbox({
	    hbox({text(" "), ui::gradient_text("plume", {th.p.love, th.p.iris, th.p.foam}) | bold,
	          text("  " + convo_title) | color(col(th.p.subtle)), filler(),
	          text(in_weave ? "weave " : "") | color(col(th.p.foam)) | dim}),
	    separator() | color(col(th.p.hl_med)),
	});
}

Element app::impl::statusbar() {
	usage tot = last_usage;
	std::string cost = std::to_string(cost_of(tot));
	cost = cost.substr(0, cost.find('.') + 5);
	const float frac = context_window > 0 ? static_cast<float>(tot.input) / context_window : 0.f;
	auto dot = [&] { return text(" · ") | color(col(th.p.muted)); };
	Elements line = {
	    hot(ui::pill(th, model_id(), th.p.iris), hit_kind::statusbar_model),  // click to pick
	    text(" "),
	    ui::meter(th, frac, 12),
	    dot(),
	    text("in " + std::to_string(tot.input) + " out " + std::to_string(tot.output)) |
	        color(col(th.p.foam)),
	    dot(),
	    text("$" + cost) | color(col(th.p.gold)),
	    dot(),
	    text(std::to_string(ttft_ms) + "ms") | color(col(th.p.pine)),
	};
	if (plugins)
		for (const auto& s : plugins->statusline()) {
			line.push_back(dot());
			line.push_back(text(s.text) | color(col(th.p.rose)));
		}
	line.push_back(filler());
	if (autoweave) line.push_back(ui::pill(th, "autoweave", th.p.pine));
	if (!pending_attach.empty())
		line.push_back(
		    ui::pill(th, "⧉ " + std::to_string(pending_attach.size()) + " staged", th.p.gold));
	if (!toast.empty()) line.push_back(text(toast + " ") | color(col(th.p.foam)));
	if (streaming || compacting.load())
		line.push_back(text(ui::spinner(now_ms()) + " streaming ") | color(col(th.p.rose)));
	line.push_back(text(in_weave ? " weave " : " chat ") | color(col(th.p.subtle)) |
	               bgcolor(col(th.p.overlay)));
	return hbox(std::move(line)) | bgcolor(col(th.p.surface));
}

Element app::impl::transcript_view() {
	Elements out;
	const bool cmp = compact();
	const bool cursor_on =
	    !follow_tail && transcript_sel >= 0 && transcript_sel < static_cast<int>(transcript.size());
	int focus_at = -1;  // index into `out` to scroll to
	int idx = 0;
	for (const auto& n : transcript) {
		auto blocks = codec::decode_blocks(n.content_json);
		std::string body;
		std::vector<std::string> thinks;
		std::vector<std::string> images;  // on-disk paths of attached images
		if (blocks) {
			body = message{n.role, *blocks}.plain_text();
			for (const auto& b : *blocks) {
				if (const auto* t = std::get_if<thinking_block>(&b)) thinks.push_back(t->thinking);
				if (const auto* im = std::get_if<image_block>(&b); im && !im->path.empty())
					images.push_back(im->path);
				if (const auto* dc = std::get_if<document_block>(&b); dc && !dc->path.empty())
					body = "[pdf: " + dc->path + "]\n" + body;
			}
		} else {
			body = n.content_json;
		}
		Element card =
		    ui::message_card(th, n.role, body, thinks, show_think, cmp, n.model, n.tokens_out);
		if (cursor_on && idx == transcript_sel) {
			card = card | bgcolor(col(th.p.hl_low));  // the selected turn
			focus_at = static_cast<int>(out.size());
		}
		out.push_back(hot(std::move(card), hit_kind::message, idx));  // click selects

		for (const auto& path : images)
			if (const img::bitmap* bm = preview(path))
				out.push_back(hbox({text("  "), ui::image_halfblock(*bm, 44, 14)}));
		++idx;
	}
	if (streaming || !live_text.empty() || !live_think.empty())
		out.push_back(ui::streaming_card(th, live_text, live_think, show_think, cmp, now_ms(),
		                                 reduce_motion()));
	if (out.empty())
		out.push_back(vbox(
		    {filler(), text("a blank page.") | color(col(th.p.subtle)) | center,
		     text("type below to begin · ctrl-w to weave") | color(col(th.p.muted)) | dim | center,
		     filler()}));
	if (!status_error.empty()) out.push_back(text("  " + status_error) | color(col(th.p.love)));
	if (focus_at >= 0)
		out[static_cast<std::size_t>(focus_at)] = out[static_cast<std::size_t>(focus_at)] | focus;
	else
		out.back() = out.back() | focus;  // follow the tail
	return vbox(std::move(out)) | yframe | vscroll_indicator | flex;
}

Element app::impl::weave_view() {
	weave w(*db);
	auto v = w.view(convo, false);
	Elements rows;
	weave_order.clear();
	if (v) {
		for (const auto& id : v->preorder) {
			const auto& tn = v->nodes.at(id);
			weave_order.push_back(id);
			const int i = static_cast<int>(weave_order.size()) - 1;
			const std::string label = codec::read_str(tn.data.params_json, "label").value_or("");
			const bool mark = codec::read_bool(tn.data.params_json, "bookmark").value_or(false);
			const bool is_source = !graft_source.empty() && id == graft_source;
			const rgb c = tn.data.role == role::user ? th.p.pine : th.p.iris;
			std::string indent;
			for (int d = 0; d < tn.depth; ++d) indent += "  ";
			const std::string glyph = tn.data.role == role::user ? "◇ " : "◆ ";
			Elements cells = {text(indent), text(mark ? "★ " : "") | color(col(th.p.gold)),
			                  text(glyph) | color(col(c)),
			                  text(std::string(to_string(tn.data.role))) | color(col(th.p.text))};
			if (!label.empty())
				cells.push_back(text("  " + label) | color(col(th.p.foam)) | italic);
			if (is_source) cells.push_back(text("  graft source") | color(col(th.p.gold)) | dim);
			Element e = hbox(std::move(cells));
			if (i == weave_cursor)
				e = hbox({text("▏") | color(col(th.p.iris)), e}) | bgcolor(col(th.p.hl_low)) |
				    focus;
			else
				e = hbox({text(" "), e});
			rows.push_back(hot(std::move(e), hit_kind::weave_node, i));  // click moves cursor
		}
	}
	if (rows.empty()) rows.push_back(text("  the tree is empty") | color(col(th.p.muted)));
	Element tree = vbox(std::move(rows)) | yframe | flex;
	Element help = text(
	                   "hjkl move · enter adopt · s spawn · r regen · c compare · p/P prune · "
	                   "m bookmark · g graft · x dot · ctrl-w back") |
	               color(col(th.p.subtle)) | dim;
	Elements bottom = {help};
	if (!weave_note.empty()) bottom.push_back(text("  " + weave_note) | color(col(th.p.gold)));
	return vbox({hbox({text(" the loom ") | bold | color(col(th.p.base)) | bgcolor(col(th.p.iris)),
	                   filler()}),
	             separator() | color(col(th.p.hl_med)), tree, separator() | color(col(th.p.hl_med)),
	             vbox(std::move(bottom))}) |
	       flex;
}

Element app::impl::spawn_view() {
	Elements cols;
	for (std::size_t i = 0; i < siblings.size(); ++i) {
		sibling_stream* s = siblings[i].get();
		const rgb tint = std::array{th.p.iris, th.p.foam, th.p.rose, th.p.gold, th.p.pine}[i % 5];
		Element head =
		    hbox({text("branch " + std::to_string(i + 1) + " ") | bold | color(col(tint)),
		          s->done ? text("done") | color(col(th.p.foam))
		                  : text(ui::spinner(now_ms())) | color(col(th.p.gold))});
		cols.push_back(vbox({head, separator() | color(col(th.p.hl_med)),
		                     paragraph(s->text) | color(col(th.p.text)) | flex}) |
		               borderRounded | color(col(tint)) | flex);
	}
	const std::string note =
	    spawn_done ? "done · any key returns to the loom to adopt a branch" : "esc to stop";
	return vbox({hbox({ui::gradient_text("the loom", {th.p.love, th.p.iris, th.p.foam}) | bold,
	                   text("  " + std::to_string(siblings.size()) + " branches, streaming") |
	                       color(col(th.p.subtle))}),
	             separator() | color(col(th.p.hl_med)), hbox(std::move(cols)) | flex,
	             separator() | color(col(th.p.hl_med)),
	             text(note) | color(col(th.p.subtle)) | dim}) |
	       flex;
}

Element app::impl::overlay_view() {
	if (ov == overlay::models) return models_view();
	if (ov == overlay::tool_approve && !tool_queue.empty()) {
		const auto& pt = tool_queue.front();
		const std::string extra =
		    tool_queue.size() > 1 ? "  (+" + std::to_string(tool_queue.size() - 1) + " more)" : "";
		return ui::overlay(
		    th, "tool call" + extra,
		    vbox({hbox({text("server  ") | color(col(th.p.muted)),
		                text(pt.server.empty() ? "(unknown)" : pt.server) | color(col(th.p.foam))}),
		          hbox({text("tool    ") | color(col(th.p.muted)),
		                text(pt.name) | color(col(th.p.iris)) | bold}),
		          separator() | color(col(th.p.hl_med)),
		          text("arguments") | color(col(th.p.gold)) | dim, tool_args_table(pt.args_json),
		          text(""),
		          text("a approve · d deny · A always allow · esc deny all") |
		              color(col(th.p.subtle)) | dim}));
	}
	if (ov == overlay::cheatsheet) {
		auto row = [&](const std::string& k, const std::string& d) {
			return hbox({text(k) | color(col(th.p.foam)) | size(WIDTH, EQUAL, 14),
			             text(d) | color(col(th.p.subtle))});
		};
		return ui::overlay(th, "keys",
		                   vbox({row("ctrl-k", "command palette"),
		                         row("ctrl-b", "conversation sidebar"),
		                         row("ctrl-l", "model picker"),
		                         row("ctrl-p", "conversation picker"),
		                         row("ctrl-f", "search everything"),
		                         row("ctrl-w", "open the loom"),
		                         row("ctrl-e", "composer to $EDITOR"),
		                         row("j/k · G", "scroll history · jump to live"),
		                         row("wheel", "scroll · click to select"),
		                         row("/", "slash command (insert)"),
		                         row("? ", "this cheatsheet (normal)"),
		                         row("esc", "stop / close"),
		                         text(""),
		                         text("weave") | color(col(th.p.gold)),
		                         row("hjkl", "move"),
		                         row("enter", "adopt branch"),
		                         row("s", "spawn 3 branches"),
		                         row("r", "regenerate"),
		                         row("c", "compare two leaves"),
		                         row("e", "edit + refork"),
		                         row("y", "yank code"),
		                         row("t", "toggle thinking"),
		                         row("m", "bookmark"),
		                         row("g / x", "graft / export dot"),
		                         row("p / P", "prune / restore"),
		                         text(""),
		                         text("composer is vim-modal — i to insert, esc for normal") |
		                             color(col(th.p.muted)) | dim}));
	}
	const auto items = overlay_items();
	const std::string title =
	    ov == overlay::palette ? "commands" : (ov == overlay::picker ? "conversations" : "search");
	return ui::overlay(th, title, ui::pick_list(th, ov_filter, items, ov_sel));
}

}  // namespace plume
