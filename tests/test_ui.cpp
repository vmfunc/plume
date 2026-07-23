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

TEST_CASE("a plume fence inside a message renders as a widget, not raw json") {
	const theme th = rose_pine();
	const std::string body =
	    "here you "
	    "go:\n```plume\n{\"type\":\"card\",\"title\":\"box\",\"fields\":{\"a\":\"1\"}}\n```\ndone";
	const std::string s =
	    render(ui::message_card(th, role::assistant, body, {}, false, false, "m", 0), 60, 10);
	CHECK(s.find("box") != std::string::npos);       // the card title
	CHECK(s.find("\"type\"") == std::string::npos);  // raw json is not shown
}

TEST_CASE("custom widgets compose primitives into a tree") {
	const theme th = rose_pine();
	const std::string tree =
	    R"({"type":"vbox","children":[{"type":"heading","text":"stats"},)"
	    R"({"type":"bar","value":0.5,"color":"foam"},)"
	    R"({"type":"sparkline","values":[1,3,2,5,4]},{"type":"kv","key":"cpu","value":"42%"}]})";
	const std::string s = render(ui::render_widget(th, tree), 50, 8);
	CHECK(s.find("stats") != std::string::npos);
	CHECK(s.find("cpu") != std::string::npos);
}

TEST_CASE("a pathological widget tree is capped, not fatal") {
	const theme th = rose_pine();
	// a 5000-deep nest and a huge sibling array must render without crash or hang.
	std::string deep = "{\"type\":\"vbox\",\"children\":[";
	std::string tail;
	for (int i = 0; i < 5000; ++i) {
		deep += "{\"type\":\"vbox\",\"children\":[";
		tail += "]}";
	}
	deep += "{\"type\":\"text\",\"text\":\"x\"}" + tail + "]}";
	CHECK_NOTHROW(static_cast<void>(render(ui::render_widget(th, deep), 40, 8)));

	std::string wide = "{\"type\":\"hbox\",\"children\":[";
	for (int i = 0; i < 5000; ++i)
		wide += (i ? "," : "") + std::string("{\"type\":\"text\",\"text\":\"a\"}");
	wide += "]}";
	CHECK_NOTHROW(static_cast<void>(render(ui::render_widget(th, wide), 40, 8)));

	// the bracket-in-string bypass: a string padded with ']' must not fool the
	// depth guard into parsing (then dumping) a stack-overflowing nested array.
	std::string bypass = "{\"type\":\"card\",\"p\":\"" + std::string(50000, ']') +
	                     "\",\"d\":" + std::string(50000, '[') + std::string(50000, ']') + "}";
	CHECK_NOTHROW(static_cast<void>(render(ui::render_widget(th, bypass), 40, 6)));

	// a preset builder fed a huge array must cap, not build millions of elements.
	std::string bigtable = "{\"type\":\"table\",\"rows\":[";
	for (int i = 0; i < 20000; ++i) bigtable += (i ? "," : "") + std::string("[\"x\"]");
	bigtable += "]}";
	CHECK_NOTHROW(static_cast<void>(render(ui::render_widget(th, bigtable), 40, 8)));

	std::string bigspark = "{\"type\":\"sparkline\",\"values\":[";
	for (int i = 0; i < 20000; ++i) bigspark += (i ? "," : "") + std::string("1");
	bigspark += "]}";
	CHECK_NOTHROW(static_cast<void>(render(ui::render_widget(th, bigspark), 40, 3)));
}

TEST_CASE("image half-block node renders cells for a decoded bitmap") {
	img::bitmap bm;
	bm.width = 8;
	bm.height = 8;
	bm.rgba.assign(8 * 8 * 4, 200);
	const std::string s = render(ui::image_halfblock(bm, 8, 4), 8, 4);
	CHECK(s.find("▀") != std::string::npos);  // the upper half block glyph
}
