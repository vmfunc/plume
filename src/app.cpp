// the ui shell. the ftxui screen owns the terminal; every provider call runs on
// a worker thread and hands results back through screen.Post, which marshals
// onto the render loop and asks for a frame. nothing here writes to stdout
// directly. the linear chat view is the weave engine's active path; ctrl-w opens
// the tree.
#include "plume/app.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <set>

#include "plume/codec.hpp"
#include "plume/plugin.hpp"
#include "plume/provider.hpp"
#include "plume/store.hpp"
#include "plume/terminal.hpp"
#include "plume/theme.hpp"
#include "plume/weave.hpp"

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

}  // namespace

// defined in provider/mock.cpp.
std::unique_ptr<provider> make_mock();

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
	usage last_usage;
	std::int64_t ttft_ms = 0;
	std::thread worker;

	std::string composer;
	bool in_weave = false;
	int weave_cursor = 0;
	std::vector<node_id> weave_order;

	explicit impl(config c) : cfg(std::move(c)) {}
	~impl() {
		stop_flag = true;
		if (worker.joinable()) worker.join();
	}

	bool has_provider() const { return prov != nullptr; }

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

		// build the request from the active path.
		request req;
		req.params = cfg.defaults;
		req.params.model = model_id();
		if (req.params.max_tokens < 1024) req.params.max_tokens = 4096;
		for (const auto& n : transcript) {
			auto blocks = codec::decode_blocks(n.content_json);
			if (blocks) req.messages.push_back(message{n.role, *blocks});
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

	// -- rendering ------------------------------------------------------------

	double cost_of(const usage& u) const {
		if (auto it = cfg.prices.find(model_id()); it != cfg.prices.end())
			return (u.input * it->second.input + u.output * it->second.output +
			        u.cache_read * it->second.cache_read +
			        u.cache_creation * it->second.cache_write) /
			       1e6;
		return 0.0;
	}

	Element statusline() {
		usage tot = last_usage;
		std::string cost = std::to_string(cost_of(tot));
		cost = cost.substr(0, cost.find('.') + 5);
		auto seg = [&](const std::string& s, rgb c) {
			return hbox({text(" " + s + " ") | color(col(c))});
		};
		Elements line = {
		    seg(model_id(), th.p.iris),
		    text("· ") | color(col(th.p.muted)),
		    seg("in " + std::to_string(tot.input) + " out " + std::to_string(tot.output),
		        th.p.foam),
		    text("· ") | color(col(th.p.muted)),
		    seg("$" + cost, th.p.gold),
		    text("· ") | color(col(th.p.muted)),
		    seg(std::to_string(ttft_ms) + "ms", th.p.pine),
		};
		// statusline segments contributed by plugins.
		if (plugins)
			for (const auto& s : plugins->statusline()) line.push_back(seg(s.text, th.p.rose));
		line.push_back(filler());
		line.push_back(streaming ? text("streaming ") | color(col(th.p.rose)) | blink : text(""));
		line.push_back(text(in_weave ? "weave " : "chat ") | color(col(th.p.subtle)));
		return hbox(std::move(line)) | bgcolor(col(th.p.surface));
	}

	Element render_node(const node& n) {
		auto blocks = codec::decode_blocks(n.content_json);
		std::string body = blocks ? message{n.role, *blocks}.plain_text() : n.content_json;
		rgb accent = n.role == role::user ? th.p.pine : th.p.foam;
		Elements lines;
		lines.push_back(text(std::string(to_string(n.role))) | bold | color(col(accent)));
		lines.push_back(paragraph(body) | color(col(th.p.text)));
		if (blocks)
			for (const auto& b : *blocks)
				if (const auto* th_blk = std::get_if<thinking_block>(&b); th_blk && show_think)
					lines.push_back(paragraph("thinking: " + th_blk->thinking) |
					                color(col(th.p.muted)) | dim);
		return vbox(std::move(lines)) | border | color(col(th.p.hl_med));
	}

	Element transcript_view() {
		Elements out;
		for (const auto& n : transcript) out.push_back(render_node(n));
		if (streaming || !live_text.empty() || !live_think.empty()) {
			Elements live;
			live.push_back(text("assistant") | bold | color(col(th.p.foam)));
			if (show_think && !live_think.empty())
				live.push_back(paragraph("thinking: " + live_think) | color(col(th.p.muted)) | dim);
			live.push_back(paragraph(live_text + (streaming ? "▌" : "")) | color(col(th.p.text)));
			out.push_back(vbox(std::move(live)) | border | color(col(th.p.iris)));
		}
		if (out.empty())
			out.push_back(text("plume is ready. ? for keys, / to talk.") | color(col(th.p.muted)) |
			              center);
		if (!status_error.empty())
			out.push_back(text("error: " + status_error) | color(col(th.p.love)));
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
				std::string indent(static_cast<std::size_t>(tn.depth) * 2, ' ');
				std::string label = codec::read_str(tn.data.params_json, "label").value_or("");
				bool mark = codec::read_bool(tn.data.params_json, "bookmark").value_or(false);
				rgb c = tn.data.role == role::user ? th.p.pine : th.p.foam;
				std::string line =
				    indent + (mark ? "★ " : "• ") + std::string(to_string(tn.data.role));
				if (!label.empty()) line += " [" + label + "]";
				Element e = text(line) | color(col(c));
				if (i == weave_cursor) e = e | bgcolor(col(th.p.hl_high)) | bold | focus;
				rows.push_back(e);
			}
		}
		if (rows.empty()) rows.push_back(text("(empty)") | color(col(th.p.muted)));
		Element tree = vbox(std::move(rows)) | yframe | flex;
		Element detail =
		    text(
		        "hjkl move · enter adopt · s siblings · p prune · P restore · L label · "
		        "m bookmark · x export dot · ctrl-w back") |
		    color(col(th.p.subtle));
		return vbox({text("weave") | bold | color(col(th.p.iris)), separator(), tree, separator(),
		             detail}) |
		       border | color(col(th.p.hl_med)) | flex;
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
		if (e == Event::Character("m")) {
			static_cast<void>(w.set_bookmark(sel, !w.bookmarked(sel).value_or(false)));
			return true;
		}
		return false;
	}
};

