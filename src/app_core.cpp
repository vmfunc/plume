// the app's core behaviour: sending, streaming, parallel spawn, the command
// table and the overlay keymap. split out of app_impl.hpp for headroom.
#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

void app::impl::spawn_siblings(const node_id& anchor, int k) {
	if (spawning || !db || !prov) return;
	auto an = db->node_of(anchor);
	if (!an) return;
	const node_id parent = an->role == role::user ? an->id : (an->parent ? *an->parent : an->id);
	const std::vector<message> ctx = context_upto(parent);

	siblings.clear();
	siblings.reserve(static_cast<std::size_t>(k));
	spawning = true;
	spawn_done = false;
	spawn_stop = false;
	spawn_pending = k;
	in_weave = false;

	for (int i = 0; i < k; ++i) {
		siblings.push_back(std::make_unique<sibling_stream>());
		sibling_stream* s = siblings.back().get();
		s->parent = parent;
		request req;
		req.params = cfg.defaults;
		req.params.model = model_id();
		if (req.params.max_tokens < 1024) req.params.max_tokens = 4096;
		req.messages = ctx;
		s->worker = std::thread([this, s, req] {
			auto out = prov->stream(
			    req,
			    [this, s](const stream_delta& d) {
				    if (d.type == stream_delta::kind::text) {
					    std::string t = d.text;
					    screen.Post([s, t = std::move(t)] { s->text += t; });
					    screen.PostEvent(Event::Custom);
				    }
			    },
			    [this] { return spawn_stop.load(); });
			screen.Post([this, s, out = std::move(out)] { finish_sibling(s, out); });
			screen.PostEvent(Event::Custom);
		});
	}
}

void app::impl::finish_sibling(sibling_stream* s, const result<completion>& out) {
	s->done = true;
	node reply;
	reply.id = node_id{new_id("node")};
	reply.convo = convo;
	reply.parent = s->parent;
	reply.role = role::assistant;
	reply.model = model_id();
	reply.created_at = now_ms();
	if (out) {
		reply.content_json = codec::encode_blocks(out->reply.blocks);
		reply.tokens_out = out->tokens.output;
	} else {
		reply.content_json = codec::encode_blocks({text_block{s->text}});
	}
	if (db) static_cast<void>(db->put_node(reply));
	if (--spawn_pending <= 0) {
		spawning = false;
		spawn_done = true;
	}
}

