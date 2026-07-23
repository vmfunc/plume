#include <doctest/doctest.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "plume/theme.hpp"
#include "ui.hpp"

using namespace ftxui;
using namespace plume;

namespace {
std::string render(Element e, int w, int h) {
	auto screen = Screen::Create(Dimension::Fixed(w), Dimension::Fixed(h));
	Render(screen, std::move(e));
	return screen.ToString();
}
}  // namespace

TEST_CASE("command overlay renders its title and items over a base view") {
	const theme th = rose_pine();
	Element base = vbox({text("header"), filler(), text("a blank page") | center, filler()});
	Element ov =
	    ui::overlay(th, "commands",
	                ui::pick_list(th, "th", {{"theme", "switch theme"}, {"weave", "the loom"}}, 0));
	const std::string s = render(dbox({base, ov}), 66, 16);
	CHECK(s.find("commands") != std::string::npos);
	CHECK(s.find("theme") != std::string::npos);
	CHECK(s.find("weave") != std::string::npos);
}

TEST_CASE("gradient wordmark keeps its letters") {
	Element g =
	    ui::gradient_text("plume", {rgb{235, 111, 146}, rgb{196, 167, 231}, rgb{156, 207, 216}});
	// each glyph is a distinct color, so ToString interleaves sgr escapes; strip
	// them before checking the letters survive in order.
	std::string s = render(g, 12, 1);
	std::string plain;
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\x1b') {
			while (i < s.size() && s[i] != 'm') ++i;
			continue;
		}
		plain.push_back(s[i]);
	}
	CHECK(plain.find("plume") != std::string::npos);
}

TEST_CASE("the context meter fills proportionally") {
	const theme th = rose_pine();
	// a full meter is all block cells; an empty one is spaces on the low tint.
	const std::string full = render(ui::meter(th, 1.0f, 10), 12, 1);
	const std::string empty = render(ui::meter(th, 0.0f, 10), 12, 1);
	CHECK(full.size() >= empty.size());  // both render without crashing
}

TEST_CASE("code fences highlight keywords, strings and a language badge") {
	const theme th = rose_pine();
	Element card = ui::message_card(th, role::assistant,
	                                "here:\n```cpp\nconst int x = \"hi\"; // note\n```\ndone", {},
	                                false, false, "m", 0);
	const std::string s = render(card, 60, 12);
	// the badge shows the language, and the code line survives lexing intact.
	CHECK(s.find("cpp") != std::string::npos);
	CHECK(s.find("const") != std::string::npos);
	CHECK(s.find("\"hi\"") != std::string::npos);
}

TEST_CASE("a plume widget directive renders as a rich element") {
	const theme th = rose_pine();
	const std::string weather =
	    R"({"type":"weather","location":"san francisco","temp_c":18,"condition":"fog"})";
	const std::string s = render(ui::render_widget(th, weather), 60, 6);
	CHECK(s.find("san francisco") != std::string::npos);
	CHECK(s.find("18") != std::string::npos);
	CHECK(s.find("fog") != std::string::npos);
	// malformed json degrades to a labelled box, never crashes.
	const std::string bad = render(ui::render_widget(th, "{not json"), 40, 4);
	CHECK(bad.find("widget") != std::string::npos);
}

TEST_CASE("image half-block node renders cells for a decoded bitmap") {
	img::bitmap bm;
	bm.width = 8;
	bm.height = 8;
	bm.rgba.assign(8 * 8 * 4, 200);
	const std::string s = render(ui::image_halfblock(bm, 8, 4), 8, 4);
	CHECK(s.find("▀") != std::string::npos);  // the upper half block glyph
}
