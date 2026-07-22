// internal detail header: the app's impl struct, its helpers and all the
// rendering. split out of app.cpp to keep both files under the length limit;
// included only by app.cpp (app is a pimpl).
#pragma once

#include "plume/app.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <set>

#include "composer.hpp"
#include "plume/codec.hpp"
#include "plume/plugin.hpp"
#include "plume/provider.hpp"
#include "plume/store.hpp"
#include "plume/sync.hpp"
#include "plume/terminal.hpp"
#include "plume/theme.hpp"
#include "plume/weave.hpp"
#include "ui.hpp"
#include "wizard.hpp"

namespace plume {

using namespace ftxui;

namespace {

Color col(rgb c) {
	return Color::RGB(c.r, c.g, c.b);
}

std::int64_t now_ms() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct action {
	const char* name;
	const char* hint;
	bool takes_arg;
};

// the command set, shared by the palette and slash commands.
constexpr std::array<action, 13> kActions = {{
    {"weave", "open the loom", false},
    {"model", "set the model <id>", true},
    {"theme", "switch theme <name>", true},
    {"system", "set a system prompt <text>", true},
    {"search", "search this conversation <q>", true},
    {"compact", "summarize older turns", false},
    {"tag", "tag this conversation <name>", true},
    {"new", "start a fresh conversation", false},
    {"export", "export convo <md|json|html>", true},
    {"density", "toggle cozy / compact", false},
    {"motion", "toggle animations", false},
    {"help", "keybinding cheatsheet", false},
    {"quit", "leave plume", false},
}};

// subsequence fuzzy match: every char of pat appears in order in text.
bool fuzzy(std::string_view pat, std::string_view text) {
	std::size_t i = 0;
	for (char c : text) {
		if (i < pat.size() && std::tolower(static_cast<unsigned char>(c)) ==
		                          std::tolower(static_cast<unsigned char>(pat[i])))
			++i;
	}
	return i == pat.size();
}

provider_config pc_from(const provider_entry& pe) {
	provider_config pc;
	pc.kind = pe.kind;
	pc.base_url = pe.base_url;
	pc.credential.value = pe.auth_value;
	if (pe.auth_source == "key_cmd")
		pc.credential.kind = auth::source::key_cmd;
	else if (pe.auth_source == "keychain")
		pc.credential.kind = auth::source::keychain;
	else if (pe.auth_source == "inline")
		pc.credential.kind = auth::source::inline_key;
	else
		pc.credential.kind = auth::source::env;
	return pc;
}

}  // namespace

// defined in provider/mock.cpp.
std::unique_ptr<provider> make_mock();

// one branch of a parallel spawn: its own worker streaming into its own buffer.
struct sibling_stream {
	std::string text;
	bool done = false;
	node_id parent;
	std::thread worker;
};

struct app::impl {
	config cfg;
	theme th;
	term::capabilities caps;
	std::optional<store> db;
	std::unique_ptr<provider> prov;
	std::unique_ptr<plugin_host> plugins;

	ScreenInteractive screen = ScreenInteractive::Fullscreen();

	// the live conversation, materialized as the weave active path.
	convo_id convo;
	std::vector<node> transcript;

	// streaming state, touched from the ui thread only (workers marshal via Post).
	std::atomic<bool> streaming{false};
	std::atomic<bool> stop_flag{false};
	std::string live_text;
	std::string live_think;
	bool show_think = true;
	std::string status_error;
	std::string toast;  // a transient note in the statusbar (command results)
	usage last_usage;
	std::int64_t ttft_ms = 0;
	std::thread worker;

	plume::composer comp;
	bool in_weave = false;
	int weave_cursor = 0;
	std::vector<node_id> weave_order;
	node_id graft_source;    // set by the first 'g', consumed by the second
	std::string weave_note;  // transient feedback in the weave detail line

	wizard wiz;
	std::string convo_title = "first light";
	std::int64_t context_window = 200000;

	// parallel spawn (the loom): k branches streaming at once, in columns.
	bool spawning = false;
	bool spawn_done = false;
	std::atomic<bool> spawn_stop{false};
	std::vector<std::unique_ptr<sibling_stream>> siblings;
	int spawn_pending = 0;