void app::impl::run_command(const std::string& name, const std::string& arg) {
	ov = overlay::none;
	ov_filter.clear();
	if (name == "weave") {
		in_weave = true;
	} else if (name == "autoweave") {
		if (int k; !arg.empty() &&
		           (std::from_chars(arg.data(), arg.data() + arg.size(), k).ec == std::errc{}) &&
		           k > 0)
			autoweave_fan = k;
		autoweave = !autoweave;
		toast = autoweave
		            ? "autoweave on: fans " + std::to_string(autoweave_fan) + " per turn to a $" +
		                  std::to_string(autoweave_cap).substr(0, 4) + " cap"
		            : "autoweave off";
	} else if (name == "models") {
		open_models();
	} else if (name == "model") {
		if (arg.empty()) {
			open_models();  // bare /model opens the picker
		} else {
			choose_model(arg);  // set it for this conversation
		}
	} else if (name == "theme") {
		const bool dark = caps.background ? caps.dark : true;
		if (auto t = resolve_theme(arg, cfg.config_dir + "/themes", dark)) {
			th = *t;
			cfg.ui.theme = arg;
			persist_config();
		}
	} else if (name == "system") {
		system_prompt = arg;
		toast = "system prompt set";
	} else if (name == "roles") {
		open_roles();
	} else if (name == "role") {
		if (arg.rfind("save ", 0) == 0)
			save_role(arg.substr(5));
		else
			open_roles();
	} else if (name == "snip" || name == "snippets") {
		open_snips();
	} else if (name == "search") {
		run_search(arg, false);
	} else if (name == "compact") {
		compact_now();
	} else if (name == "tag") {
		if (db && !arg.empty()) static_cast<void>(db->tag(convo, arg));
		toast = "tagged " + arg;
	} else if (name == "new") {
		new_conversation();
	} else if (name == "export") {
		do_export(arg.empty() ? "md" : arg);
	} else if (name == "density") {
		cfg.ui.density = cfg.ui.density == "cozy" ? "compact" : "cozy";
		persist_config();
	} else if (name == "motion") {
		cfg.ui.reduce_motion = !cfg.ui.reduce_motion;
		persist_config();
	} else if (name == "help") {
		ov = overlay::cheatsheet;
	} else if (name == "settings") {
		open_settings();
	} else if (name == "sidebar") {
		toggle_sidebar();
	} else if (name == "cost") {
		open_cost();
	} else if (name == "plan") {
		plan_mode = !plan_mode;
		toast = plan_mode ? "plan mode on" : "plan mode off";
	} else if (name == "websearch" || name == "web") {
		web_on = !web_on;
		toast = web_on ? "web search on" : "web search off";
	} else if (name == "inspect") {
		open_inspect();
	} else if (name == "artifacts") {
		open_artifacts();
	} else if (name == "mouse") {
		mouse_on = !mouse_on;
		screen.TrackMouse(mouse_on);
		toast = mouse_on ? "mouse on" : "mouse off (terminal selection)";
	} else if (name == "continue") {
		continue_turn();
	} else if (name == "attach") {
		attach(arg);
	} else if (name == "quit") {
		screen.ExitLoopClosure()();
	} else if (set_param(name, arg)) {
		// handled: params / temp / top_p / max / think
	} else {
		toast = "unknown command: " + name;
	}
}

bool app::impl::handle_overlay(const Event& e) {
	if (ov == overlay::tool_approve) {
		if (tool_queue.empty()) {
			ov = overlay::none;
			return true;
		}
		const pending_tool pt = tool_queue.front();
		auto resolve = [&](bool approve) {
			if (approve)
				run_tool(pt);
			else
				tool_results.push_back(tool_result_block{pt.id, "denied by user", true});
			tool_queue.erase(tool_queue.begin());
			ov = overlay::none;
			advance_tools();  // next tool, or continue the turn
		};
		if (e == Event::Character("a") || e == Event::Return) return resolve(true), true;
		if (e == Event::Character("A")) {  // remember this tool on its server
			for (auto& sv : cfg.mcp)
				if (sv.name == pt.server) sv.allow.push_back(pt.name);
			persist_config();
			return resolve(true), true;
		}
		if (e == Event::Character("d")) return resolve(false), true;
		if (e == Event::Escape) {  // deny this and every queued tool at once
			for (auto& q : tool_queue)
				tool_results.push_back(tool_result_block{q.id, "denied by user", true});
			tool_queue.clear();
			ov = overlay::none;
			submit_tool_results();
			return true;
		}
		return true;  // swallow other keys while the prompt is up
	}
	if (ov == overlay::models) return handle_models(e);
	if (ov == overlay::settings) return handle_settings(e);
	if (ov == overlay::roles) return handle_roles(e);
	if (ov == overlay::snippets) return handle_snips(e);
	if (ov == overlay::artifacts) return handle_artifacts(e);
	if (e == Event::Escape) {
		ov = overlay::none;
		return true;
	}
	if (ov == overlay::cheatsheet || ov == overlay::cost || ov == overlay::inspect) {
		ov = overlay::none;  // read-only overlays close on any key
		return true;
	}
	const auto items = overlay_items();
	const int max = static_cast<int>(items.size()) - 1;
	if (e == Event::ArrowDown || e == Event::CtrlN) {
		ov_sel = std::min(ov_sel + 1, std::max(0, max));
		return true;
	}
	if (e == Event::ArrowUp || e == Event::CtrlP) {
		ov_sel = std::max(ov_sel - 1, 0);
		return true;
	}
	if (e == Event::Backspace) {
		if (!ov_filter.empty()) ov_filter.pop_back();
		ov_sel = 0;
		if (ov == overlay::search) run_search(ov_filter, search_global);
		return true;
	}
	if (e == Event::Return) {
		if (ov == overlay::palette && ov_sel <= max) {
			// map the visible row back to the action.
			int seen = 0;
			for (const auto& a : kActions) {
				if (!fuzzy(ov_filter, a.name)) continue;
				if (seen == ov_sel) {
					if (a.takes_arg) {
						comp.set_text(std::string("/") + a.name + " ");
						ov = overlay::none;
					} else {
						run_command(a.name, "");
					}
					return true;
				}
				++seen;
			}
		} else if (ov == overlay::picker && ov_sel <= max) {
			int seen = 0;
			for (const auto& c : picker_convos) {
				const std::string title = c.title.empty() ? "(untitled)" : c.title;
				if (!ov_filter.empty() && !fuzzy(ov_filter, title)) continue;
				if (seen == ov_sel) {
					switch_convo(c.id);
					ov = overlay::none;
					return true;
				}
				++seen;
			}
		} else if (ov == overlay::search && ov_sel >= 0 &&
		           ov_sel < static_cast<int>(search_hits.size())) {
			goto_hit(search_hits[static_cast<std::size_t>(ov_sel)]);
		}
		return true;
	}
	if (e.is_character()) {
		ov_filter += e.character();
		ov_sel = 0;
		if (ov == overlay::search) run_search(ov_filter, search_global);
		return true;
	}
	return true;
}

