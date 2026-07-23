// generative terminal widgets. the model can emit a ```plume fenced block of json
// with a "type" field; plume renders it as a rich element instead of raw code.
// this is what lets "what's the weather" come back as a card, not a paragraph.
#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "ui.hpp"

namespace plume::ui {

using namespace ftxui;
using json = nlohmann::json;

namespace {

std::string as_str(const json& v) {
	if (v.is_string()) return v.get<std::string>();
	if (v.is_number_integer()) return std::to_string(v.get<long long>());
	if (v.is_number()) {
		std::string s = std::to_string(v.get<double>());
		if (const auto d = s.find('.'); d != std::string::npos) {
			std::size_t last = s.find_last_not_of('0');
			if (last == d) --last;
			s.erase(last + 1);
		}
		return s;
	}
	if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
	return v.is_null() ? "" : v.dump();
}

std::string field(const json& j, const char* k) {
	return j.contains(k) ? as_str(j[k]) : std::string{};
}

Element frame(Element body, rgb tint) {
	return body | borderRounded | color(col(tint)) | size(WIDTH, LESS_THAN, 60);
}

Element weather_widget(const theme& th, const json& j) {
	std::string temp = j.contains("temp_c")   ? as_str(j["temp_c"]) + "°C"
	                   : j.contains("temp_f") ? as_str(j["temp_f"]) + "°F"
	                                          : field(j, "temp");
	Elements rows = {hbox({text(field(j, "location")) | bold | color(col(th.p.text)), filler(),
	                       text(temp) | color(col(th.p.gold)) | bold}),
	                 text(field(j, "condition")) | color(col(th.p.foam))};
	if (j.contains("forecast") && j["forecast"].is_array()) {
		Elements days;
		for (const auto& d : j["forecast"])
			days.push_back(
			    vbox({text(field(d, "day")) | color(col(th.p.subtle)),
			          text(field(d, "hi") + "/" + field(d, "lo")) | color(col(th.p.muted))}) |
			    flex);
		rows.push_back(separator() | color(col(th.p.hl_low)));
		rows.push_back(hbox(std::move(days)));
	}
	return frame(vbox(std::move(rows)), th.p.iris);
}

Element table_widget(const theme& th, const json& j) {
	Elements rows;
	if (j.contains("columns") && j["columns"].is_array()) {
		Elements hdr;
		for (const auto& c : j["columns"])
			hdr.push_back(text(as_str(c) + "  ") | bold | color(col(th.p.iris)) | flex);
		rows.push_back(hbox(std::move(hdr)));
		rows.push_back(separator() | color(col(th.p.hl_med)));
	}
	if (j.contains("rows") && j["rows"].is_array())
		for (const auto& r : j["rows"]) {
			Elements cells;
			if (r.is_array())
				for (const auto& c : r)
					cells.push_back(text(as_str(c) + "  ") | color(col(th.p.text)) | flex);
			rows.push_back(hbox(std::move(cells)));
		}
	return frame(vbox(std::move(rows)), th.p.hl_med);
}

Element card_widget(const theme& th, const json& j) {
	Elements rows;
	if (const std::string t = field(j, "title"); !t.empty()) {
		rows.push_back(text(t) | bold | color(col(th.p.text)));
		rows.push_back(separator() | color(col(th.p.hl_low)));
	}
	if (j.contains("fields") && j["fields"].is_object())
		for (const auto& [k, v] : j["fields"].items())
			rows.push_back(hbox({text(k) | color(col(th.p.muted)) | size(WIDTH, EQUAL, 16),
			                     text(as_str(v)) | color(col(th.p.foam))}));
	if (rows.empty()) rows.push_back(paragraph(j.dump(2)) | color(col(th.p.subtle)));
	return frame(vbox(std::move(rows)), th.p.iris);
}

Element progress_widget(const theme& th, const json& j) {
	const float v = j.contains("value") && j["value"].is_number()
	                    ? static_cast<float>(j["value"].get<double>())
	                    : 0.0f;
	return frame(
	    vbox({hbox({text(field(j, "label")) | color(col(th.p.text)), filler(),
	                text(std::to_string(static_cast<int>(v * 100)) + "%") | color(col(th.p.gold))}),
	          meter(th, v, 28)}),
	    th.p.hl_med);
}

Element chart_widget(const theme& th, const json& j) {
	double maxv = 0;
	if (j.contains("bars"))
		for (const auto& b : j["bars"])
			if (b.contains("value") && b["value"].is_number())
				maxv = std::max(maxv, b["value"].get<double>());
	if (maxv <= 0) maxv = 1;
	Elements rows;
	if (const std::string l = field(j, "label"); !l.empty())
		rows.push_back(text(l) | bold | color(col(th.p.text)));
	if (j.contains("bars"))
		for (const auto& b : j["bars"]) {
			const double val =
			    b.contains("value") && b["value"].is_number() ? b["value"].get<double>() : 0.0;
			const int w = static_cast<int>(std::lround(val / maxv * 24));
			Elements bar;
			for (int i = 0; i < 24; ++i)
				bar.push_back(text(" ") | bgcolor(col(i < w ? th.p.iris : th.p.hl_low)));
			rows.push_back(
			    hbox({text(field(b, "label")) | color(col(th.p.subtle)) | size(WIDTH, EQUAL, 12),
			          hbox(std::move(bar)),
			          text(" " + as_str(b.value("value", json(0)))) | color(col(th.p.foam))}));
		}
	return frame(vbox(std::move(rows)), th.p.hl_med);
}

Element checklist_widget(const theme& th, const json& j) {
	Elements rows;
	if (j.contains("title")) rows.push_back(text(field(j, "title")) | bold | color(col(th.p.text)));
	if (j.contains("items"))
		for (const auto& it : j["items"]) {
			const bool done = it.is_object() && it.value("done", false);
			const std::string txt = it.is_string() ? it.get<std::string>() : field(it, "text");
			rows.push_back(
			    hbox({text(done ? " [x] " : " [ ] ") | color(col(done ? th.p.foam : th.p.muted)),
			          text(txt) | color(col(done ? th.p.subtle : th.p.text))}));
		}
	return frame(vbox(std::move(rows)), th.p.hl_med);
}

}  // namespace

Element render_widget(const theme& th, const std::string& src) {
	const auto j = json::parse(src, nullptr, false);
	if (j.is_discarded() || !j.is_object())
		return frame(vbox({text(" widget ") | color(col(th.p.muted)) | dim,
		                   paragraph(src) | color(col(th.p.subtle))}),
		             th.p.hl_low);
	const std::string type = j.value("type", "");
	if (type == "weather") return weather_widget(th, j);
	if (type == "table") return table_widget(th, j);
	if (type == "progress") return progress_widget(th, j);
	if (type == "chart" || type == "bar") return chart_widget(th, j);
	if (type == "checklist" || type == "todo") return checklist_widget(th, j);
	return card_widget(th, j);  // "card" and anything unknown
}

}  // namespace plume::ui
