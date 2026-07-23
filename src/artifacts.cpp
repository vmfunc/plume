// artifacts: the code and document blocks a conversation produces, gathered into
// a drawer you can browse, copy to the clipboard, or save to a file. out-of-line
// members of app::impl.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

namespace {

// map a fence language to a file extension for saving.
std::string ext_for(const std::string& lang) {
	static const std::unordered_map<std::string, std::string> m = {
	    {"cpp", "cpp"},   {"c++", "cpp"},       {"c", "c"},       {"python", "py"},
	    {"py", "py"},     {"javascript", "js"}, {"js", "js"},     {"typescript", "ts"},
	    {"ts", "ts"},     {"rust", "rs"},       {"go", "go"},     {"nix", "nix"},
	    {"json", "json"}, {"toml", "toml"},     {"yaml", "yaml"}, {"sh", "sh"},
	    {"bash", "sh"},   {"html", "html"},     {"css", "css"},   {"lua", "lua"},
	    {"sql", "sql"},   {"markdown", "md"},   {"md", "md"}};
	if (auto it = m.find(lang); it != m.end()) return it->second;
	return lang.empty() ? "txt" : "txt";
}

}  // namespace

std::vector<app::impl::artifact> app::impl::collect_artifacts() const {
	std::vector<artifact> out;
	for (const auto& n : transcript) {
		const std::string body = node_text(n);
		std::size_t pos = 0;
		while ((pos = body.find("```", pos)) != std::string::npos) {
			const std::size_t nl = body.find('\n', pos);
			if (nl == std::string::npos) break;
			std::string lang = body.substr(pos + 3, nl - pos - 3);
			const std::size_t close = body.find("```", nl + 1);
			if (close == std::string::npos) break;
			if (lang == "plume" || lang == "plume-widget") {  // widgets aren't artifacts
				pos = close + 3;
				continue;
			}
			std::string content = body.substr(nl + 1, close - nl - 1);
			if (!content.empty() && content.back() == '\n') content.pop_back();
			out.push_back({lang, content});
			pos = close + 3;
		}
	}
	return out;
}

Element app::impl::artifacts_view() {
	const auto arts = collect_artifacts();
	if (art_view >= 0 && art_view < static_cast<int>(arts.size())) {  // full view
		const auto& a = arts[static_cast<std::size_t>(art_view)];
		Elements lines;
		std::size_t start = 0;
		int shown = 0;
		while (start <= a.content.size() && shown < 40) {
			const std::size_t nl = a.content.find('\n', start);
			lines.push_back(
			    text("  " + a.content.substr(
			                    start, nl == std::string::npos ? std::string::npos : nl - start)) |
			    color(col(th.p.text)));
			if (nl == std::string::npos) break;
			start = nl + 1;
			++shown;
		}
		return ui::overlay(
		    th, "artifact  " + (a.lang.empty() ? "text" : a.lang),
		    vbox({vbox(std::move(lines)) | yframe | size(HEIGHT, LESS_THAN, 20),
		          separator() | color(col(th.p.hl_med)),
		          text(" y copy · s save · esc back") | color(col(th.p.muted)) | dim}));
	}

	Elements rows = {text("")};
	if (arts.empty())
		rows.push_back(text("  no code or document blocks yet") | color(col(th.p.muted)) | dim);
	for (int i = 0; i < static_cast<int>(arts.size()); ++i) {
		const bool on = i == art_sel;
		const auto& a = arts[static_cast<std::size_t>(i)];
		const int nlines =
		    static_cast<int>(std::count(a.content.begin(), a.content.end(), '\n')) + 1;
		std::string first = a.content.substr(0, a.content.find('\n'));
		if (first.size() > 34) first = first.substr(0, 33) + "…";
		Element row = hbox({text(on ? "  ▸ " : "    ") | color(col(on ? th.p.iris : th.p.muted)),
		                    text(a.lang.empty() ? "text" : a.lang) | color(col(th.p.foam)) |
		                        size(WIDTH, EQUAL, 12),
		                    text(std::to_string(nlines) + "L ") | color(col(th.p.muted)) | dim,
		                    text(first) | color(col(on ? th.p.text : th.p.subtle))});
		if (on) row = row | bgcolor(col(th.p.hl_low));
		rows.push_back(hot(std::move(row), hit_kind::overlay_row, i));
	}
	rows.push_back(text(""));
	rows.push_back(text("enter view · esc close") | color(col(th.p.muted)) | dim);
	return ui::overlay(th, "artifacts", vbox(std::move(rows)));
}

bool app::impl::handle_artifacts(const Event& e) {
	const auto arts = collect_artifacts();
	if (art_view >= 0) {  // viewing one
		if (e == Event::Escape) return art_view = -1, true;
		if (art_view >= static_cast<int>(arts.size())) return art_view = -1, true;
		const auto& a = arts[static_cast<std::size_t>(art_view)];
		if (e == Event::Character("y")) {
			osc52_copy(a.content);
			toast = "copied artifact";
			return true;
		}
		if (e == Event::Character("s")) {
			const std::string dir = cfg.state_dir + "/artifacts";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			const std::string path = dir + "/artifact-" + new_id("art") + "." + ext_for(a.lang);
			std::ofstream(path) << a.content;
			toast = "saved " + path;
			return true;
		}
		return true;
	}
	const int max = static_cast<int>(arts.size()) - 1;
	if (e == Event::Escape) return ov = overlay::none, true;
	if (e == Event::ArrowDown || e == Event::CtrlN || e == Event::Character("j"))
		return art_sel = std::min(art_sel + 1, std::max(0, max)), true;
	if (e == Event::ArrowUp || e == Event::CtrlP || e == Event::Character("k"))
		return art_sel = std::max(art_sel - 1, 0), true;
	if (e == Event::Return && art_sel <= max && max >= 0) return art_view = art_sel, true;
	return true;
}

}  // namespace plume
