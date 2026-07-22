#include "wizard.hpp"

#include <array>
#include <chrono>
#include <cstdlib>

#include <ftxui/component/screen_interactive.hpp>

#include "plume/provider.hpp"
#include "ui.hpp"

namespace plume {

// defined in provider/mock.cpp.
std::unique_ptr<provider> make_mock();

using namespace ftxui;

namespace {

struct prov_choice {
	const char* label;
	const char* kind;
	const char* env;       // default env var, empty when none
	const char* base_url;  // empty for the backend default
};

constexpr std::array<prov_choice, 5> kProviders = {{
    {"anthropic", "anthropic", "ANTHROPIC_API_KEY", ""},
    {"openai", "openai", "OPENAI_API_KEY", ""},
    {"openrouter", "openrouter", "OPENROUTER_API_KEY", ""},
    {"ollama (local)", "ollama", "", "http://localhost:11434/v1"},
    {"openai-compatible", "openai-compatible", "", ""},
}};

constexpr std::array<const char*, 4> kAuthLabels = {"environment variable", "paste a key",
                                                    "key command", "os keychain"};
constexpr std::array<const char*, 4> kAuthSource = {"env", "inline", "key_cmd", "keychain"};
constexpr std::array<const char*, 4> kThemeNames = {"rose-pine", "rose-pine-moon", "rose-pine-dawn",
                                                    "va11"};

std::int64_t now_ms() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

auto C(rgb c) {
	return color(ui::col(c));
}

// a picker row: a marker plus a label, highlighted when selected.
Element pick_row(const theme& th, const std::string& label, bool selected, bool active_col) {
	const rgb marker_c = selected ? (active_col ? th.p.iris : th.p.subtle) : th.p.muted;
	Element row = hbox({text(selected ? "  ▸ " : "    ") | C(marker_c),
	                    text(label) | C(selected ? th.p.text : th.p.subtle)});
	if (selected && active_col) row = row | bgcolor(ui::col(th.p.hl_low));
	return row;
}

Element step_dots(const theme& th, int at, int total) {
	Elements dots;
	for (int i = 0; i < total; ++i) {
		const rgb c = i == at ? th.p.iris : (i < at ? th.p.foam : th.p.muted);
		dots.push_back(text(i == at ? "●" : "○") | C(c));
		if (i + 1 < total) dots.push_back(text(" ") | C(th.p.muted));
	}
	return hbox(std::move(dots));
}

Element shell(const theme& th, int at, const std::string& title, Element body,
              const std::string& hint) {
	Element card = vbox({
	                   hbox({text("plume") | bold | C(th.p.iris), filler(), step_dots(th, at, 6)}),
	                   separator() | C(th.p.hl_med),
	                   text(title) | bold | C(th.p.gold),
	                   text(""),
	                   body,
	                   text(""),
	                   separator() | C(th.p.hl_med),
	                   text(hint) | C(th.p.subtle) | dim,
	               }) |
	               borderRounded | C(th.p.hl_high) | size(WIDTH, GREATER_THAN, 56) |
	               size(WIDTH, LESS_THAN, 92);
	return vbox({filler(), hbox({filler(), card, filler()}), filler()}) |
	       bgcolor(ui::col(th.p.base));
}

}  // namespace

wizard::~wizard() {
	if (val_thread.joinable()) val_thread.join();
}

void wizard::begin(const config& base, term::capabilities c, std::int64_t ms) {
	result = base;
	caps = c;
	at = step::caps;
	active = true;
	finished = false;
	entered_ms = ms;
}

theme wizard::preview_theme(bool dark_bg) const {
	switch (theme_pick) {
		case 1: return rose_pine_moon();
		case 2: return rose_pine_dawn();
		case 3: return va11();
		default: return dark_bg ? rose_pine() : rose_pine_dawn();
	}
}

// -- rendering ----------------------------------------------------------------

namespace {

Element caps_body(const theme& th, const term::capabilities& caps, std::int64_t since,
                  std::int64_t ms) {
	struct row {
		const char* name;
		bool ok;
	};
	const std::array<row, 5> rows = {{{"truecolor", caps.truecolor},
	                                  {"kitty graphics", caps.kitty_graphics},
	                                  {"sixel", caps.sixel},
	                                  {"italics", caps.italics},
	                                  {"clipboard (osc 52)", caps.osc52}}};
	Elements out;
	for (std::size_t i = 0; i < rows.size(); ++i) {
		// reveal one line at a time.
		if (ms - since < static_cast<std::int64_t>(i) * 110) break;
		const auto& r = rows[i];
		out.push_back(hbox({text(r.ok ? "  ● " : "  ○ ") | C(r.ok ? th.p.foam : th.p.muted),
		                    text(r.name) | C(th.p.text) | size(WIDTH, EQUAL, 22),
		                    text(r.ok ? "yes" : "no") | C(r.ok ? th.p.pine : th.p.muted)}));
	}
	if (ms - since >= 5 * 110) {
		const std::string bg = caps.background ? (caps.dark ? "dark" : "light") : "unknown";
		out.push_back(text(""));
		out.push_back(hbox({text("  background  ") | C(th.p.muted),
		                    text(bg) | C(caps.dark ? th.p.iris : th.p.gold)}));
	}
	return vbox(std::move(out));
}

Element provider_body(const wizard& w, const theme& th, std::int64_t ms) {
	if (w.psub == 0) {
		Elements rows;
		for (std::size_t i = 0; i < kProviders.size(); ++i)
			rows.push_back(
			    pick_row(th, kProviders[i].label, static_cast<int>(i) == w.provider_pick, true));
		return vbox(std::move(rows));
	}
	if (w.psub == 1) {
		Elements rows;
		for (std::size_t i = 0; i < kAuthLabels.size(); ++i)
			rows.push_back(pick_row(th, kAuthLabels[i], static_cast<int>(i) == w.auth_pick, true));
		return vbox({text("  " + std::string(kProviders[w.provider_pick].label)) | C(th.p.foam),
		             text(""), vbox(std::move(rows))});
	}
	// psub 2: enter the key/value + validate.
	std::string shown = w.key_input;
	if (w.auth_pick == 1) {  // mask a pasted key
		shown.clear();
		for (std::size_t i = 0; i < w.key_input.size(); ++i) shown += "•";
	}
	Element field =
	    hbox({text("  " + shown) | C(th.p.text), w.editing ? ui::caret(th, ms, false) : text("")});
	Elements out = {text("  " + std::string(kAuthLabels[w.auth_pick])) | C(th.p.foam), text(""),
	                field | bgcolor(ui::col(th.p.surface)) | size(WIDTH, GREATER_THAN, 40)};
	if (w.validating.load())
		out.push_back(hbox({text("  " + ui::spinner(ms) + " ") | C(th.p.gold),
		                    text("checking...") | C(th.p.subtle)}));
	else if (w.validated.load() == 1)
		out.push_back(text("  ● " + w.validate_note) | C(th.p.foam));
	else if (w.validated.load() == -1)
		out.push_back(text("  ○ " + w.validate_note) | C(th.p.love));
	return vbox(std::move(out));
}

Element theme_body(const wizard& w, const theme& th) {
	Elements rows;
	for (std::size_t i = 0; i < kThemeNames.size(); ++i)
		rows.push_back(pick_row(th, kThemeNames[i], static_cast<int>(i) == w.theme_pick, true));
	Element picker = vbox(std::move(rows)) | size(WIDTH, EQUAL, 20);

	// a live preview of a real exchange, in the picked theme.
	Element preview =
	    vbox({ui::message_card(th, role::user, "what is a quill", {}, false, true, "", 0),
	          ui::message_card(th, role::assistant,
	                           "a pen cut from a feather.\nink, and a steady hand.", {}, false,
	                           true, "", 0)}) |
	    borderRounded | C(th.p.hl_med) | flex;
	return hbox({picker, text("  "), preview | flex});
}

Element keys_body(const wizard& w, const theme& th) {
	auto toggle = [&](const std::string& name, const std::string& a, const std::string& b, int val,
	                  bool focused) {
		Element chip_a = ui::pill(th, a, val == 0 ? th.p.base : th.p.subtle);
		Element chip_b = ui::pill(th, b, val == 1 ? th.p.base : th.p.subtle);
		if (val == 0) chip_a = text(" " + a + " ") | C(th.p.base) | bgcolor(ui::col(th.p.iris));
		if (val == 1) chip_b = text(" " + b + " ") | C(th.p.base) | bgcolor(ui::col(th.p.iris));
		return hbox({text(focused ? "  ▸ " : "    ") | C(focused ? th.p.iris : th.p.muted),
		             text(name) | C(th.p.text) | size(WIDTH, EQUAL, 12), chip_a, text(" "),
		             chip_b});
	};
	return vbox({toggle("keymap", "vim", "emacs", w.keys_pick, w.keys_focus == 0), text(""),
	             toggle("density", "cozy", "compact", w.density_pick, w.keys_focus == 1)});
}

Element import_body(const wizard& w, const theme& th, std::int64_t ms) {
	return vbox({text("  point at an unpacked claude.ai data export, or skip.") | C(th.p.subtle),
	             text(""),
	             hbox({text("  " + w.import_path) | C(th.p.text), ui::caret(th, ms, false)}) |
	                 bgcolor(ui::col(th.p.surface)) | size(WIDTH, GREATER_THAN, 40)});
}

}  // namespace

Element wizard::render(const theme& th, std::int64_t ms) {
	switch (at) {
		case step::caps:
			return shell(th, 0, "terminal report card", caps_body(th, caps, entered_ms, ms),
			             "enter to continue");
		case step::provider:
			return shell(
			    th, 1, "connect a provider", provider_body(*this, th, ms),
			    psub == 2 ? "type, enter to check, esc back" : "j/k choose · enter · esc back");
		case step::theme:
			return shell(th, 2, "pick a theme", theme_body(*this, th),
			             "j/k choose · enter · esc back");
		case step::keys:
			return shell(th, 3, "keys and density", keys_body(*this, th),
			             "j/k row · h/l change · enter · esc back");
		case step::import_:
			return shell(th, 4, "import from claude.ai (optional)", import_body(*this, th, ms),
			             "type a path, enter to import or skip");
		case step::done:
			return shell(th, 5, "ready",
			             vbox({text("  everything is set.") | C(th.p.text), text(""),
			                   text("  bonne écriture.") | C(th.p.gold) | italic}),
			             "enter to begin");
	}
	return text("");
}

// -- events -------------------------------------------------------------------

namespace {

provider_config build_pc(const wizard& w) {
	const auto& p = kProviders[w.provider_pick];
	provider_config pc;
	pc.kind = p.kind;
	pc.base_url = p.base_url;
	pc.credential.value = w.key_input;
	const std::string src = kAuthSource[w.auth_pick];
	if (src == "key_cmd")
		pc.credential.kind = auth::source::key_cmd;
	else if (src == "keychain")
		pc.credential.kind = auth::source::keychain;
	else if (src == "inline")
		pc.credential.kind = auth::source::inline_key;
	else
		pc.credential.kind = auth::source::env;
	return pc;
}

void start_validation(wizard& w, ScreenInteractive& screen) {
	if (w.val_thread.joinable()) w.val_thread.join();
	w.validating = true;
	w.validated = 0;
	w.validate_note.clear();
	const provider_config pc = build_pc(w);
	w.val_thread = std::thread([&w, &screen, pc] {
		auto p = std::getenv("PLUME_MOCK") ? result<std::unique_ptr<provider>>(make_mock())
		                                   : make_provider(pc);
		if (!p) {
			w.validate_note = p.error().detail;
			w.validated = -1;
		} else if (auto models = (*p)->list_models(); !models) {
			w.validate_note = models.error().detail;
			w.validated = -1;
		} else {
			std::string chosen;
			for (const auto& m : *models)
				if (m.id.find("opus") != std::string::npos ||
				    m.id.find("sonnet") != std::string::npos) {
					chosen = m.id;
					break;
				}
			if (chosen.empty() && !models->empty()) chosen = models->front().id;
			w.validate_model = chosen;
			w.validate_note = std::to_string(models->size()) + " models reachable";
			w.validated = 1;
		}
		w.validating = false;
		screen.PostEvent(Event::Custom);
	});
}

void commit_provider(wizard& w) {
	const auto& p = kProviders[w.provider_pick];
	provider_entry pe;
	pe.kind = p.kind;
	pe.base_url = p.base_url;
	pe.auth_source = kAuthSource[w.auth_pick];
	pe.auth_value = w.key_input;
	pe.default_model = w.validate_model;
	w.result.providers[p.kind] = pe;
	w.result.default_provider = p.kind;
	w.result.defaults.model = w.validate_model;
}

}  // namespace

bool wizard::handle(const Event& e, ScreenInteractive& screen) {
	const std::int64_t ms = now_ms();

	// a finished validation advances the flow on the next event tick.
	if (at == step::provider && validated.load() == 1) {
		commit_provider(*this);
		validated = 0;
		at = step::theme;
		entered_ms = ms;
		return true;
	}
	if (e == Event::Custom) return true;

	auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };

