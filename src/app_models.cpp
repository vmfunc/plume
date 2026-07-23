// the model + provider picker overlay: an off-thread fetch of the provider's
// model list, cached to disk, with fuzzy filter, capability chips, recent models
// on top, and a per-conversation selection. out-of-line members of app::impl.
#include <algorithm>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

namespace {

std::string snapshot_path(const config& cfg) {
	return cfg.state_dir + "/models-" +
	       (cfg.default_provider.empty() ? "default" : cfg.default_provider) + ".json";
}

std::string ctx_label(std::int64_t ctx) {
	if (ctx <= 0) return "";
	if (ctx >= 1'000'000) return std::to_string(ctx / 1'000'000) + "m";
	return std::to_string(ctx / 1000) + "k";
}

}  // namespace

void app::impl::open_models() {
	if (!prov) {
		toast = "no provider configured";
		return;
	}
	ov = overlay::models;
	ov_filter.clear();
	ov_sel = 0;
	if (model_list.empty()) load_models_snapshot();  // show something instantly
	fetch_models(false);                             // then refresh in the background
}

void app::impl::fetch_models(bool force) {
	if (!prov || models_loading.load()) return;
	if (!force && !model_list.empty()) return;
	if (models_worker.joinable()) models_worker.join();
	models_loading = true;
	models_error.clear();
	models_worker = std::thread([this] {
		auto r = prov->list_models();
		screen.Post([this, r = std::move(r)] {
			models_loading = false;
			if (r)
				model_list = *r, save_models_snapshot();
			else if (model_list.empty())
				models_error = r.error().detail;
		});
		screen.PostEvent(Event::Custom);
	});
}

void app::impl::save_models_snapshot() const {
	if (model_list.empty()) return;
	nlohmann::json j = nlohmann::json::array();
	for (const auto& m : model_list)
		j.push_back({{"id", m.id},
		             {"display_name", m.display_name},
		             {"context", m.context},
		             {"max_output", m.max_output},
		             {"vision", m.caps.vision},
		             {"tools", m.caps.tools},
		             {"thinking", m.caps.thinking}});
	std::error_code ec;
	std::filesystem::create_directories(cfg.state_dir, ec);
	std::ofstream(snapshot_path(cfg)) << j.dump();
}

void app::impl::load_models_snapshot() {
	std::ifstream in(snapshot_path(cfg));
	if (!in) return;
	const std::string body((std::istreambuf_iterator<char>(in)), {});
	const auto j = nlohmann::json::parse(body, nullptr, false);
	if (j.is_discarded() || !j.is_array()) return;
	model_list.clear();
	for (const auto& e : j) {
		model_info m;
		m.id = e.value("id", "");
		m.display_name = e.value("display_name", "");
		m.context = e.value("context", std::int64_t{0});
		m.max_output = e.value("max_output", std::int64_t{0});
		m.caps.vision = e.value("vision", false);
		m.caps.tools = e.value("tools", false);
		m.caps.thinking = e.value("thinking", false);
		if (!m.id.empty()) model_list.push_back(std::move(m));
	}
}

std::vector<const model_info*> app::impl::filtered_models() const {
	const auto matches = [&](const model_info& m) {
		return ov_filter.empty() || fuzzy(ov_filter, m.id) || fuzzy(ov_filter, m.display_name);
	};
	std::vector<const model_info*> out;
	std::set<std::string> seen;
	for (const auto& id : recent_models)  // MRU first
		for (const auto& m : model_list)
			if (m.id == id && matches(m) && seen.insert(id).second) out.push_back(&m);
	for (const auto& m : model_list)
		if (matches(m) && seen.insert(m.id).second) out.push_back(&m);
	return out;
}

void app::impl::choose_model(const std::string& id) {
	if (id.empty()) return;
	convo_model = id;  // per-conversation override
	recent_models.erase(std::remove(recent_models.begin(), recent_models.end(), id),
	                    recent_models.end());
	recent_models.insert(recent_models.begin(), id);
	if (recent_models.size() > 8) recent_models.resize(8);
	ov = overlay::none;
	toast = "model " + id;
}

