// the app class surface: construction, the first-run wizard trigger, and the
// event loop. the impl struct and rendering live in app_impl.hpp.
#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

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
	s.recover_convo_model();  // reflect the thread's last model in the pill
	s.touch_tab(s.convo);
	s.web_on = s.cfg.web_search;  // persistable default for the web search tool
	if (s.cfg.ui.sidebar) {       // open (visible, unfocused) so you can still type
		s.sb = impl::sidebar_mode::open;
		s.refresh_sidebar();
	}

	// build the provider if one is configured and reachable.
	if (std::getenv("PLUME_MOCK")) {
		s.prov = make_mock();
		s.cfg.default_provider = "mock";
		s.cfg.providers["mock"] = {"mock", "", "env", "", "mock-model"};
	} else if (const provider_entry* pe = s.cfg.active_provider()) {
		if (auto p = make_provider(pc_from(*pe))) s.prov = std::move(*p);
	} else if (const char* key = std::getenv("ANTHROPIC_API_KEY"); key && *key) {
		// zero-config: an env key gives a working provider. on a first run the
		// wizard still shows (below) and can adopt it; after that this is chat.
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

	// mcp: connect every declared server and surface its tools to the model. a
	// server that fails to start is logged and skipped, never fatal.
	if (!s.cfg.mcp.empty()) {
		if (auto mc = mcp_client::create()) {
			s.mcp = std::move(*mc);
			for (const auto& sv : s.cfg.mcp) static_cast<void>(s.mcp->connect(sv));
			if (auto ts = s.mcp->tools()) s.mcp_tools = std::move(*ts);
		}
	}

	// show the wizard on a forced `plume setup`, or (outside mock mode) whenever
	// nothing is configured or this is a first run. a first run is the absence of
	// a config file: §231 wants first launch to land in the wizard even when an
	// env key could otherwise auto-configure a provider behind your back.
	const bool first_run = !std::filesystem::exists(s.cfg.config_dir + "/config.toml");
	if (std::getenv("PLUME_WIZARD") || (!std::getenv("PLUME_MOCK") && (!s.prov || first_run)))
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
		s.regions_.clear();  // rebuild the mouse hit-test table for this frame
		if (s.root_box_.x_max >= s.root_box_.x_min)
			s.last_width = s.root_box_.x_max - s.root_box_.x_min + 1;
		if (s.wiz.active) {
			const bool dark = s.caps.background ? s.caps.dark : true;
			return s.wiz.render(s.wiz.preview_theme(dark), now_ms());
		}
		if (s.comparing) return vbox({s.header(), s.compare_view() | flex, s.statusbar()});
		const bool loom = s.spawning || s.spawn_done;
		Element body = loom ? s.spawn_view() : (s.in_weave ? s.weave_view() : s.transcript_view());
		if (s.tabs.size() >= 2 && !loom)  // a browser-style tab strip above the body
			body = vbox({s.tabs_strip(), separator() | color(col(s.th.p.hl_low)), body | flex});
		Element input = (s.in_weave || loom) ? hbox({filler()}) : s.comp.render(s.th);
		Element main = (!s.in_weave && !loom && !s.slash_matches().empty())
		                   ? vbox({body | flex, s.slash_popup(), input})
		                   : vbox({body | flex, input});
		// the sidebar wraps the main column (header/statusbar span full width). the
		// loom needs the room and narrow terminals can't spare it, so the sidebar
		// steps aside while branches stream or below ~76 columns (responsive).
		Element mid = main;
		if (s.sb != impl::sidebar_mode::hidden && !loom && s.last_width >= 76)
			mid = hbox({s.sidebar_view() | size(WIDTH, EQUAL, 30),
			            separator() | color(col(s.th.p.hl_med)), main | flex});
		Element view = vbox({s.header(), mid | flex, s.statusbar()});
		if (s.ov != impl::overlay::none) view = dbox({view, s.overlay_view()});
		if (s.ctx_open) view = dbox({view, s.ctx_menu_view()});
		return view | reflect(s.root_box_);  // capture width for responsiveness
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
		if (e.is_mouse()) {
			Event ev = e;  // mouse() is non-const; the caught event is const
			return s.handle_mouse(ev.mouse());
		}
		if (s.ctx_open) {  // any key dismisses the context menu
			s.ctx_open = false;
			if (e == Event::Escape) return true;
		}
		s.toast.clear();  // any keypress dismisses a transient note
		// the compare view owns the keyboard until dismissed.
		if (s.comparing) {
			if (e == Event::Escape || e == Event::Character("q")) {
				s.comparing = false;
				s.cmp_a = {};
				s.cmp_b = {};
			}
			return true;
		}
		// an open overlay owns the keyboard.
		if (s.ov != impl::overlay::none) return s.handle_overlay(e);
		// global overlays. ftxui delivers control keys as named events (like the
		// Event::CtrlC above); the raw-byte form never matched, so use the names.
		if (e == Event::CtrlK) {  // command palette
			s.ov = impl::overlay::palette;
			s.ov_filter.clear();
			s.ov_sel = 0;
			return true;
		}
		if (e == Event::CtrlP && s.db) {  // conversation picker
			s.ov = impl::overlay::picker;
			s.ov_filter.clear();
			s.ov_sel = 0;
			if (auto list = s.db->conversations()) s.picker_convos = *list;
			return true;
		}
		if (e == Event::CtrlF) {  // search everything
			s.run_search("", true);
			return true;
		}
		if (e == Event::CtrlL) {  // model picker
			s.open_models();
			return true;
		}
		if (e == Event::CtrlB) {  // toggle the conversation sidebar
			s.toggle_sidebar();
			return true;
		}
		if (e == Event::F1) {  // help, from any mode
			s.ov = impl::overlay::cheatsheet;
			return true;
		}
		if (e == Event::CtrlT) {  // snippet library
			s.open_snips();
			return true;
		}
		// a focused sidebar owns the keyboard (open-but-unfocused lets you type).
		if (s.sb == impl::sidebar_mode::focused) return s.handle_sidebar(e);
		// the loom owns the keyboard while branches stream.
		if (s.spawning || s.spawn_done) {
			if (s.spawning) {
				if (e == Event::Escape) s.spawn_stop = true;
			} else {
				s.siblings.clear();
				s.spawn_done = false;
				s.in_weave = true;  // back to the tree to adopt a branch
			}
			return true;
		}
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
		// '?' in normal mode opens the cheatsheet (insert mode types it).
		if (!s.comp.insert_mode() && e == Event::Character("?")) {
			s.ov = impl::overlay::cheatsheet;
			return true;
		}
		// transcript scrollback: in normal mode with an empty composer, motion keys
		// walk the message cursor rather than typing. G/End re-pin the live tail.
		if (!s.comp.insert_mode() && s.comp.value().empty()) {
			if (s.pending_g && e == Event::Character("t"))
				return s.pending_g = false, s.cycle_tab(1), true;
			if (s.pending_g && e == Event::Character("T"))
				return s.pending_g = false, s.cycle_tab(-1), true;
			if (e == Event::Character("g")) {
				if (s.pending_g) {
					s.scroll_top();
					s.pending_g = false;
				} else {
					s.pending_g = true;
				}
				return true;
			}
			s.pending_g = false;
			if (e == Event::Character("k") || e == Event::ArrowUp)
				return s.scroll_transcript(-1), true;
			if (e == Event::Character("j") || e == Event::ArrowDown)
				return s.scroll_transcript(1), true;
			if (e == Event::PageUp || e == Event::CtrlU) return s.scroll_transcript(-4), true;
			if (e == Event::PageDown || e == Event::CtrlD) return s.scroll_transcript(4), true;
			if (e == Event::Character("G") || e == Event::End) return s.scroll_tail(), true;
			if (e == Event::Home) return s.scroll_top(), true;
			if (e == Event::Escape && !s.follow_tail) return s.scroll_tail(), true;
			// message actions on the selected turn.
			if (!s.follow_tail && s.transcript_sel >= 0)
				for (char a : {'y', 'c', 'e', 'r', 'b', 'q', 'x'})
					if (e == Event::Character(std::string(1, a))) return s.message_action(a), true;
		}
		// slash-command autocomplete: cycle and accept the dropdown suggestions.
		if (const auto sm = s.slash_matches(); !sm.empty()) {
			const int n = static_cast<int>(sm.size());
			if (e == Event::Tab || e == Event::ArrowDown || e == Event::CtrlN)
				return s.slash_sel = (s.slash_sel + 1) % n, true;
			if (e == Event::ArrowUp || e == Event::CtrlP)
				return s.slash_sel = (s.slash_sel + n - 1) % n, true;
			if (e == Event::Return) return s.accept_slash(), true;
		}
		// recall previously sent lines with the arrows on an empty composer.
		if (s.comp.insert_mode() && s.comp.value().empty()) {
			if (e == Event::ArrowUp) return s.recall_history(-1), true;
			if (e == Event::ArrowDown) return s.recall_history(1), true;
		}
		// retry the newest turn if it errored (R in normal mode, or the chip).
		if (!s.comp.insert_mode() && e == Event::Character("R") && !s.transcript.empty() &&
		    s.transcript.back().state == node_state::error) {
			s.retry_last();
			return true;
		}
		// chat: the modal composer handles every key.
		switch (s.comp.handle(e)) {
			case composer::result::submit: {
				const std::string t = s.comp.value();
				if (t.empty()) return true;
				s.comp.clear();
				if (t.front() == '/')
					s.run_slash(t);  // a slash command
				else
					s.send(t);
				return true;
			}
			case composer::result::to_editor: s.edit_in_editor(); return true;
			case composer::result::none: return true;
		}
		return true;
	});

	s.screen.Loop(with_keys);
	return 0;
}

}  // namespace plume
