// the roles (personas) and snippet library overlays. a role applies as the
// conversation system prompt; a snippet expands into the composer, prompting for
// any {{variables}} first. out-of-line members of app::impl.
#include <algorithm>

#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

namespace {

std::string first_line(const std::string& s, std::size_t n) {
	std::string line = s.substr(0, s.find('\n'));
	if (line.size() > n) line = line.substr(0, n - 1) + "…";
	return line;
}

std::vector<std::string> extract_vars(const std::string& body) {
	std::vector<std::string> vars;
	std::size_t p = 0;
	while ((p = body.find("{{", p)) != std::string::npos) {
		const std::size_t e = body.find("}}", p + 2);
		if (e == std::string::npos) break;
		std::string v = body.substr(p + 2, e - p - 2);
		while (!v.empty() && v.front() == ' ') v.erase(v.begin());
		while (!v.empty() && v.back() == ' ') v.pop_back();
		if (!v.empty() && std::find(vars.begin(), vars.end(), v) == vars.end()) vars.push_back(v);
		p = e + 2;
	}
	return vars;
}

std::string fill_vars(std::string body, const std::vector<std::string>& vars,
                      const std::vector<std::string>& vals) {
	for (std::size_t i = 0; i < vars.size(); ++i) {
		const std::string needle = "{{" + vars[i] + "}}";
		for (std::size_t p; (p = body.find(needle)) != std::string::npos;)
			body.replace(p, needle.size(), i < vals.size() ? vals[i] : "");
		// also match with inner spaces: {{ name }}
		const std::string spaced = "{{ " + vars[i] + " }}";
		for (std::size_t p; (p = body.find(spaced)) != std::string::npos;)
			body.replace(p, spaced.size(), i < vals.size() ? vals[i] : "");
	}
	return body;
}

}  // namespace

// -- roles --------------------------------------------------------------------

void app::impl::save_role(const std::string& name) {
	if (name.empty() || system_prompt.empty()) {
		toast = "nothing to save (set a system prompt first)";
		return;
	}
	cfg.roles[name] = system_prompt;
	persist_config();
	toast = "saved role " + name;
}

Element app::impl::roles_view() {
	std::vector<const std::pair<const std::string, std::string>*> list;
	for (const auto& kv : cfg.roles)
		if (ov_filter.empty() || fuzzy(ov_filter, kv.first)) list.push_back(&kv);

	Elements rows = {
	    hbox({text("› ") | color(col(th.p.iris)) | bold, text(ov_filter) | color(col(th.p.text)),
	          text("▏") | color(col(th.p.iris))}),
	    text("")};
	if (list.empty())
		rows.push_back(text("  no roles yet — /role save <name> keeps the current system prompt") |
		               color(col(th.p.muted)) | dim);
	for (int i = 0; i < static_cast<int>(list.size()); ++i) {
		const bool on = i == ov_sel;
		const bool active = list[static_cast<std::size_t>(i)]->second == system_prompt;
		Element row = hbox({text(on ? "  ▸ " : "    ") | color(col(on ? th.p.iris : th.p.muted)),
		                    text(list[static_cast<std::size_t>(i)]->first) |
		                        color(col(on ? th.p.text : th.p.subtle)) | size(WIDTH, EQUAL, 16),
		                    text(first_line(list[static_cast<std::size_t>(i)]->second, 40)) |
		                        color(col(th.p.muted)) | dim,
		                    active ? text("  active") | color(col(th.p.pine)) : text("")});
		if (on) row = row | bgcolor(col(th.p.hl_low));
		rows.push_back(hot(std::move(row), hit_kind::overlay_row, i));
	}
	rows.push_back(text(""));
	rows.push_back(text("enter apply · esc close") | color(col(th.p.muted)) | dim);
	return ui::overlay(th, "roles", vbox(std::move(rows)));
}

bool app::impl::handle_roles(const Event& e) {
	std::vector<const std::pair<const std::string, std::string>*> list;
	for (const auto& kv : cfg.roles)
		if (ov_filter.empty() || fuzzy(ov_filter, kv.first)) list.push_back(&kv);
	const int max = static_cast<int>(list.size()) - 1;
	if (e == Event::Escape) return ov = overlay::none, true;
	if (e == Event::ArrowDown || e == Event::CtrlN)
		return ov_sel = std::min(ov_sel + 1, std::max(0, max)), true;
	if (e == Event::ArrowUp || e == Event::CtrlP) return ov_sel = std::max(ov_sel - 1, 0), true;
	if (e == Event::Backspace) {
		if (!ov_filter.empty()) ov_filter.pop_back();
		ov_sel = 0;
		return true;
	}
	if (e == Event::Return && ov_sel >= 0 && ov_sel <= max) {
		system_prompt = list[static_cast<std::size_t>(ov_sel)]->second;
		toast = "role " + list[static_cast<std::size_t>(ov_sel)]->first;
		ov = overlay::none;
		return true;
	}
	if (e.is_character()) {
		ov_filter += e.character();
		ov_sel = 0;
		return true;
	}
	return true;
}

