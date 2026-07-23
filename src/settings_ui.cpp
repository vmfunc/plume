// the settings overlay: a small list of preferences edited live, each change
// applied and persisted immediately. out-of-line members of app::impl.
#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

namespace {

// the editable fields, in display order. keep in sync with the switch below.
constexpr int kFieldCount = 10;
constexpr const char* kLabels[kFieldCount] = {
    "theme",   "density",       "animations", "notify",  "thinking",
    "sidebar", "show thinking", "zen",        "widgets", "web search",
};

}  // namespace

void app::impl::cycle_theme(int dir) {
	static const char* themes[] = {"rose-pine", "rose-pine-moon", "rose-pine-dawn", "va11"};
	int idx = 0;
	for (int i = 0; i < 4; ++i)
		if (cfg.ui.theme == themes[i]) idx = i;
	idx = (idx + dir + 4) % 4;
	cfg.ui.theme = themes[idx];
	const bool dark = caps.background ? caps.dark : true;
	if (auto t = resolve_theme(cfg.ui.theme, cfg.config_dir + "/themes", dark)) th = *t;
}

Element app::impl::settings_view() {
	auto value = [&](int f) -> std::string {
		switch (f) {
			case 0: return cfg.ui.theme;
			case 1: return cfg.ui.density;
			case 2: return cfg.ui.reduce_motion ? "off" : "on";
			case 3: return cfg.notify;
			case 4:
				return cfg.defaults.thinking == thinking_mode::off        ? "off"
				       : cfg.defaults.thinking == thinking_mode::adaptive ? "adaptive"
				                                                          : "budget";
			case 5: return cfg.ui.sidebar ? "on start" : "on demand";
			case 6: return show_think ? "shown" : "hidden";
			case 7: return cfg.ui.zen ? "on" : "off";
			case 8: return cfg.ui.widgets ? "on" : "off";
			case 9: return web_on ? "on" : "off";
			default: return "";
		}
	};
	Elements rows;
	for (int f = 0; f < kFieldCount; ++f) {
		const bool on = f == settings_sel;
		Element row = hbox(
		    {text(on ? " ▸ " : "   ") | color(col(on ? th.p.iris : th.p.muted)),
		     text(kLabels[f]) | color(col(on ? th.p.text : th.p.subtle)) | size(WIDTH, EQUAL, 16),
		     text(value(f)) | color(col(th.p.foam))});
		if (on) row = row | bgcolor(col(th.p.hl_low));
		rows.push_back(hot(std::move(row), hit_kind::overlay_row, f));
	}
	rows.push_back(text(""));
	rows.push_back(text("j/k move · h/l or enter change · esc close") | color(col(th.p.muted)) |
	               dim);
	return ui::overlay(th, "settings", vbox(std::move(rows)));
}

bool app::impl::handle_settings(const Event& e) {
	if (e == Event::Escape) return ov = overlay::none, true;
	if (e == Event::ArrowDown || e == Event::CtrlN || e == Event::Character("j"))
		return settings_sel = std::min(settings_sel + 1, kFieldCount - 1), true;
	if (e == Event::ArrowUp || e == Event::CtrlP || e == Event::Character("k"))
		return settings_sel = std::max(settings_sel - 1, 0), true;

	const int dir = (e == Event::ArrowLeft || e == Event::Character("h")) ? -1 : 1;
	const bool change = e == Event::Return || e == Event::Character("h") ||
	                    e == Event::Character("l") || e == Event::ArrowLeft ||
	                    e == Event::ArrowRight;
	if (!change) return true;

	switch (settings_sel) {
		case 0: cycle_theme(dir); break;
		case 1: cfg.ui.density = cfg.ui.density == "cozy" ? "compact" : "cozy"; break;
		case 2: cfg.ui.reduce_motion = !cfg.ui.reduce_motion; break;
		case 3:
			cfg.notify = cfg.notify == "bell" ? "osc9" : cfg.notify == "osc9" ? "off" : "bell";
			break;
		case 4:
			cfg.defaults.thinking =
			    cfg.defaults.thinking == thinking_mode::off        ? thinking_mode::adaptive
			    : cfg.defaults.thinking == thinking_mode::adaptive ? thinking_mode::budget
			                                                       : thinking_mode::off;
			break;
		case 5:
			cfg.ui.sidebar = !cfg.ui.sidebar;
			sb = cfg.ui.sidebar ? sidebar_mode::open : sidebar_mode::hidden;
			if (cfg.ui.sidebar) refresh_sidebar();
			break;
		case 6: show_think = !show_think; break;
		case 7: cfg.ui.zen = !cfg.ui.zen; break;
		case 8: cfg.ui.widgets = !cfg.ui.widgets; break;
		case 9:
			web_on = !web_on;
			cfg.web_search = web_on;
			break;
		default: break;
	}
	persist_config();
	return true;
}

}  // namespace plume