Element app::impl::models_view() {
	const auto fm = filtered_models();
	const int recent_n = static_cast<int>(std::count_if(fm.begin(), fm.end(), [&](const auto* m) {
		return std::find(recent_models.begin(), recent_models.end(), m->id) != recent_models.end();
	}));
	Elements rows = {
	    hbox({text("› ") | color(col(th.p.iris)) | bold, text(ov_filter) | color(col(th.p.text)),
	          text("▏") | color(col(th.p.iris))}),
	    text("")};
	if (models_loading.load() && model_list.empty())
		rows.push_back(text("  " + ui::spinner(now_ms()) + " fetching the model list...") |
		               color(col(th.p.subtle)));
	else if (fm.empty() && !models_error.empty())
		rows.push_back(text("  " + models_error) | color(col(th.p.love)));
	else if (fm.empty())
		rows.push_back(text("  enter uses \"" + ov_filter + "\" as a raw model id") |
		               color(col(th.p.muted)) | dim);

	int vis = 0;
	for (const auto* m : fm) {
		if (vis == recent_n && recent_n > 0)  // divider between recent and the rest
			rows.push_back(separator() | color(col(th.p.hl_low)));
		const bool on = vis == ov_sel;
		const bool active = m->id == model_id();
		const std::string name = m->display_name.empty() ? m->id : m->display_name;
		Elements cells = {
		    text(on ? "  ▸ " : "    ") | color(col(on ? th.p.iris : th.p.muted)),
		    text(name) | color(col(on ? th.p.text : th.p.subtle)) | size(WIDTH, EQUAL, 26)};
		if (const std::string cl = ctx_label(m->context); !cl.empty())
			cells.push_back(text(cl + " ") | color(col(th.p.foam)) | dim);
		if (auto it = cfg.prices.find(m->id); it != cfg.prices.end())
			cells.push_back(text("$" + std::to_string(static_cast<int>(it->second.input)) + "/" +
			                     std::to_string(static_cast<int>(it->second.output)) + " ") |
			                color(col(th.p.gold)) | dim);
		if (m->caps.vision) cells.push_back(text(" vis") | color(col(th.p.rose)) | dim);
		if (m->caps.tools) cells.push_back(text(" tools") | color(col(th.p.iris)) | dim);
		if (m->caps.thinking) cells.push_back(text(" think") | color(col(th.p.pine)) | dim);
		if (active) cells.push_back(text("  active") | color(col(th.p.pine)));
		Element row = hbox(std::move(cells));
		if (on) row = row | bgcolor(col(th.p.hl_low));
		rows.push_back(hot(std::move(row), hit_kind::overlay_row, vis));
		++vis;
	}

	std::string title = "models";
	if (!cfg.default_provider.empty()) title += "  " + cfg.default_provider;
	if (models_loading.load() && !model_list.empty()) title += "  refreshing";
	return ui::overlay(
	    th, title,
	    vbox(std::move(rows)) | yframe | vscroll_indicator | size(HEIGHT, LESS_THAN, 22));
}

bool app::impl::handle_models(const Event& e) {
	const auto fm = filtered_models();
	const int max = static_cast<int>(fm.size()) - 1;
	if (e == Event::Escape) return ov = overlay::none, true;
	if (e == Event::ArrowDown || e == Event::CtrlN)
		return ov_sel = std::min(ov_sel + 1, std::max(0, max)), true;
	if (e == Event::ArrowUp || e == Event::CtrlP) return ov_sel = std::max(ov_sel - 1, 0), true;
	if (e == Event::CtrlR) return fetch_models(true), true;  // refresh the list
	if (e == Event::Backspace) {
		if (!ov_filter.empty()) ov_filter.pop_back();
		ov_sel = 0;
		return true;
	}
	if (e == Event::Return) {
		if (ov_sel >= 0 && ov_sel <= max)
			choose_model(fm[static_cast<std::size_t>(ov_sel)]->id);
		else if (!ov_filter.empty())
			choose_model(ov_filter);  // offline escape: use the typed text as an id
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
