// internal detail header: the app's impl struct, its helpers and all the
// rendering. split out of app.cpp to keep both files under the length limit;
// included only by app.cpp (app is a pimpl).
#pragma once

#include "plume/app.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <set>

#include "composer.hpp"
#include "mouse.hpp"
#include "plume/codec.hpp"
#include "plume/image.hpp"
#include "plume/mcp.hpp"
#include "plume/plugin.hpp"
#include "plume/provider.hpp"
#include "plume/store.hpp"
#include "plume/sync.hpp"
#include "plume/terminal.hpp"
#include "plume/theme.hpp"
#include "plume/weave.hpp"
#include "ui.hpp"
#include "util_base64.hpp"
#include "wizard.hpp"

namespace plume {

using namespace ftxui;

namespace {

[[maybe_unused]] Color col(rgb c) {  // used by the render TUs, not every includer
	return Color::RGB(c.r, c.g, c.b);
}

std::int64_t now_ms() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// copy to the system clipboard via OSC 52. the terminal consumes the escape
// immediately, so it survives ftxui repainting the cells around it.
[[maybe_unused]] void osc52_copy(std::string_view s) {
	const std::string seq = "\x1b]52;c;" + detail::base64_encode(s) + "\x07";
	std::fwrite(seq.data(), 1, seq.size(), stdout);
	std::fflush(stdout);
}

// the last fenced code block if there is one, else the whole text.
[[maybe_unused]] std::string last_code_block(const std::string& body) {
	std::size_t close = body.rfind("```");
	if (close == std::string::npos) return body;
	std::size_t open = body.rfind("```", close == 0 ? 0 : close - 1);
	if (open == std::string::npos || open == close) return body;
	std::size_t nl = body.find('\n', open);  // skip the ```lang line
	if (nl == std::string::npos || nl >= close) return body;
	return body.substr(nl + 1, close - nl - 1);
}

struct action {
	const char* name;
	const char* hint;
	bool takes_arg;
};

// the command set, shared by the palette and slash commands.
constexpr std::array<action, 30> kActions = {{
    {"weave", "open the loom", false},
    {"autoweave", "toggle auto-fan <k>", true},
    {"models", "pick a model (ctrl-l)", false},
    {"model", "set the model <id>", true},
    {"attach", "attach an image/pdf <path>", true},
    {"params", "show sampling params", false},
    {"temp", "set temperature <0..1>", true},
    {"top_p", "set top_p <0..1>", true},
    {"max", "set max_tokens <n>", true},
    {"think", "thinking <off|adaptive|n>", true},
    {"theme", "switch theme <name>", true},
    {"system", "set a system prompt <text>", true},
    {"roles", "apply a persona", false},
    {"role", "save current as a role <name>", true},
    {"snip", "insert a snippet (ctrl-t)", false},
    {"search", "search this conversation <q>", true},
    {"compact", "summarize older turns", false},
    {"continue", "resume a truncated turn", false},
    {"tag", "tag this conversation <name>", true},
    {"new", "start a fresh conversation", false},
    {"export", "export convo <md|json|html>", true},
    {"density", "toggle cozy / compact", false},
    {"motion", "toggle animations", false},
    {"settings", "preferences (click statusbar)", false},
    {"sidebar", "toggle the sidebar", false},
    {"cost", "token + spend dashboard", false},
    {"inspect", "raw node + nerd stats", false},
    {"mouse", "toggle mouse capture", false},
    {"help", "keybinding cheatsheet (f1)", false},
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
	std::unique_ptr<mcp_client> mcp;
	std::vector<mcp_tool> mcp_tools;

	ScreenInteractive screen = ScreenInteractive::Fullscreen();

	// the live conversation, materialized as the weave active path.
	convo_id convo;
	std::vector<node> transcript;

	// transcript scrollback: a message cursor over `transcript`. follow_tail keeps
	// the newest turn in view; scrolling up pins a message and drops the tail.
	int transcript_sel = -1;
	bool follow_tail = true;
	bool pending_g = false;  // first 'g' of a gg (jump to top)

	// composer input history (recalled with the arrows on an empty line).
	std::vector<std::string> input_history;
	int history_pos = -1;
	void recall_history(int dir) {
		if (input_history.empty()) return;
		if (history_pos < 0) history_pos = static_cast<int>(input_history.size());
		history_pos = std::clamp(history_pos + dir, 0, static_cast<int>(input_history.size()));
		comp.set_text(history_pos < static_cast<int>(input_history.size())
		                  ? input_history[static_cast<std::size_t>(history_pos)]
		                  : "");
	}

	// re-run a turn that came back as an error, from its user parent.
	void retry_last() {
		if (transcript.empty() || !db) return;
		const node last = transcript.back();
		if (last.state != node_state::error || !last.parent) return;
		weave w(*db);
		static_cast<void>(w.prune(last.id));  // drop the error leaf
		stream_reply(*last.parent);
	}

	// mouse hit-test registry, rebuilt every frame. a deque (never a vector): hot()
	// hands reflect() a Box& into the element tree, so a reallocation would dangle
	// every earlier reflect. cleared at the top of each render pass.
	std::deque<hit_region> regions_;
	Element hot(Element e, hit_kind k, int index = 0) {
		regions_.push_back(hit_region{Box{}, k, index});
		return e | reflect(regions_.back().box);
	}
	const hit_region* hit_test(int x, int y) const;
	bool handle_mouse(const Mouse& m);
	std::int64_t last_click_ms = 0;  // for double-click detection
	int last_click_index = -1;
	hit_kind last_click_kind = hit_kind::none;

	// step the message cursor by delta turns; reaching the last turn re-pins tail.
	void scroll_transcript(int delta) {
		const int last = static_cast<int>(transcript.size()) - 1;
		if (last < 0) return;
		int cur = follow_tail ? last : transcript_sel;
		cur = std::clamp(cur + delta, 0, last);
		transcript_sel = cur;
		follow_tail = (cur == last);
	}
	void scroll_top() {
		if (transcript.empty()) return;
		transcript_sel = 0;
		follow_tail = false;
	}
	void scroll_tail() {
		transcript_sel = -1;
		follow_tail = true;
	}

	// act on the selected transcript turn (the message action bar / its keys).
	void message_action(char a) {
		if (transcript_sel < 0 || transcript_sel >= static_cast<int>(transcript.size())) return;
		const node n = transcript[static_cast<std::size_t>(transcript_sel)];
		switch (a) {
			case 'y': osc52_copy(node_text(n)), toast = "copied message"; break;
			case 'c': osc52_copy(last_code_block(node_text(n))), toast = "copied code"; break;
			case 'e':  // edit as a new branch from this turn's parent
				comp.set_text(node_text(n));
				refork_parent = n.parent;
				scroll_tail();
				break;
			case 'q':  // quote into the composer
				comp.set_text("> " + node_text(n) + "\n\n");
				break;
			case 'r': spawn_siblings(n.id, 1); break;  // regenerate
			case 'b': spawn_siblings(n.id, 3); break;  // branch three ways
			case 'x':                                  // prune to the graveyard
				if (db) {
					weave w(*db);
					static_cast<void>(w.prune(n.id));
					reload_transcript();
					scroll_tail();
				}
				break;
			default: break;
		}
	}

	// streaming state, touched from the ui thread only (workers marshal via Post).
	std::atomic<bool> streaming{false};
	std::atomic<bool> stop_flag{false};
	std::string live_text;
	std::string live_think;
	bool show_think = true;
	bool truncated = false;  // last turn hit max_tokens; /continue resumes
	std::string status_error;
	std::string toast;  // a transient note in the statusbar (command results)
	usage last_usage;
	std::int64_t ttft_ms = 0;
	std::thread worker;

	plume::composer comp;
	bool in_weave = false;
	int weave_cursor = 0;
	std::vector<node_id> weave_order;
	node_id graft_source;                  // set by the first 'g', consumed by the second
	std::optional<node_id> refork_parent;  // set by 'e': the next send forks here
	std::string weave_note;                // transient feedback in the weave detail line

	// compare (§100): two leaves picked with 'c', diffed side by side.
	bool comparing = false;
	node_id cmp_a, cmp_b;

	// autoweave (§101): after each of your turns, fan out k continuations up to a
	// spend cap, so alternatives are always waiting in the loom. off by default.
	bool autoweave = false;
	int autoweave_fan = 3;
	double autoweave_cap = 0.50;  // dollars of turn cost before it stops firing

	wizard wiz;
	std::string convo_title = "first light";
	std::int64_t context_window = 200000;

	// the mcp tool-use loop. when an assistant turn asks for tools, each request
	// is queued and resolved against the server's approval policy (ask/allowlist/
	// yolo); results are gathered into a tool_result turn that continues the chat.
	struct pending_tool {
		std::string server, name, id, args_json, policy;
	};
	std::vector<pending_tool> tool_queue;
	std::vector<tool_result_block> tool_results;
	node_id tool_parent;  // the assistant turn that requested the tools

	// images/pdfs staged with /attach, folded into the next user turn.
	std::vector<content_block> pending_attach;

	// parallel spawn (the loom): k branches streaming at once, in columns.
	bool spawning = false;
	bool spawn_done = false;
	std::atomic<bool> spawn_stop{false};
	std::vector<std::unique_ptr<sibling_stream>> siblings;
	int spawn_pending = 0;

	// overlays: the command palette, the cheatsheet, the conversation picker,
	// search, the model picker, and the mcp tool-approval prompt. one at a time.
	enum class overlay : std::uint8_t {
		none,
		palette,
		cheatsheet,
		picker,
		search,
		models,
		settings,
		cost,
		roles,
		snippets,
		inspect,
		tool_approve,
	};
	overlay ov = overlay::none;
	std::string ov_filter;
	int ov_sel = 0;
	bool search_global = false;
	std::vector<search_hit> search_hits;
	std::vector<conversation> picker_convos;

	// model picker (defined in app_models.cpp). the list is fetched off-thread and
	// cached to disk; convo_model is a per-conversation override recovered from the
	// last assistant turn's model, and recent_models is the MRU shown on top.
	std::vector<model_info> model_list;
	std::atomic<bool> models_loading{false};
	std::string models_error;
	std::thread models_worker;
	std::vector<std::string> recent_models;
	std::string convo_model;
	void open_models();
	void fetch_models(bool force);
	void save_models_snapshot() const;
	void load_models_snapshot();
	std::vector<const model_info*> filtered_models() const;
	Element models_view();
	bool handle_models(const Event& e);
	void choose_model(const std::string& id);

	// the conversation sidebar (defined in sidebar.cpp). a three-state toggle:
	// hidden, focused (owns the keyboard), or open (visible while you type).
	enum class sidebar_mode : std::uint8_t { hidden, focused, open };
	sidebar_mode sb = sidebar_mode::hidden;
	int sb_cursor = 0;
	std::string sb_filter;
	bool sb_filtering = false;
	bool sb_renaming = false;
	std::string sb_rename_buf;
	int sb_confirm_delete = -1;  // list index awaiting a delete confirmation
	std::vector<conversation> sb_convos;
	void toggle_sidebar();
	void refresh_sidebar();
	std::vector<const conversation*> sidebar_list() const;
	Element sidebar_view();
	bool handle_sidebar(const Event& e);

	// conversation tabs: a browser-style MRU strip cycled with gt/gT. switching a
	// tab is a plain switch_convo; per-tab background streaming is not modelled.
	std::vector<convo_id> tabs;
	bool pending_g_tab = false;  // saw 'g', waiting for t/T (or another g)
	void touch_tab(const convo_id& id) {
		if (std::find(tabs.begin(), tabs.end(), id) == tabs.end()) {
			tabs.push_back(id);
			if (tabs.size() > 6) tabs.erase(tabs.begin());
		}
	}
	void cycle_tab(int dir) {
		if (tabs.size() < 2) return;
		int cur = 0;
		for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
			if (tabs[static_cast<std::size_t>(i)] == convo) cur = i;
		cur = (cur + dir + static_cast<int>(tabs.size())) % static_cast<int>(tabs.size());
		switch_convo(tabs[static_cast<std::size_t>(cur)]);
	}
	Element tabs_strip();

	// inline slash-command autocomplete: a dropdown above the composer while the
	// line is a bare "/partial" with no argument yet.
	int slash_sel = 0;
	std::vector<std::string> slash_matches() const {
		const std::string v = comp.value();
		std::vector<std::string> out;
		if (v.size() < 1 || v[0] != '/' || v.find(' ') != std::string::npos) return out;
		const std::string_view pat = std::string_view(v).substr(1);
		for (const auto& a : kActions)
			if (fuzzy(pat, a.name)) out.emplace_back(a.name);
		return out;
	}
	void accept_slash() {
		const auto m = slash_matches();
		if (m.empty()) return;
		const std::string name =
		    m[static_cast<std::size_t>(std::clamp(slash_sel, 0, int(m.size()) - 1))];
		bool takes_arg = false;
		for (const auto& a : kActions)
			if (name == a.name) takes_arg = a.takes_arg;
		if (takes_arg) {
			comp.set_text("/" + name + " ");
		} else {
			comp.clear();
			run_command(name, "");
		}
		slash_sel = 0;
	}
	Element slash_popup();

	// the settings overlay (defined in settings_ui.cpp): a live-editing list of
	// preferences that persist immediately.
	int settings_sel = 0;
	void open_settings() {
		ov = overlay::settings;
		settings_sel = 0;
	}
	Element settings_view();
	bool handle_settings(const Event& e);
	void cycle_theme(int dir);

	// the cost dashboard (defined in cost_ui.cpp). cost is never stored, so it is
	// summed from token totals priced by cfg.prices when the overlay opens.
	struct cost_stats {
		std::int64_t in = 0, out = 0;
		double usd = 0;
	};
	std::map<std::string, cost_stats> cost_by_model;
	cost_stats cost_convo, cost_all;
	void open_cost();
	Element cost_view();

	// the roles + snippet library (defined in library_ui.cpp).
	void open_roles() {
		ov = overlay::roles;
		ov_filter.clear();
		ov_sel = 0;
	}
	Element roles_view();
	bool handle_roles(const Event& e);
	void save_role(const std::string& name);

	bool snip_fill = false;  // filling {{variables}} of a chosen snippet
	std::string snip_body;
	std::vector<std::string> snip_vars, snip_vals;
	int snip_idx = 0;
	void open_snips() {
		ov = overlay::snippets;
		ov_filter.clear();
		ov_sel = 0;
		snip_fill = false;
	}
	Element snips_view();
	bool handle_snips(const Event& e);

	// phase-8 extras (defined in extras_ui.cpp): a right-click context menu, an
	// /inspect nerd-stats overlay, mouse-capture toggle, and responsive collapse.
	bool mouse_on = true;
	bool ctx_open = false;
	int ctx_x = 0, ctx_y = 0;
	Box root_box_;         // the whole frame's box, reflected each render
	int last_width = 200;  // derived from root_box_, for responsive collapse
	void open_inspect() { ov = overlay::inspect; }
	Element ctx_menu_view();
	Element inspect_view();
	Element picker_view();  // the ctrl-p switcher, with date groups + a preview
	void recover_convo_model() {
		convo_model.clear();
		for (auto it = transcript.rbegin(); it != transcript.rend(); ++it)
			if (it->role == role::assistant && !it->model.empty()) {
				convo_model = it->model;
				break;
			}
	}

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
		if (models_worker.joinable()) models_worker.join();
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
		if (!convo_model.empty()) return convo_model;  // per-conversation override
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

	std::string node_text(const node& n) const {
		std::string out;
		if (auto blocks = codec::decode_blocks(n.content_json))
			for (const auto& b : *blocks)
				if (const auto* t = std::get_if<text_block>(&b)) out += t->text;
		return out;
	}

	// decoded image previews, memoized by path so the transcript can render an
	// attachment inline without re-decoding every frame. a zero-size entry marks
	// a file that failed to decode, so we don't keep retrying it.
	std::map<std::string, img::bitmap> image_cache;
	const img::bitmap* preview(const std::string& path) {
		if (auto it = image_cache.find(path); it != image_cache.end())
			return it->second.width > 0 ? &it->second : nullptr;
		if (auto bm = img::decode(path); bm && bm->width > 0) {
			auto [it, _] = image_cache.emplace(path, std::move(*bm));
			return &it->second;
		}
		image_cache.emplace(path, img::bitmap{});
		return nullptr;
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
	void spawn_siblings(const node_id& anchor, int k);

	void finish_sibling(sibling_stream* s, const result<completion>& out);

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
		scroll_tail();
		recover_convo_model();
		touch_tab(convo);
	}

	void switch_convo(const convo_id& id) {
		convo = id;
		if (auto c = db->conversation_of(id)) convo_title = c->title;
		system_prompt.clear();
		compaction_summary.clear();
		compaction_boundary = 0;
		reload_transcript();
		scroll_tail();
		recover_convo_model();
		touch_tab(convo);
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
						out += "<section><h3>" + std::string(to_string(n.role)) + "</h3><pre>" +
						       body + "</pre></section>\n";
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
		recover_convo_model();
		ov = overlay::none;
	}

	void run_command(const std::string& name, const std::string& arg);

	// resume a turn cut short by max_tokens: nudge the model to keep going.
	void continue_turn() {
		if (streaming || transcript.empty()) return;
		truncated = false;
		send("continue.");
	}

	// sampling-param setters, /params summary and /attach live in app_tools.cpp.
	bool set_param(const std::string& name, const std::string& arg);
	std::string params_summary() const;
	void attach(const std::string& path);

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
				const std::string badge = c.source == "claude-export" ? "↪ claude.ai"
				                          : c.source == "claude-live" ? "↪ live mirror"
				                                                      : "";
				items.emplace_back(title, badge);
			}
		} else if (ov == overlay::search) {
			for (const auto& h : search_hits)
				items.emplace_back(h.snippet, h.node.str().substr(0, 12));
		}
		return items;
	}