	// overlays: the command palette, the cheatsheet, the conversation picker,
	// and search. only one is up at a time.
	enum class overlay : std::uint8_t { none, palette, cheatsheet, picker, search };
	overlay ov = overlay::none;
	std::string ov_filter;
	int ov_sel = 0;
	bool search_global = false;
	std::vector<search_hit> search_hits;
	std::vector<conversation> picker_convos;

	std::string system_prompt;       // per-conversation system prompt (/system)
	std::string compaction_summary;  // set by /compact; prepended to context
	std::size_t compaction_boundary = 0;
	std::thread compact_worker;
	std::atomic<bool> compacting{false};

	// the animation heartbeat: posts a frame while something is moving, unless
	// motion is off.
	std::thread ticker;
	std::atomic<bool> alive{true};

	explicit impl(config c) : cfg(std::move(c)) {}
	~impl() {
		stop_flag = true;
		spawn_stop = true;
		alive = false;
		if (worker.joinable()) worker.join();
		if (compact_worker.joinable()) compact_worker.join();
		for (auto& s : siblings)
			if (s->worker.joinable()) s->worker.join();
		if (ticker.joinable()) ticker.join();
	}

	bool has_provider() const { return prov != nullptr; }
	bool reduce_motion() const { return cfg.ui.reduce_motion; }
	bool compact() const { return cfg.ui.density == "compact"; }
	bool animating() const {
		return streaming.load() || wiz.animating() || spawning || compacting.load();
	}

	std::string model_id() const {
		const provider_entry* pe = cfg.active_provider();
		if (pe && !pe->default_model.empty()) return pe->default_model;
		if (!cfg.defaults.model.empty()) return cfg.defaults.model;
		return "claude-opus-4-8";
	}

	void reload_transcript() {
		if (!db) return;
		weave w(*db);
		if (auto path = w.active_path(convo)) transcript = *path;
	}

	// the request context (root -> leaf) as provider messages.
	std::vector<message> context_upto(std::optional<node_id> leaf) {
		std::vector<node> chain;
		std::optional<node_id> cur = std::move(leaf);
		while (cur) {
			auto n = db->node_of(*cur);
			if (!n) break;
			chain.push_back(*n);
			cur = n->parent;
		}
		std::reverse(chain.begin(), chain.end());
		std::vector<message> out;
		for (const auto& n : chain)
			if (auto b = codec::decode_blocks(n.content_json)) out.push_back(message{n.role, *b});
		return out;
	}

