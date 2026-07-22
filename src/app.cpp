// the ui shell. the ftxui screen owns the terminal; every provider call runs on
// a worker thread and hands results back through screen.Post, which marshals
// onto the render loop and asks for a frame. nothing here writes to stdout
// directly. the linear chat view is the weave engine's active path; ctrl-w opens
// the tree.
#include "plume/app.hpp"

#include <atomic>
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

	plume::composer comp;
	bool in_weave = false;
	int weave_cursor = 0;
	std::vector<node_id> weave_order;
	node_id graft_source;    // set by the first 'g', consumed by the second
	std::string weave_note;  // transient feedback in the weave detail line

	wizard wiz;
	std::string convo_title = "first light";
	std::int64_t context_window = 200000;

	// the animation heartbeat: posts a frame while something is moving, unless
	// motion is off.
	std::thread ticker;
	std::atomic<bool> alive{true};

	explicit impl(config c) : cfg(std::move(c)) {}
	~impl() {
		stop_flag = true;
		alive = false;
		if (worker.joinable()) worker.join();
		if (ticker.joinable()) ticker.join();
	}

	bool has_provider() const { return prov != nullptr; }
	bool reduce_motion() const { return cfg.ui.reduce_motion; }
	bool compact() const { return cfg.ui.density == "compact"; }
	bool animating() const { return streaming.load() || wiz.animating(); }

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
		if (streaming)
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
		s.convo_title = list->front().title;
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
		if (auto p = make_provider(pc_from(*pe))) s.prov = std::move(*p);
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

	// nothing configured (and no env key), or a forced `plume setup`: run the wizard.
	if ((!s.prov && !std::getenv("PLUME_MOCK")) || std::getenv("PLUME_WIZARD"))
		s.wiz.begin(s.cfg, s.caps, now_ms());

	return a;
}

int app::run() {
	impl& s = *pimpl_;

	// the animation heartbeat: a frame every 60ms while something moves, unless
	// motion is off. idle chat costs nothing.
	s.ticker = std::thread([&s] {
		while (s.alive.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(60));
			if (s.alive.load() && s.animating() && !s.reduce_motion())
				s.screen.PostEvent(Event::Custom);
		}
	});

	// no ftxui Input: the composer is a hand-rolled modal editor, so the whole
	// keyboard is routed by hand.
	auto root = Renderer([&] {
		if (s.wiz.active) {
			const bool dark = s.caps.background ? s.caps.dark : true;
			return s.wiz.render(s.wiz.preview_theme(dark), now_ms());
		}
		Element body = s.in_weave ? s.weave_view() : s.transcript_view();
		Element input = s.in_weave ? hbox({filler()}) : s.comp.render(s.th);
		return vbox({s.header(), body | flex, input, s.statusbar()});
	});

	auto with_keys = CatchEvent(root, [&](const Event& e) {
		if (e == Event::CtrlC) {
			s.screen.ExitLoopClosure()();
			return true;
		}
		// the wizard owns the keyboard while it runs.
		if (s.wiz.active) {
			const bool handled = s.wiz.handle(e, s.screen);
			if (s.wiz.finished) s.apply_wizard();
			return handled;
		}
		if (e == Event::Custom) return true;  // a worker asked for a frame
		if (e == Event::CtrlW) {
			s.in_weave = !s.in_weave;
			s.weave_note.clear();
			return true;
		}
		if (e == Event::Escape && s.streaming) {
			s.stop_flag = true;
			return true;
		}
		// the weave view fully owns the keyboard so keys never leak to the composer.
		if (s.in_weave) {
			if (e == Event::Character("q")) {
				s.screen.ExitLoopClosure()();
				return true;
			}
			s.handle_weave(e);
			return true;
		}
		// chat: the modal composer handles every key.
		switch (s.comp.handle(e)) {
			case composer::result::submit:
				if (!s.comp.value().empty()) {
					const std::string t = s.comp.value();
					s.comp.clear();
					s.send(t);
				}
				return true;
			case composer::result::to_editor: s.edit_in_editor(); return true;
			case composer::result::none: return true;
		}
		return true;
	});

	s.screen.Loop(with_keys);
	return 0;
}

}  // namespace plume