	Element overlay_view();  // defined in app_render.cpp

	bool handle_overlay(const Event& e);

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
	void send(const std::string& raw);

	void finish_stream(const result<completion>& out, const node_id& parent);

	// ring the terminal (or fire an OSC 9 desktop notification) when a turn lands,
	// so a backgrounded plume can nudge you. off by default-respecting config.
	void notify_done() {
		if (cfg.notify == "off") return;
		std::string seq = "\a";  // bell
		if (cfg.notify == "osc9") seq = "\x1b]9;plume: your turn\x07";
		std::fwrite(seq.data(), 1, seq.size(), stdout);
		std::fflush(stdout);
	}

	// -- mcp tool-use loop (defined in app_tools.cpp) -------------------------
	std::vector<tool_def> tool_defs() const;
	bool allowed(const pending_tool&) const;
	void advance_tools();
	void run_tool(const pending_tool&);
	void submit_tool_results();
	void stream_reply(const node_id& parent);
	Element tool_args_table(const std::string& args_json) const;

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

	// defined out-of-line in app_render.cpp.
	double cost_of(const usage& u) const;
	Element header();
	Element statusbar();
	Element transcript_view();
	Element weave_view();
	Element spawn_view();

	node_id weave_selected() const {
		if (weave_cursor >= 0 && weave_cursor < static_cast<int>(weave_order.size()))
			return weave_order[static_cast<std::size_t>(weave_cursor)];
		return {};
	}

	// the weave keymap and the compare view are defined in app_input.cpp.
	bool handle_weave(const Event& e);
	Element compare_view();
	void maybe_autoweave(const node_id& assistant_leaf);
};

}  // namespace plume
