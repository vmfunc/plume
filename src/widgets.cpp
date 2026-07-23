// generative terminal widgets. the model emits a ```plume fenced block of json;
// plume renders it as a rich element. two ways to author one: a preset "type"
// (weather, table, card, progress, chart, checklist) for the common shapes, or a
// tree of layout primitives (vbox/hbox/text/bar/sparkline/box/...) for anything
// custom. the tree is untrusted model output, so rendering is capped in depth and
// node count and every field is optional — a malformed widget degrades, never
// crashes or hangs.
#include <algorithm>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "ui.hpp"

namespace plume::ui {

using namespace ftxui;
using json = nlohmann::json;

namespace {

constexpr int kMaxDepth = 14;     // deepest primitive nesting we will render
constexpr int kNodeBudget = 600;  // hard cap on total nodes per widget
constexpr int kMaxItems = 256;    // cap on rows/bars/items/values a preset builds

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
	// never dump() an untrusted object/array here: the serializer recurses per
	// level and a deep value would overflow the stack. show a placeholder instead.
	if (v.is_object()) return "{…}";
	if (v.is_array()) return "[…]";
	return "";
}

std::string field(const json& j, const char* k) {
	return j.contains(k) ? as_str(j[k]) : std::string{};
}

double num(const json& j, const char* k, double d) {
	return j.contains(k) && j[k].is_number() ? j[k].get<double>() : d;
}

rgb color_by_name(const theme& th, const std::string& n) {
	if (n == "iris") return th.p.iris;
	if (n == "foam") return th.p.foam;
	if (n == "gold") return th.p.gold;
	if (n == "love") return th.p.love;
	if (n == "rose") return th.p.rose;
	if (n == "pine") return th.p.pine;
	if (n == "subtle") return th.p.subtle;
	if (n == "muted") return th.p.muted;
	if (n == "base") return th.p.base;
	if (n == "surface") return th.p.surface;
	if (n == "hl") return th.p.hl_med;
	return th.p.text;
}

Element frame(Element body, rgb tint) {
	return body | borderRounded | color(col(tint)) | size(WIDTH, LESS_THAN, 64);
}

Element bar_of(const theme& th, double v, rgb tint, int width) {
	width = std::clamp(width, 1, 60);
	const int w = static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * width));
	Elements cells;
	for (int i = 0; i < width; ++i)
		cells.push_back(text(" ") | bgcolor(col(i < w ? tint : th.p.hl_low)));
	return hbox(std::move(cells));
}

Element sparkline(const theme& th, const json& j) {
	static const char* blocks[8] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
	std::vector<double> vs;
	double mn = 1e30, mx = -1e30;
	if (j.contains("values") && j["values"].is_array())
		for (const auto& v : j["values"]) {
			if (static_cast<int>(vs.size()) >= kMaxItems) break;  // cap untrusted length
			if (v.is_number()) {
				vs.push_back(v.get<double>());
				mn = std::min(mn, vs.back());
				mx = std::max(mx, vs.back());
			}
		}
	if (vs.empty()) return text("");
	const double range = mx > mn ? mx - mn : 1.0;
	std::string s;
	for (const double d : vs) {
		int lvl = static_cast<int>(std::lround((d - mn) / range * 7));
		s += blocks[std::clamp(lvl, 0, 7)];
	}
	return text(s) | color(col(color_by_name(th, j.value("color", "iris"))));
}

Element styled_text(const theme& th, const json& j) {
	Element e = text(field(j, "text")) | color(col(color_by_name(th, j.value("color", "text"))));
	const std::string st = j.value("style", "");
	if (st.find("bold") != std::string::npos) e = e | bold;
	if (st.find("dim") != std::string::npos) e = e | dim;
	if (st.find("italic") != std::string::npos) e = e | italic;
	if (j.contains("bg") && j["bg"].is_string()) e = e | bgcolor(col(color_by_name(th, j["bg"])));
	return e;
}

// -- preset composites --------------------------------------------------------

Element weather_widget(const theme& th, const json& j) {
	std::string temp = j.contains("temp_c")   ? as_str(j["temp_c"]) + "°C"
	                   : j.contains("temp_f") ? as_str(j["temp_f"]) + "°F"
	                                          : field(j, "temp");
	Elements rows = {hbox({text(field(j, "location")) | bold | color(col(th.p.text)), filler(),
	                       text(temp) | color(col(th.p.gold)) | bold}),
	                 text(field(j, "condition")) | color(col(th.p.foam))};
	if (j.contains("forecast") && j["forecast"].is_array()) {
		Elements days;
		int n = 0;
		for (const auto& d : j["forecast"]) {
			if (++n > kMaxItems) break;
			days.push_back(
			    vbox({text(field(d, "day")) | color(col(th.p.subtle)),
			          text(field(d, "hi") + "/" + field(d, "lo")) | color(col(th.p.muted))}) |
			    flex);
		}
		rows.push_back(separator() | color(col(th.p.hl_low)));
		rows.push_back(hbox(std::move(days)));
	}
	return frame(vbox(std::move(rows)), th.p.iris);
}