void app::impl::send(const std::string& raw) {
	if (streaming || !db || !prov || raw.empty()) return;
	if (worker.joinable()) worker.join();
	if (input_history.empty() || input_history.back() != raw) input_history.push_back(raw);
	history_pos = -1;

	// plugins may rewrite the outgoing message (pre_send). cheap text hooks
	// run inline; a hook that reaches for a model only does so when the user
	// granted its net capability.
	const std::string text = plugins ? plugins->run_pre_send(raw) : raw;

	weave w(*db);
	std::optional<node_id> parent;
	if (refork_parent) {
		parent = *refork_parent;  // an edited turn forks a sibling from its old parent
		refork_parent.reset();
	} else if (auto conv = db->conversation_of(convo); conv) {
		parent = conv->active_leaf;
	}

	node user;
	user.id = node_id{new_id("node")};
	user.convo = convo;
	user.parent = parent;
	user.role = role::user;
	// stage any /attach images/pdfs ahead of the prose, then reset the tray.
	std::vector<content_block> user_blocks = std::move(pending_attach);
	pending_attach.clear();
	user_blocks.push_back(text_block{text});
	user.content_json = codec::encode_blocks(user_blocks);
	user.created_at = now_ms();
	if (auto r = db->put_node(user); !r) {
		status_error = r.error().detail;
		return;
	}
	static_cast<void>(db->set_active_leaf(convo, user.id));
	reload_transcript();

	// build the request from the active path, honoring the system prompt and
	// any compaction (older turns replaced by a summary, originals kept).
	request req;
	req.params = cfg.defaults;
	req.params.model = model_id();
	if (req.params.max_tokens < 1024) req.params.max_tokens = 4096;
	if (std::string sys = effective_system(); !sys.empty()) req.system = sys;
	const std::size_t begin =
	    compaction_summary.empty() ? 0 : std::min(compaction_boundary, transcript.size());
	for (std::size_t i = begin; i < transcript.size(); ++i) {
		auto blocks = codec::decode_blocks(transcript[i].content_json);
		if (blocks) req.messages.push_back(message{transcript[i].role, *blocks});
	}
	req.tools = tool_defs();
	req.web_search = web_on;
	req.cache_prefix = true;

	live_text.clear();
	live_think.clear();
	status_error.clear();
	stream_convo = convo;
	streaming = true;
	stop_flag = false;
	ttft_ms = 0;
	const std::int64_t start = now_ms();
	const node_id user_id = user.id;

	worker = std::thread([this, req, start, user_id] {
		auto on_delta = [&](const stream_delta& d) {
			screen.Post([this, d, start] {
				if (ttft_ms == 0 &&
				    (d.type == stream_delta::kind::text || d.type == stream_delta::kind::thinking))
					ttft_ms = now_ms() - start;
				if (d.type == stream_delta::kind::text) {
					live_text += d.text;
					if (plugins) plugins->run_on_chunk(d.text);
				} else if (d.type == stream_delta::kind::thinking)
					live_think += d.text;
				else if (d.type == stream_delta::kind::usage)
					last_usage = d.tokens;
			});
			screen.PostEvent(Event::Custom);
		};
		auto stop = [this] { return stop_flag.load(); };
		auto out = prov->stream(req, on_delta, stop);

		screen.Post([this, out = std::move(out), user_id] { finish_stream(out, user_id); });
		screen.PostEvent(Event::Custom);
	});
}