app::app() = default;
app::app(app&&) noexcept = default;
app& app::operator=(app&&) noexcept = default;
app::~app() = default;

result<app> app::create(config cfg) {
	app a;
	a.pimpl_ = std::make_unique<impl>(std::move(cfg));
	impl& s = *a.pimpl_;

	s.caps = term::probe();
	const bool dark = s.caps.background ? s.caps.dark : true;
	if (auto th = resolve_theme(s.cfg.ui.theme, s.cfg.config_dir + "/themes", dark))
		s.th = *th;
	else
		s.th = rose_pine();

	if (auto db = store::open(s.cfg.data_dir + "/plume.sqlite"))
		s.db = std::move(*db);
	else
		return fail(db.error().code, db.error().detail);

	// resolve or create the working conversation.
	if (auto list = s.db->conversations(); list && !list->empty()) {
		s.convo = list->front().id;
	} else {
		conversation c;
		c.id = convo_id{new_id("convo")};
		c.title = "first light";
		c.created_at = now_ms();
		static_cast<void>(s.db->put_conversation(c));
		s.convo = c.id;
	}
	s.reload_transcript();

	// build the provider if one is configured and reachable.
	if (std::getenv("PLUME_MOCK")) {
		s.prov = make_mock();
		s.cfg.default_provider = "mock";
		s.cfg.providers["mock"] = {"mock", "", "env", "", "mock-model"};
	} else if (const provider_entry* pe = s.cfg.active_provider()) {
		provider_config pc;
		pc.kind = pe->kind;
		pc.base_url = pe->base_url;
		pc.credential.value = pe->auth_value;
		if (pe->auth_source == "key_cmd")
			pc.credential.kind = auth::source::key_cmd;
		else if (pe->auth_source == "keychain")
			pc.credential.kind = auth::source::keychain;
		else if (pe->auth_source == "inline")
			pc.credential.kind = auth::source::inline_key;
		if (auto p = make_provider(pc)) s.prov = std::move(*p);
	} else if (const char* key = std::getenv("ANTHROPIC_API_KEY"); key && *key) {
		// zero-config: an env key drops you straight into a working chat.
		provider_config pc;
		pc.kind = "anthropic";
		pc.credential.kind = auth::source::env;
		pc.credential.value = "ANTHROPIC_API_KEY";
		if (auto p = make_provider(pc)) {
			s.prov = std::move(*p);
			s.cfg.default_provider = "anthropic";
			s.cfg.providers["anthropic"] = {"anthropic", "", "env", "ANTHROPIC_API_KEY", ""};
		}
	}

	// plugin host. capabilities are denied by default; a user grants them by
	// listing them in PLUME_PLUGIN_CAPS (interactive per-plugin approval is a
	// refinement — see docs/plugins.md).
	if (auto ph = plugin_host::create()) {
		s.plugins = std::move(*ph);
		impl* sp = &s;
		s.plugins->set_model(
		    [sp](const std::string& prompt, const std::string& model) -> std::string {
			    if (!sp->prov) return {};
			    request r;
			    r.params = sp->cfg.defaults;
			    r.params.model = model.empty() ? sp->model_id() : model;
			    r.params.thinking = thinking_mode::off;
			    r.messages.push_back(message::text(role::user, prompt));
			    std::string out;
			    auto res = sp->prov->stream(
			        r,
			        [&out](const stream_delta& d) {
				        if (d.type == stream_delta::kind::text) out += d.text;
			        },
			        [] { return false; });
			    return res ? out : std::string{};
		    });

		std::set<std::string> granted;
		if (const char* caps = std::getenv("PLUME_PLUGIN_CAPS")) {
			std::string c(caps), tok;
			for (char ch : c) {
				if (ch == ',') {
					if (!tok.empty()) granted.insert(tok);
					tok.clear();
				} else {
					tok.push_back(ch);
				}
			}
			if (!tok.empty()) granted.insert(tok);
		}
		auto approve = [granted](const std::string&, const std::string& cap) {
			return granted.contains(cap);
		};
		static_cast<void>(s.plugins->load_all(s.cfg.config_dir + "/plugins", approve));
	}

	return a;
}

