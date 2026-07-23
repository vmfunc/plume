#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace plume::ui {

using namespace ftxui;

Color col(rgb c) {
	return Color::RGB(c.r, c.g, c.b);
}

rgb lerp(rgb a, rgb b, float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	auto mix = [&](std::uint8_t x, std::uint8_t y) {
		return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
	};
	return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

rgb lerp(const std::vector<rgb>& stops, float t) {
	if (stops.empty()) return {};
	if (stops.size() == 1) return stops.front();
	t = std::clamp(t, 0.0f, 1.0f);
	const float scaled = t * (stops.size() - 1);
	const auto i = std::min(static_cast<std::size_t>(scaled), stops.size() - 2);
	return lerp(stops[i], stops[i + 1], scaled - static_cast<float>(i));
}

Element gradient_text(std::string_view s, std::vector<rgb> stops) {
	Elements cells;
	for (std::size_t i = 0; i < s.size(); ++i) {
		const float t = s.size() > 1 ? static_cast<float>(i) / (s.size() - 1) : 0.0f;
		cells.push_back(text(std::string(1, s[i])) | color(col(lerp(stops, t))));
	}
	return hbox(std::move(cells));
}

img::bitmap splash_bitmap(int w, int h, const theme& th) {
	img::bitmap bm;
	bm.width = w;
	bm.height = h;
	bm.rgba.resize(static_cast<std::size_t>(w) * h * 4);
	// a calm rosé pine aurora: a smooth love -> rose -> iris -> foam -> pine band
	// across the width, brightest through the middle and easing into the base at
	// the top and bottom edges, with a faint diagonal drift. no hard seams.
	const std::vector<rgb> band = {th.p.love, th.p.rose, th.p.iris, th.p.foam, th.p.pine};
	for (int y = 0; y < h; ++y) {
		const float fy = h > 1 ? static_cast<float>(y) / (h - 1) : 0.5f;
		const float env = 0.30f + 0.70f * std::sin(fy * 3.14159265f);  // edge -> middle -> edge
		for (int x = 0; x < w; ++x) {
			const float fx = w > 1 ? static_cast<float>(x) / (w - 1) : 0.0f;
			const float t = std::clamp(fx + (fy - 0.5f) * 0.18f, 0.0f, 1.0f);
			const rgb c = lerp(th.p.base, lerp(band, t), env);
			auto* p = &bm.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
			p[0] = c.r;
			p[1] = c.g;
			p[2] = c.b;
			p[3] = 255;
		}
	}
	return bm;
}

namespace {

class image_node : public Node {
   public:
	image_node(img::bitmap bm, int cols, int rows) : bm_(std::move(bm)), cols_(cols), rows_(rows) {}

	void ComputeRequirement() override {
		requirement_.min_x = cols_;
		requirement_.min_y = rows_;
	}

	void Render(Screen& screen) override {
		const int w = box_.x_max - box_.x_min + 1;
		const int h = box_.y_max - box_.y_min + 1;
		if (w <= 0 || h <= 0 || bm_.width <= 0 || bm_.height <= 0) return;
		auto at = [&](int cx, int cy) -> rgb {
			const int sx = std::clamp(bm_.width * cx / w, 0, bm_.width - 1);
			const int sy = std::clamp(bm_.height * cy / (h * 2), 0, bm_.height - 1);
			const auto* p = &bm_.rgba[(static_cast<std::size_t>(sy) * bm_.width + sx) * 4];
			return {p[0], p[1], p[2]};
		};
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) {
				const rgb top = at(x, 2 * y);
				const rgb bot = at(x, 2 * y + 1);
				Pixel& px = screen.PixelAt(box_.x_min + x, box_.y_min + y);
				px.character = "▀";  // upper half: fg is the top pixel, bg the bottom
				px.foreground_color = Color::RGB(top.r, top.g, top.b);
				px.background_color = Color::RGB(bot.r, bot.g, bot.b);
			}
	}

   private:
	img::bitmap bm_;
	int cols_, rows_;
};

}  // namespace

Element image_halfblock(const img::bitmap& bm, int max_cols, int max_rows) {
	const double aspect = bm.height > 0 ? static_cast<double>(bm.width) / bm.height : 1.0;
	int cols = max_cols;
	int rows = std::max(1, static_cast<int>(std::lround(cols / aspect / 2.0)));
	if (rows > max_rows) {
		rows = max_rows;
		cols = std::max(1, static_cast<int>(std::lround(rows * aspect * 2.0)));
	}
	cols = std::clamp(cols, 1, max_cols);
	rows = std::clamp(rows, 1, max_rows);
	return std::make_shared<image_node>(bm, cols, rows);
}

float breathe(std::int64_t ms, int period_ms) {
	const float t = static_cast<float>(ms % period_ms) / static_cast<float>(period_ms);
	return 0.5f - 0.5f * std::cos(t * 2.0f * 3.14159265f);  // eased 0..1..0
}

std::string spinner(std::int64_t ms) {
	static const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
	return frames[(ms / 80) % 10];
}

