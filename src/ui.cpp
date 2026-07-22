#include "ui.hpp"

#include <algorithm>
#include <cmath>

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

namespace {

std::string role_label(plume::role r) {
	return r == plume::role::user ? "you" : "plume";
}

Element body_block(const theme& th, const std::string& body) {
	// code fences render dim on the surface; everything else is plain prose.
	Elements lines;
	bool in_code = false;
	std::size_t start = 0;
	const std::string text_body = body.empty() ? " " : body;
	while (start <= text_body.size()) {
		const std::size_t nl = text_body.find('\n', start);
		const std::string ln =
		    text_body.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		if (ln.rfind("```", 0) == 0) {
			in_code = !in_code;
		} else if (in_code) {
			lines.push_back(text("  " + ln) | color(col(th.p.foam)) | bgcolor(col(th.p.surface)));
		} else {
			lines.push_back(paragraph(ln) | color(col(th.p.text)));
		}
		if (nl == std::string::npos) break;
		start = nl + 1;
	}
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