// -- snippets -----------------------------------------------------------------

Element app::impl::snips_view() {
	if (snip_fill) {  // the {{variable}} fill form
		Elements rows;
		for (std::size_t i = 0; i < snip_vars.size(); ++i) {
			const bool on = static_cast<int>(i) == snip_idx;
			rows.push_back(
			    hbox({text(on ? " ▸ " : "   ") | color(col(on ? th.p.iris : th.p.muted)),
			          text(snip_vars[i]) | color(col(th.p.foam)) | size(WIDTH, EQUAL, 16),
			          text(snip_vals[i]) | color(col(th.p.text)),
			          on ? text("▏") | color(col(th.p.iris)) : text("")}));
		}
		rows.push_back(text(""));
		rows.push_back(text("type · enter next · esc cancel") | color(col(th.p.muted)) | dim);
		return ui::overlay(th, "fill snippet", vbox(std::move(rows)));
	}

	std::vector<const snippet*> list;
	for (const auto& s : cfg.snippets)
		if (ov_filter.empty() || fuzzy(ov_filter, s.name)) list.push_back(&s);
	Elements rows = {
	    hbox({text("› ") | color(col(th.p.iris)) | bold, text(ov_filter) | color(col(th.p.text)),
	          text("▏") | color(col(th.p.iris))}),
	    text("")};
	if (list.empty())
		rows.push_back(text("  no snippets — add [[snippets]] with name + body to config.toml") |
		               color(col(th.p.muted)) | dim);
	for (int i = 0; i < static_cast<int>(list.size()); ++i) {
		const bool on = i == ov_sel;
		Element row = hbox({text(on ? "  ▸ " : "    ") | color(col(on ? th.p.iris : th.p.muted)),
		                    text(list[static_cast<std::size_t>(i)]->name) |
		                        color(col(on ? th.p.text : th.p.subtle)) | size(WIDTH, EQUAL, 16),
		                    text(first_line(list[static_cast<std::size_t>(i)]->body, 40)) |
		                        color(col(th.p.muted)) | dim});
		if (on) row = row | bgcolor(col(th.p.hl_low));
		rows.push_back(hot(std::move(row), hit_kind::overlay_row, i));
	}
	rows.push_back(text(""));
	rows.push_back(text("enter insert · esc close") | color(col(th.p.muted)) | dim);
	return ui::overlay(th, "snippets", vbox(std::move(rows)));
}

bool app::impl::handle_snips(const Event& e) {
	if (snip_fill) {
		if (e == Event::Escape) return snip_fill = false, ov = overlay::none, true;
		if (e == Event::Backspace) {
			if (!snip_vals[static_cast<std::size_t>(snip_idx)].empty())
				snip_vals[static_cast<std::size_t>(snip_idx)].pop_back();
			return true;
		}
		if (e == Event::Return) {
			if (snip_idx + 1 < static_cast<int>(snip_vars.size())) {
				++snip_idx;
			} else {  // done: expand into the composer
				comp.set_text(fill_vars(snip_body, snip_vars, snip_vals));
				snip_fill = false;
				ov = overlay::none;
			}
			return true;
		}
		if (e.is_character()) {
			snip_vals[static_cast<std::size_t>(snip_idx)] += e.character();
			return true;
		}
		return true;
	}

	std::vector<const snippet*> list;
	for (const auto& s : cfg.snippets)
		if (ov_filter.empty() || fuzzy(ov_filter, s.name)) list.push_back(&s);
	const int max = static_cast<int>(list.size()) - 1;
	if (e == Event::Escape) return ov = overlay::none, true;
	if (e == Event::ArrowDown || e == Event::CtrlN)
		return ov_sel = std::min(ov_sel + 1, std::max(0, max)), true;
	if (e == Event::ArrowUp || e == Event::CtrlP) return ov_sel = std::max(ov_sel - 1, 0), true;
	if (e == Event::Backspace) {
		if (!ov_filter.empty()) ov_filter.pop_back();
		ov_sel = 0;
		return true;
	}
	if (e == Event::Return && ov_sel >= 0 && ov_sel <= max) {
		const std::string body = list[static_cast<std::size_t>(ov_sel)]->body;
		snip_vars = extract_vars(body);
		if (snip_vars.empty()) {  // no variables: straight into the composer
			comp.set_text(body);
			ov = overlay::none;
		} else {
			snip_body = body;
			snip_vals.assign(snip_vars.size(), "");
			snip_idx = 0;
			snip_fill = true;
		}
		return true;
	}
	if (e.is_character()) {
		ov_filter += e.character();
		ov_sel = 0;
		return true;
	}
	return true;
}

}  // namespace plume