Element caret(const theme& th, std::int64_t ms, bool reduce_motion) {
	if (reduce_motion) return text("▌") | color(col(th.p.iris));
	const rgb c = lerp(th.p.muted, th.p.iris, breathe(ms, 1100));
	return text("▌") | color(col(c));
}

Element meter(const theme& th, float frac, int width) {
	frac = std::clamp(frac, 0.0f, 1.0f);
	const int filled = static_cast<int>(std::lround(frac * width));
	Elements cells;
	for (int i = 0; i < width; ++i) {
		const float p = width > 1 ? static_cast<float>(i) / (width - 1) : 0.0f;
		if (i < filled) {
			// pine -> gold across the first half, gold -> love across the second.
			const rgb c = p < 0.5f ? lerp(th.p.pine, th.p.gold, p * 2.0f)
			                       : lerp(th.p.gold, th.p.love, (p - 0.5f) * 2.0f);
			cells.push_back(text(" ") | bgcolor(col(c)));
		} else {
			cells.push_back(text(" ") | bgcolor(col(th.p.hl_low)));
		}
	}
	return hbox(std::move(cells));
}

Element pill(const theme& th, const std::string& label, rgb fg) {
	return text(" " + label + " ") | color(col(fg)) | bgcolor(col(th.p.overlay));
}

Element overlay(const theme& th, const std::string& title, Element body) {
	Element card = vbox({
	                   hbox({gradient_text("plume", {th.p.love, th.p.iris, th.p.foam}) | bold,
	                         text("  " + title) | color(col(th.p.gold))}),
	                   separator() | color(col(th.p.hl_med)),
	                   body,
	               }) |
	               borderRounded | color(col(th.p.hl_high)) | bgcolor(col(th.p.surface)) |
	               size(WIDTH, GREATER_THAN, 52) | size(WIDTH, LESS_THAN, 88) |
	               size(HEIGHT, LESS_THAN, 24);
	return vbox({filler(), hbox({filler(), card, filler()}), filler()});
}

Element pick_list(const theme& th, const std::string& filter,
                  const std::vector<std::pair<std::string, std::string>>& items, int sel) {
	Elements rows;
	rows.push_back(hbox({text("› ") | color(col(th.p.iris)) | bold,
	                     text(filter) | color(col(th.p.text)), text("▏") | color(col(th.p.iris))}));
	rows.push_back(text(""));
	for (std::size_t i = 0; i < items.size(); ++i) {
		const bool on = static_cast<int>(i) == sel;
		Element row = hbox({text(on ? "  ▸ " : "    ") | color(col(on ? th.p.iris : th.p.muted)),
		                    text(items[i].first) | color(col(on ? th.p.text : th.p.subtle)) |
		                        size(WIDTH, EQUAL, 16),
		                    text(items[i].second) | color(col(th.p.muted)) | dim});
		if (on) row = row | bgcolor(col(th.p.hl_low));
		rows.push_back(row);
	}
	return vbox(std::move(rows));
}