	switch (at) {
		case step::caps:
			if (e == Event::Return) {
				at = step::provider;
				entered_ms = ms;
			}
			return true;

		case step::provider:
			if (psub == 0) {
				if (e == Event::Character("j") || e == Event::ArrowDown)
					provider_pick =
					    clamp(provider_pick + 1, 0, static_cast<int>(kProviders.size()) - 1);
				else if (e == Event::Character("k") || e == Event::ArrowUp)
					provider_pick =
					    clamp(provider_pick - 1, 0, static_cast<int>(kProviders.size()) - 1);
				else if (e == Event::Return) {
					psub = 1;
					auth_pick = 0;
				} else if (e == Event::Escape) {
					at = step::caps;
				}
			} else if (psub == 1) {
				if (e == Event::Character("j") || e == Event::ArrowDown)
					auth_pick = clamp(auth_pick + 1, 0, 3);
				else if (e == Event::Character("k") || e == Event::ArrowUp)
					auth_pick = clamp(auth_pick - 1, 0, 3);
				else if (e == Event::Return) {
					psub = 2;
					editing = true;
					key_input = auth_pick == 0 ? kProviders[provider_pick].env : "";
				} else if (e == Event::Escape) {
					psub = 0;
				}
			} else {  // psub 2: key entry + validate
				if (validating.load()) return true;
				if (e == Event::Return) {
					if (!key_input.empty() ||
					    std::string(kProviders[provider_pick].kind) == "ollama")
						start_validation(*this, screen);
				} else if (e == Event::Escape) {
					psub = 1;
					editing = false;
					validated = 0;
				} else if (e == Event::Backspace) {
					if (!key_input.empty()) key_input.pop_back();
				} else if (e.is_character()) {
					key_input += e.character();
				}
			}
			return true;

		case step::theme:
			if (e == Event::Character("j") || e == Event::ArrowDown)
				theme_pick = clamp(theme_pick + 1, 0, static_cast<int>(kThemeNames.size()) - 1);
			else if (e == Event::Character("k") || e == Event::ArrowUp)
				theme_pick = clamp(theme_pick - 1, 0, static_cast<int>(kThemeNames.size()) - 1);
			else if (e == Event::Return) {
				result.ui.theme = kThemeNames[theme_pick];
				at = step::keys;
				entered_ms = ms;
			} else if (e == Event::Escape) {
				psub = 0;
				at = step::provider;
			}
			return true;

		case step::keys:
			if (e == Event::Character("j") || e == Event::ArrowDown)
				keys_focus = clamp(keys_focus + 1, 0, 1);
			else if (e == Event::Character("k") || e == Event::ArrowUp)
				keys_focus = clamp(keys_focus - 1, 0, 1);
			else if (e == Event::Character("h") || e == Event::Character("l") ||
			         e == Event::ArrowLeft || e == Event::ArrowRight ||
			         e == Event::Character(" ")) {
				if (keys_focus == 0)
					keys_pick ^= 1;
				else
					density_pick ^= 1;
			} else if (e == Event::Return) {
				result.keys.preset = keys_pick == 0 ? "vim" : "emacs";
				result.ui.density = density_pick == 0 ? "cozy" : "compact";
				at = step::import_;
				entered_ms = ms;
			} else if (e == Event::Escape) {
				at = step::theme;
			}
			return true;

		case step::import_:
			if (e == Event::Return) {
				at = step::done;
				entered_ms = ms;
			} else if (e == Event::Escape) {
				at = step::keys;
			} else if (e == Event::Backspace) {
				if (!import_path.empty()) import_path.pop_back();
			} else if (e.is_character()) {
				import_path += e.character();
			}
			return true;

		case step::done:
			if (e == Event::Return) {
				finished = true;
				active = false;
			}
			return true;
	}
	return true;
}

}  // namespace plume