	// spawn k continuations of a node's parent turn, each streamed in parallel.
	void spawn_siblings(const node_id& anchor, int k) {
		if (spawning || !db || !prov) return;
		auto an = db->node_of(anchor);
		if (!an) return;
		const node_id parent =
		    an->role == role::user ? an->id : (an->parent ? *an->parent : an->id);
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

	void finish_sibling(sibling_stream* s, const result<completion>& out) {
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

	// -- commands, palette, overlays ------------------------------------------

	void persist_config() { static_cast<void>(save_config(cfg, cfg.config_dir + "/config.toml")); }

	void new_conversation() {
		conversation c;
		c.id = convo_id{new_id("convo")};
		c.title = "new thread";
		c.created_at = now_ms();
		static_cast<void>(db->put_conversation(c));
		convo = c.id;
		convo_title = c.title;
		system_prompt.clear();
		compaction_summary.clear();
		compaction_boundary = 0;
		reload_transcript();
	}

	void switch_convo(const convo_id& id) {
		convo = id;
		if (auto c = db->conversation_of(id)) convo_title = c->title;
		system_prompt.clear();
		compaction_summary.clear();
		compaction_boundary = 0;
		reload_transcript();
	}

	void do_export(const std::string& fmt) {
		if (!db) return;
		weave w(*db);
		std::string out;
		if (fmt == "json") {
			if (auto j = w.to_json(convo)) out = *j;
		} else {
			auto path = w.active_path(convo);
			if (path)
				for (const auto& n : *path) {
					auto b = codec::decode_blocks(n.content_json);
					const std::string body = b ? message{n.role, *b}.plain_text() : n.content_json;
					if (fmt == "html")
						out += "<section><h3>" + std::string(to_string(n.role)) + "</h3><pre>" + body +
						       "</pre></section>\n";
					else
						out += "## " + std::string(to_string(n.role)) + "\n\n" + body + "\n\n";
				}
		}
		std::error_code ec;
		std::filesystem::create_directories(cfg.state_dir, ec);
		const std::string file = cfg.state_dir + "/" + convo.str() + "." + fmt;
		std::ofstream(file) << out;
		toast = "exported to " + file;
	}

	void compact_now() {
		if (!prov || compacting.load() || transcript.size() < 3) {
			toast = "nothing to compact";
			return;
		}
		if (compact_worker.joinable()) compact_worker.join();
		const std::size_t keep = 2;
		const std::size_t upto = transcript.size() - keep;
		std::string convo_text;
		for (std::size_t i = 0; i < upto; ++i) {
			auto b = codec::decode_blocks(transcript[i].content_json);
			convo_text += std::string(to_string(transcript[i].role)) + ": " +
			              (b ? message{transcript[i].role, *b}.plain_text() : "") + "\n";
		}
		compacting = true;
		toast = "compacting...";
		compact_worker = std::thread([this, convo_text, upto] {
			request r;
			r.params = cfg.defaults;
			r.params.model = model_id();
			r.params.thinking = thinking_mode::off;
			r.messages.push_back(message::text(
			    role::user,
			    "Summarize the conversation so far in a few sentences, preserving key facts and "
			    "decisions. Reply with only the summary.\n\n" +
			        convo_text));
			std::string sum;
			auto res = prov->stream(
			    r,
			    [&sum](const stream_delta& d) {
				    if (d.type == stream_delta::kind::text) sum += d.text;
			    },
			    [] { return false; });
			screen.Post([this, sum, upto] {
				compacting = false;
				if (!sum.empty()) {
					compaction_summary = sum;
					compaction_boundary = upto;
					toast = "compacted " + std::to_string(upto) + " turns";
				} else {
					toast = "compaction failed";
				}
			});
			screen.PostEvent(Event::Custom);
		});
	}

	void run_search(const std::string& q, bool global) {
		search_global = global;
		ov = overlay::search;
		ov_filter = q;
		ov_sel = 0;
		search_hits.clear();
		if (!db || q.empty()) return;
		auto r = global ? db->search(q) : db->search_in(convo, q);
		if (r) search_hits = *r;
	}

	void goto_hit(const search_hit& h) {
		if (h.convo != convo) switch_convo(h.convo);
		weave w(*db);
		static_cast<void>(w.adopt(h.convo, h.node));
		reload_transcript();
		ov = overlay::none;
	}

	void run_command(const std::string& name, const std::string& arg) {
		ov = overlay::none;
		ov_filter.clear();
		if (name == "weave") {
			in_weave = true;
		} else if (name == "model") {
			cfg.defaults.model = arg;
			if (auto it = cfg.providers.find(cfg.default_provider); it != cfg.providers.end())
				it->second.default_model = arg;
			persist_config();
			toast = "model " + arg;
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
		} else if (name == "quit") {
			screen.ExitLoopClosure()();
		} else {
			toast = "unknown command: " + name;
		}
	}

	// run a "/name arg" line from the composer.
	void run_slash(const std::string& line) {
		const std::string body = line.substr(1);
		const auto sp = body.find(' ');
		const std::string name = body.substr(0, sp);
		const std::string arg = sp == std::string::npos ? "" : body.substr(sp + 1);
		run_command(name, arg);
	}

	std::vector<std::pair<std::string, std::string>> overlay_items() {
		std::vector<std::pair<std::string, std::string>> items;
		if (ov == overlay::palette) {
			for (const auto& a : kActions)
				if (fuzzy(ov_filter, a.name)) items.emplace_back(a.name, a.hint);
		} else if (ov == overlay::picker) {
			for (const auto& c : picker_convos) {
				const std::string title = c.title.empty() ? "(untitled)" : c.title;
				if (!ov_filter.empty() && !fuzzy(ov_filter, title)) continue;
				items.emplace_back(title, c.source);
			}
		} else if (ov == overlay::search) {
			for (const auto& h : search_hits) items.emplace_back(h.snippet, h.node.str().substr(0, 12));
		}
		return items;
	}

	Element overlay_view() {
		if (ov == overlay::cheatsheet) {
			auto row = [&](const std::string& k, const std::string& d) {
				return hbox({text(k) | color(col(th.p.foam)) | size(WIDTH, EQUAL, 14),
				             text(d) | color(col(th.p.subtle))});
			};
			return ui::overlay(
			    th, "keys",
			    vbox({row("ctrl-k", "command palette"), row("ctrl-p", "conversation picker"),
			          row("ctrl-f", "search everything"), row("ctrl-w", "open the loom"),
			          row("ctrl-e", "composer to $EDITOR"), row("/", "slash command (insert)"),
			          row("? ", "this cheatsheet (normal)"), row("esc", "stop / close"),
			          text(""), text("weave") | color(col(th.p.gold)),
			          row("hjkl", "move"), row("enter", "adopt branch"), row("s", "spawn 3 branches"),
			          row("g / x", "graft / export dot"), row("p / P", "prune / restore"),
			          text(""), text("composer is vim-modal — i to insert, esc for normal") |
			                        color(col(th.p.muted)) | dim}));
		}
		const auto items = overlay_items();
		const std::string title =
		    ov == overlay::palette ? "commands" : (ov == overlay::picker ? "conversations" : "search");
		return ui::overlay(th, title, ui::pick_list(th, ov_filter, items, ov_sel));
	}

	bool handle_overlay(const Event& e) {
		if (e == Event::Escape) {
			ov = overlay::none;
			return true;
		}
		if (ov == overlay::cheatsheet) {
			ov = overlay::none;
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
			} else if (ov == overlay::search && ov_sel >= 0 && ov_sel < static_cast<int>(search_hits.size())) {
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

	// ctrl-e: bounce the composer out to $EDITOR and back, with the terminal
	// restored around the child process.
	void edit_in_editor() {
		const char* ed = std::getenv("EDITOR");
		const std::string editor = ed && *ed ? ed : "vi";
		const std::string path =
		    (std::filesystem::temp_directory_path() / ("plume-compose-" + new_id("c") + ".md"))
		        .string();
		{
			std::ofstream out(path);
			out << comp.value();
		}
		screen.WithRestoredIO(
		    [&] { static_cast<void>(std::system((editor + " " + path).c_str())); })();
		std::ifstream in(path);
		std::string body((std::istreambuf_iterator<char>(in)), {});
		while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
		comp.set_text(body);
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}

	// persist a user turn and a streaming assistant placeholder, then kick a
	// worker to stream the reply into live_text.
	void send(const std::string& raw) {
		if (streaming || !db || !prov || raw.empty()) return;
		if (worker.joinable()) worker.join();

		// plugins may rewrite the outgoing message (pre_send). cheap text hooks
		// run inline; a hook that reaches for a model only does so when the user
		// granted its net capability.
		const std::string text = plugins ? plugins->run_pre_send(raw) : raw;

		weave w(*db);
		std::optional<node_id> parent;
		if (auto conv = db->conversation_of(convo); conv) parent = conv->active_leaf;

		node user;
		user.id = node_id{new_id("node")};
		user.convo = convo;
		user.parent = parent;
		user.role = role::user;
		user.content_json = codec::encode_blocks({text_block{text}});
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
		std::string sys = system_prompt;
		if (!compaction_summary.empty())
			sys += (sys.empty() ? "" : "\n\n") + std::string("summary of earlier turns: ") +
			       compaction_summary;
		if (!sys.empty()) req.system = sys;
		const std::size_t begin =
		    compaction_summary.empty() ? 0 : std::min(compaction_boundary, transcript.size());
		for (std::size_t i = begin; i < transcript.size(); ++i) {
			auto blocks = codec::decode_blocks(transcript[i].content_json);
			if (blocks) req.messages.push_back(message{transcript[i].role, *blocks});
		}
		req.cache_prefix = true;

		live_text.clear();
		live_think.clear();
		status_error.clear();
		streaming = true;
		stop_flag = false;
		ttft_ms = 0;
		const std::int64_t start = now_ms();
		const node_id user_id = user.id;

		worker = std::thread([this, req, start, user_id] {
			auto on_delta = [&](const stream_delta& d) {
				screen.Post([this, d, start] {
					if (ttft_ms == 0 && (d.type == stream_delta::kind::text ||
					                     d.type == stream_delta::kind::thinking))
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

	void finish_stream(const result<completion>& out, const node_id& parent) {
		streaming = false;
		node reply;
		reply.id = node_id{new_id("node")};
		reply.convo = convo;
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
			status_error = out.error().detail;
			reply.content_json =
			    codec::encode_blocks({text_block{"[error: " + out.error().detail + "]"}});
			reply.state = node_state::error;
		}
		if (db) {
			static_cast<void>(db->put_node(reply));
			static_cast<void>(db->set_active_leaf(convo, reply.id));
		}
		if (plugins && out) plugins->run_post_receive(out->reply.plain_text());
		live_text.clear();
		live_think.clear();
		reload_transcript();
	}

	// adopt the wizard's choices: persist the config, reload the theme, build the
	// provider, and import a claude.ai export if one was named.
	void apply_wizard() {
		cfg = wiz.result;
		static_cast<void>(save_config(cfg, cfg.config_dir + "/config.toml"));

		const bool dark = caps.background ? caps.dark : true;
		if (auto t = resolve_theme(cfg.ui.theme, cfg.config_dir + "/themes", dark)) th = *t;

		if (std::getenv("PLUME_MOCK"))
			prov = make_mock();
		else if (const provider_entry* pe = cfg.active_provider())
			if (auto p = make_provider(pc_from(*pe))) prov = std::move(*p);

		if (!wiz.import_path.empty() && db) {
			if (auto backend = open_export(wiz.import_path))
				static_cast<void>((*backend)->import_into(*db));
			if (auto list = db->conversations(); list && !list->empty()) {
				convo = list->front().id;
				convo_title = list->front().title;
			}
			reload_transcript();
		}
		wiz.active = false;
		wiz.finished = false;
	}

	// -- rendering ------------------------------------------------------------

	double cost_of(const usage& u) const {
		if (auto it = cfg.prices.find(model_id()); it != cfg.prices.end())
			return (u.input * it->second.input + u.output * it->second.output +
			        u.cache_read * it->second.cache_read +
			        u.cache_creation * it->second.cache_write) /
			       1e6;
		return 0.0;
	}

	Element header() {
		return vbox({
		    hbox({text(" "), ui::gradient_text("plume", {th.p.love, th.p.iris, th.p.foam}) | bold,
		          text("  " + convo_title) | color(col(th.p.subtle)), filler(),
		          text(in_weave ? "weave " : "") | color(col(th.p.foam)) | dim}),
		    separator() | color(col(th.p.hl_med)),
		});
	}

	Element statusbar() {
		usage tot = last_usage;
		std::string cost = std::to_string(cost_of(tot));
		cost = cost.substr(0, cost.find('.') + 5);
		const float frac =
		    context_window > 0 ? static_cast<float>(tot.input) / context_window : 0.f;
		auto dot = [&] { return text(" · ") | color(col(th.p.muted)); };
		Elements line = {
		    ui::pill(th, model_id(), th.p.iris),
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
		if (!toast.empty()) line.push_back(text(toast + " ") | color(col(th.p.foam)));
		if (streaming || compacting.load())
			line.push_back(text(ui::spinner(now_ms()) + " streaming ") | color(col(th.p.rose)));
		line.push_back(text(in_weave ? " weave " : " chat ") | color(col(th.p.subtle)) |
		               bgcolor(col(th.p.overlay)));
		return hbox(std::move(line)) | bgcolor(col(th.p.surface));
	}

	Element transcript_view() {
		Elements out;
		const bool cmp = compact();
		for (const auto& n : transcript) {
			auto blocks = codec::decode_blocks(n.content_json);
			std::string body;
			std::vector<std::string> thinks;
			if (blocks) {
				body = message{n.role, *blocks}.plain_text();
				for (const auto& b : *blocks)
					if (const auto* t = std::get_if<thinking_block>(&b))
						thinks.push_back(t->thinking);
			} else {
				body = n.content_json;
			}
			out.push_back(
			    ui::message_card(th, n.role, body, thinks, show_think, cmp, n.model, n.tokens_out));
		}
		if (streaming || !live_text.empty() || !live_think.empty())
			out.push_back(ui::streaming_card(th, live_text, live_think, show_think, cmp, now_ms(),
			                                 reduce_motion()));
		if (out.empty())
			out.push_back(vbox({filler(), text("a blank page.") | color(col(th.p.subtle)) | center,
			                    text("type below to begin · ctrl-w to weave") |
			                        color(col(th.p.muted)) | dim | center,
			                    filler()}));
		if (!status_error.empty()) out.push_back(text("  " + status_error) | color(col(th.p.love)));
		out.back() = out.back() | focus;  // keep the newest turn in view
		return vbox(std::move(out)) | yframe | flex;
	}

	Element weave_view() {
		weave w(*db);
		auto v = w.view(convo, false);
		Elements rows;
		weave_order.clear();
		if (v) {
			for (const auto& id : v->preorder) {
				const auto& tn = v->nodes.at(id);
				weave_order.push_back(id);
				const int i = static_cast<int>(weave_order.size()) - 1;
				const std::string label =
				    codec::read_str(tn.data.params_json, "label").value_or("");
				const bool mark = codec::read_bool(tn.data.params_json, "bookmark").value_or(false);
				const bool is_source = !graft_source.empty() && id == graft_source;
				const rgb c = tn.data.role == role::user ? th.p.pine : th.p.iris;
				std::string indent;
				for (int d = 0; d < tn.depth; ++d) indent += "  ";
				const std::string glyph = tn.data.role == role::user ? "◇ " : "◆ ";
				Elements cells = {
				    text(indent), text(mark ? "★ " : "") | color(col(th.p.gold)),
				    text(glyph) | color(col(c)),
				    text(std::string(to_string(tn.data.role))) | color(col(th.p.text))};
				if (!label.empty())
					cells.push_back(text("  " + label) | color(col(th.p.foam)) | italic);
				if (is_source)
					cells.push_back(text("  graft source") | color(col(th.p.gold)) | dim);
				Element e = hbox(std::move(cells));
				if (i == weave_cursor)
					e = hbox({text("▏") | color(col(th.p.iris)), e}) | bgcolor(col(th.p.hl_low)) |
					    focus;
				else
					e = hbox({text(" "), e});
				rows.push_back(e);
			}
		}
		if (rows.empty()) rows.push_back(text("  the tree is empty") | color(col(th.p.muted)));
		Element tree = vbox(std::move(rows)) | yframe | flex;
		Element help = text(
		                   "hjkl move · enter adopt · p prune · P restore · m bookmark · g graft · "
		                   "x export dot · ctrl-w back") |
		               color(col(th.p.subtle)) | dim;
		Elements bottom = {help};
		if (!weave_note.empty()) bottom.push_back(text("  " + weave_note) | color(col(th.p.gold)));
		return vbox({hbox({text(" the loom ") | bold | color(col(th.p.base)) |
		                       bgcolor(col(th.p.iris)),
		                   filler()}),
		             separator() | color(col(th.p.hl_med)), tree,
		             separator() | color(col(th.p.hl_med)), vbox(std::move(bottom))}) |
		       flex;
	}

	Element spawn_view() {
		Elements cols;
		for (std::size_t i = 0; i < siblings.size(); ++i) {
			sibling_stream* s = siblings[i].get();
			const rgb tint =
			    std::array{th.p.iris, th.p.foam, th.p.rose, th.p.gold, th.p.pine}[i % 5];
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

	node_id weave_selected() const {
		if (weave_cursor >= 0 && weave_cursor < static_cast<int>(weave_order.size()))
			return weave_order[static_cast<std::size_t>(weave_cursor)];
		return {};
	}

	bool handle_weave(const Event& e) {
		weave w(*db);
		auto move = [&](int d) {
			weave_cursor = std::max(
			    0, std::min<int>(weave_cursor + d, static_cast<int>(weave_order.size()) - 1));
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
};

}  // namespace plume