Element table_widget(const theme& th, const json& j) {
	Elements rows;
	if (j.contains("columns") && j["columns"].is_array()) {
		Elements hdr;
		int n = 0;
		for (const auto& c : j["columns"]) {
			if (++n > kMaxItems) break;
			hdr.push_back(text(as_str(c) + "  ") | bold | color(col(th.p.iris)) | flex);
		}
		rows.push_back(hbox(std::move(hdr)));
		rows.push_back(separator() | color(col(th.p.hl_med)));
	}
	if (j.contains("rows") && j["rows"].is_array()) {
		int n = 0;
		for (const auto& r : j["rows"]) {
			if (++n > kMaxItems) break;
			Elements cells;
			int m = 0;
			if (r.is_array())
				for (const auto& c : r) {
					if (++m > kMaxItems) break;
					cells.push_back(text(as_str(c) + "  ") | color(col(th.p.text)) | flex);
				}
			rows.push_back(hbox(std::move(cells)));
		}
	}
	return frame(vbox(std::move(rows)), th.p.hl_med);
}

Element card_widget(const theme& th, const json& j) {
	Elements rows;
	if (const std::string t = field(j, "title"); !t.empty()) {
		rows.push_back(text(t) | bold | color(col(th.p.text)));
		rows.push_back(separator() | color(col(th.p.hl_low)));
	}
	if (j.contains("fields") && j["fields"].is_object()) {
		int n = 0;
		for (const auto& [k, v] : j["fields"].items()) {
			if (++n > kMaxItems) break;
			rows.push_back(hbox({text(k) | color(col(th.p.muted)) | size(WIDTH, EQUAL, 16),
			                     text(as_str(v)) | color(col(th.p.foam))}));
		}
	}
	if (rows.empty()) rows.push_back(text("(empty card)") | color(col(th.p.muted)) | dim);
	return frame(vbox(std::move(rows)), th.p.iris);
}

Element progress_widget(const theme& th, const json& j) {
	const float v = static_cast<float>(num(j, "value", 0));
	return frame(
	    vbox({hbox({text(field(j, "label")) | color(col(th.p.text)), filler(),
	                text(std::to_string(static_cast<int>(v * 100)) + "%") | color(col(th.p.gold))}),
	          meter(th, v, 28)}),
	    th.p.hl_med);
}

Element chart_widget(const theme& th, const json& j) {
	double maxv = 0;
	int seen = 0;
	if (j.contains("bars") && j["bars"].is_array())
		for (const auto& b : j["bars"]) {
			if (++seen > kMaxItems) break;
			maxv = std::max(maxv, num(b, "value", 0));
		}
	if (maxv <= 0) maxv = 1;
	Elements rows;
	if (const std::string l = field(j, "label"); !l.empty())
		rows.push_back(text(l) | bold | color(col(th.p.text)));
	if (j.contains("bars") && j["bars"].is_array()) {
		int n = 0;
		for (const auto& b : j["bars"]) {
			if (++n > kMaxItems) break;
			rows.push_back(
			    hbox({text(field(b, "label")) | color(col(th.p.subtle)) | size(WIDTH, EQUAL, 12),
			          bar_of(th, num(b, "value", 0) / maxv, th.p.iris, 24),
			          text(" " + as_str(b.value("value", json(0)))) | color(col(th.p.foam))}));
		}
	}
	return frame(vbox(std::move(rows)), th.p.hl_med);
}

Element checklist_widget(const theme& th, const json& j) {
	Elements rows;
	if (j.contains("title")) rows.push_back(text(field(j, "title")) | bold | color(col(th.p.text)));
	if (j.contains("items") && j["items"].is_array()) {
		int n = 0;
		for (const auto& it : j["items"]) {
			if (++n > kMaxItems) break;
			const bool done = it.is_object() && it.value("done", false);
			const std::string txt = it.is_string() ? it.get<std::string>() : field(it, "text");
			rows.push_back(
			    hbox({text(done ? " [x] " : " [ ] ") | color(col(done ? th.p.foam : th.p.muted)),
			          text(txt) | color(col(done ? th.p.subtle : th.p.text))}));
		}
	}
	return frame(vbox(std::move(rows)), th.p.hl_med);
}

bool is_preset(const std::string& t) {
	return t == "weather" || t == "table" || t == "card" || t == "progress" || t == "chart" ||
	       t == "checklist" || t == "todo" || t == "box" || t == "panel";
}