namespace {

std::string role_label(plume::role r) {
	return r == plume::role::user ? "you" : "plume";
}

bool ident_char(char c) {
	const auto u = static_cast<unsigned char>(c);
	return std::isalnum(u) || c == '_';
}

// a built-in, language-agnostic lexer for code blocks. tree-sitter is linked but
// per-grammar wiring is out of this build's scope (flagged experimental in the
// docs); this covers strings, comments, numbers and a common keyword set well
// enough to read code by.
Element highlight_line(const theme& th, const std::string& ln) {
	static const std::unordered_set<std::string> kw = {
	    "if",        "else",      "elif",       "for",       "while",     "do",       "return",
	    "break",     "continue",  "switch",     "case",      "default",   "class",    "struct",
	    "enum",      "union",     "public",     "private",   "protected", "virtual",  "override",
	    "const",     "constexpr", "static",     "void",      "int",       "float",    "double",
	    "char",      "bool",      "auto",       "namespace", "template",  "typename", "using",
	    "new",       "delete",    "this",       "true",      "false",     "null",     "nullptr",
	    "none",      "None",      "True",       "False",     "def",       "fn",       "func",
	    "let",       "var",       "mut",        "pub",       "impl",      "trait",    "match",
	    "import",    "from",      "as",         "async",     "await",     "yield",    "lambda",
	    "try",       "except",    "finally",    "raise",     "throw",     "catch",    "package",
	    "interface", "extends",   "implements", "type",      "where",     "and",      "or",
	    "not",       "in",        "is",         "with",      "self",      "go",       "defer"};
	Elements spans;
	const std::size_t n = ln.size();
	std::size_t i = 0;
	auto push = [&](std::size_t a, std::size_t b, rgb c) {
		spans.push_back(text(ln.substr(a, b - a)) | color(col(c)));
	};
	while (i < n) {
		const char c = ln[i];
		if ((c == '/' && i + 1 < n && ln[i + 1] == '/') || c == '#') {
			push(i, n, th.p.muted);
			break;
		}
		if (c == '"' || c == '\'' || c == '`') {
			std::size_t j = i + 1;
			while (j < n && ln[j] != c) j += (ln[j] == '\\' ? 2 : 1);
			j = std::min(j + 1, n);
			push(i, j, th.p.gold);
			i = j;
		} else if (std::isdigit(static_cast<unsigned char>(c))) {
			std::size_t j = i;
			while (j < n && (std::isalnum(static_cast<unsigned char>(ln[j])) || ln[j] == '.')) ++j;
			push(i, j, th.p.foam);
			i = j;
		} else if (ident_char(c) && !std::isdigit(static_cast<unsigned char>(c))) {
			std::size_t j = i;
			while (j < n && ident_char(ln[j])) ++j;
			push(i, j, kw.contains(ln.substr(i, j - i)) ? th.p.iris : th.p.text);
			i = j;
		} else {
			std::size_t j = i + 1;
			while (j < n && !ident_char(ln[j]) && ln[j] != '"' && ln[j] != '\'' && ln[j] != '`' &&
			       ln[j] != '#' && !(ln[j] == '/' && j + 1 < n && ln[j + 1] == '/'))
				++j;
			push(i, j, th.p.subtle);
			i = j;
		}
	}
	if (spans.empty()) spans.push_back(text(" "));
	return hbox(std::move(spans)) | bgcolor(col(th.p.surface));
}

Element body_block(const theme& th, const std::string& body) {
	Elements lines;
	bool in_code = false;
	bool in_widget = false;  // inside a ```plume widget directive
	std::string widget_src;
	std::size_t start = 0;
	const std::string text_body = body.empty() ? " " : body;
	while (start <= text_body.size()) {
		const std::size_t nl = text_body.find('\n', start);
		const std::string ln =
		    text_body.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		if (ln.rfind("```", 0) == 0) {
			const std::string lang = ln.substr(3);
			if (in_widget) {  // closing fence: render the accumulated directive
				in_widget = false;
				lines.push_back(render_widget(th, widget_src));
			} else if (!in_code && (lang == "plume" || lang == "plume-widget")) {
				in_widget = true;
				widget_src.clear();
			} else {
				in_code = !in_code;
				if (in_code && !lang.empty())
					lines.push_back(hbox({text(" " + lang + " ") | color(col(th.p.base)) |
					                      bgcolor(col(th.p.pine))}));  // language badge
			}
		} else if (in_widget) {
			widget_src += ln + "\n";
		} else if (in_code) {
			lines.push_back(hbox({text("  "), highlight_line(th, ln)}));
		} else {
			lines.push_back(paragraph(ln) | color(col(th.p.text)));
		}
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
	if (in_widget)  // still streaming in: show a quiet placeholder
		lines.push_back(text("  … building widget") | color(col(th.p.muted)) | dim);
	return vbox(std::move(lines));
}

Element rule_card(const theme& th, plume::role r, Element header, Element body, bool compact) {
	const rgb accent = r == plume::role::user ? th.p.pine : th.p.iris;
	Element content = vbox({header, body | flex});
	Element card = hbox({text("▏") | color(col(accent)), text(" "), content | flex});
	return compact ? card : vbox({card, text("")});
}

}  // namespace

Element message_card(const theme& th, plume::role r, const std::string& body,
                     const std::vector<std::string>& thinking, bool show_think, bool compact,
                     const std::string& model, std::int64_t tokens_out) {
	const rgb accent = r == plume::role::user ? th.p.pine : th.p.iris;
	Elements meta;
	if (!model.empty()) meta.push_back(text(model + " ") | color(col(th.p.muted)) | dim);
	if (tokens_out > 0)
		meta.push_back(text(std::to_string(tokens_out) + "t") | color(col(th.p.muted)) | dim);
	Element header = hbox({text(role_label(r)) | bold | color(col(accent)), filler(), hbox(meta)});

	Elements stack{body_block(th, body)};
	if (show_think)
		for (const auto& t : thinking)
			stack.push_back(hbox({text("  thinking ") | color(col(th.p.muted)) | dim | italic,
			                      paragraph(t) | color(col(th.p.muted)) | dim}));
	return rule_card(th, r, header, vbox(std::move(stack)), compact);
}

Element streaming_card(const theme& th, const std::string& text_in, const std::string& thinking,
                       bool show_think, bool compact, std::int64_t ms, bool reduce_motion) {
	Element header = hbox({text("plume") | bold | color(col(th.p.iris)), text("  "),
	                       text(spinner(ms)) | color(col(th.p.foam)), filler()});
	Elements stack;
	if (show_think && !thinking.empty())
		stack.push_back(hbox({text("  thinking ") | color(col(th.p.muted)) | dim | italic,
		                      paragraph(thinking) | color(col(th.p.muted)) | dim}));
	stack.push_back(hbox({body_block(th, text_in) | flex, caret(th, ms, reduce_motion)}));
	return rule_card(th, plume::role::assistant, header, vbox(std::move(stack)), compact);
}

}  // namespace plume::ui