int app::run() {
	impl& s = *pimpl_;

	auto composer = Input(&s.composer, "type / to talk, ? for keys");
	auto root = Renderer(composer, [&] {
		Element body = s.in_weave ? s.weave_view() : s.transcript_view();
		Element wizard;
		if (!s.has_provider()) {
			wizard = vbox({
			             text("plume — first run") | bold | color(col(s.th.p.iris)),
			             separator(),
			             text("terminal report card") | color(col(s.th.p.gold)),
			             text("  truecolor  " + std::string(s.caps.truecolor ? "yes" : "no")),
			             text("  kitty gfx  " + std::string(s.caps.kitty_graphics ? "yes" : "no")),
			             text("  sixel      " + std::string(s.caps.sixel ? "yes" : "no")),
			             text("  osc52      " + std::string(s.caps.osc52 ? "yes" : "no")),
			             separator(),
			             text("no provider configured.") | color(col(s.th.p.love)),
			             text("set ANTHROPIC_API_KEY and restart, or edit "),
			             text("  " + s.cfg.config_dir + "/config.toml") | color(col(s.th.p.foam)),
			             text("press q to quit."),
			         }) |
			         border | center;
		}
		Element main = wizard ? wizard : body;
		return vbox({
		    s.statusline(),
		    main | flex,
		    hbox({text("› ") | color(col(s.th.p.iris)), composer->Render() | flex}) |
		        bgcolor(col(s.th.p.surface)),
		});
	});

	auto with_keys = CatchEvent(root, [&](const Event& e) {
		if (e == Event::Custom) return true;  // a worker asked for a frame
		// q quits only when not typing into the composer (weave view or wizard).
		const bool typing = s.has_provider() && !s.in_weave;
		if (e == Event::CtrlC || (!typing && e == Event::Character("q"))) {
			s.screen.ExitLoopClosure()();
			return true;
		}
		if (e == Event::CtrlW && s.has_provider()) {
			s.in_weave = !s.in_weave;
			return true;
		}
		if (e == Event::Escape && s.streaming) {
			s.stop_flag = true;
			return true;
		}
		if (s.in_weave) return s.handle_weave(e);
		// chat mode: enter sends.
		if (e == Event::Return && !s.composer.empty()) {
			const std::string t = s.composer;
			s.composer.clear();
			s.send(t);
			return true;
		}
		return false;  // let the composer input handle the rest
	});

	s.screen.Loop(with_keys);
	return 0;
}

}  // namespace plume