// the recursive primitive renderer. depth and *budget guard against pathological
// (deeply nested / enormous) model output; both degrade to an ellipsis, no crash.
Element render_node(const theme& th, const json& j, int depth, int& budget) {
	if (--budget < 0 || depth > kMaxDepth) return text(" … ") | color(col(th.p.muted)) | dim;
	if (j.is_string()) return text(j.get<std::string>()) | color(col(th.p.text));
	if (j.is_number() || j.is_boolean()) return text(as_str(j)) | color(col(th.p.foam));
	if (j.is_array()) {
		Elements kids;
		for (const auto& c : j) kids.push_back(render_node(th, c, depth + 1, budget));
		return kids.empty() ? text("") : vbox(std::move(kids));
	}
	if (!j.is_object()) return text("");

	std::string type = j.value("type", "");
	if (type.empty())  // infer a sensible default from the shape
		type = j.contains("children")                          ? "vbox"
		       : (j.contains("fields") || j.contains("title")) ? "card"
		                                                       : "text";

	if (type == "weather") return weather_widget(th, j);
	if (type == "table") return table_widget(th, j);
	if (type == "card") return card_widget(th, j);
	if (type == "progress") return progress_widget(th, j);
	if (type == "chart" || type == "bars") return chart_widget(th, j);
	if (type == "checklist" || type == "todo") return checklist_widget(th, j);

	auto children = [&] {
		Elements e;
		if (j.contains("children") && j["children"].is_array())
			for (const auto& c : j["children"]) e.push_back(render_node(th, c, depth + 1, budget));
		return e;
	};
	if (type == "vbox" || type == "col" || type == "column") {
		auto e = children();
		return e.empty() ? text("") : vbox(std::move(e));
	}
	if (type == "hbox" || type == "row") {
		auto e = children();
		return e.empty() ? text("") : hbox(std::move(e));
	}
	if (type == "box" || type == "panel") {
		Element child =
		    j.contains("child") ? render_node(th, j["child"], depth + 1, budget) : vbox(children());
		Elements inner;
		if (const std::string t = field(j, "title"); !t.empty()) {
			inner.push_back(text(t) | bold | color(col(th.p.text)));
			inner.push_back(separator() | color(col(th.p.hl_low)));
		}
		inner.push_back(std::move(child));
		return frame(vbox(std::move(inner)), color_by_name(th, j.value("color", "hl")));
	}
	if (type == "text" || type == "label") return styled_text(th, j);
	if (type == "heading" || type == "title")
		return text(field(j, "text")) | bold |
		       color(col(color_by_name(th, j.value("color", "text"))));
	if (type == "bar" || type == "gauge" || type == "meter")
		return bar_of(th, num(j, "value", 0), color_by_name(th, j.value("color", "iris")),
		              static_cast<int>(num(j, "width", 24)));
	if (type == "sparkline") return sparkline(th, j);
	if (type == "separator" || type == "hr") return separator() | color(col(th.p.hl_low));
	if (type == "spacer" || type == "filler") return filler();
	if (type == "kv" || type == "field")
		return hbox({text(field(j, "key") + "  ") | color(col(th.p.muted)) | size(WIDTH, EQUAL, 16),
		             text(field(j, "value")) | color(col(th.p.foam))});
	if (type == "badge" || type == "pill")
		return pill(th, field(j, "text"), color_by_name(th, j.value("color", "iris")));

	if (j.contains("children")) return vbox(children());  // lenient: unknown container
	return text(as_str(j)) | color(col(th.p.subtle));
}

}  // namespace

Element render_widget(const theme& th, const std::string& src) {
	// nlohmann's DOM parser recurses per nesting level, so guard the parser itself
	// against absurdly deep untrusted input. count only STRUCTURAL brackets: a
	// string value full of ']' must not skew the depth (that was a real bypass).
	int depth = 0, worst = 0;
	bool in_str = false, esc = false;
	for (const char c : src) {
		if (in_str) {
			if (esc)
				esc = false;
			else if (c == '\\')
				esc = true;
			else if (c == '"')
				in_str = false;
			continue;
		}
		if (c == '"')
			in_str = true;
		else if (c == '{' || c == '[')
			worst = std::max(worst, ++depth);
		else if (c == '}' || c == ']')
			--depth;
	}
	const auto j = worst > 120 ? json(json::value_t::discarded) : json::parse(src, nullptr, false);
	if (j.is_discarded())
		return frame(vbox({text(" widget ") | color(col(th.p.muted)) | dim,
		                   paragraph(src) | color(col(th.p.subtle))}),
		             th.p.hl_low);
	int budget = kNodeBudget;
	Element e = render_node(th, j, 0, budget);
	// presets frame themselves; a bare primitive tree gets a light frame.
	const std::string type = j.is_object() ? j.value("type", "") : "";
	if (j.is_object() && is_preset(type)) return e;
	return frame(std::move(e), th.p.hl_med);
}

}  // namespace plume::ui