void app::impl::finish_stream(const result<completion>& out, const node_id& parent) {
	streaming = false;
	const convo_id sc = stream_convo;  // the stream may have outlived a convo switch
	const bool active = sc == convo;   // is its conversation the one on screen?
	node reply;
	reply.id = node_id{new_id("node")};
	reply.convo = sc;
	reply.parent = parent;
	reply.role = role::assistant;
	reply.model = model_id();
	reply.created_at = now_ms();
	if (out) {
		reply.content_json = codec::encode_blocks(out->reply.blocks);
		reply.tokens_in = out->tokens.input;
		reply.tokens_out = out->tokens.output;
		reply.state = node_state::complete;
		last_usage = out->tokens;
	} else {
		if (active) status_error = out.error().detail;
		reply.content_json =
		    codec::encode_blocks({text_block{"[error: " + out.error().detail + "]"}});
		reply.state = node_state::error;
	}
	if (db) {
		static_cast<void>(db->put_node(reply));
		static_cast<void>(db->set_active_leaf(sc, reply.id));
	}
	if (plugins && out) plugins->run_post_receive(out->reply.plain_text());
	live_text.clear();
	live_think.clear();
	if (active) reload_transcript();

	if (!active) {  // a background conversation finished; just persist and nudge
		notify_done();
		return;
	}

	// a turn cut short by the token cap or a paused server-tool loop resumes with
	// /continue (the web search loop caps its iterations and returns pause_turn).
	truncated = out && (out->stop_reason == "max_tokens" || out->stop_reason == "pause_turn");
	if (truncated)
		toast = out->stop_reason == "pause_turn" ? "search paused, /continue to resume"
		                                         : "response truncated, /continue to resume";

	// if the assistant asked for tools, resolve them and loop.
	if (out) {
		for (const auto& b : out->reply.blocks)
			if (const auto* tu = std::get_if<tool_use_block>(&b)) {
				pending_tool pt;
				pt.id = tu->id;
				pt.name = tu->name;
				pt.args_json = tu->input_json.empty() ? "{}" : tu->input_json;
				for (const auto& mt : mcp_tools)
					if (mt.name == tu->name) pt.server = mt.server;
				for (const auto& sv : cfg.mcp)
					if (sv.name == pt.server) pt.policy = sv.approval;
				tool_queue.push_back(std::move(pt));
			}
		if (!tool_queue.empty()) {
			tool_parent = reply.id;
			advance_tools();
			return;  // notify only when the whole exchange settles
		}
	}
	notify_done();
	if (out && !truncated) maybe_autoweave(reply.id);
}

}  // namespace plume
