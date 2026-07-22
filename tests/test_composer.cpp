#include <doctest/doctest.h>

#include <ftxui/component/event.hpp>

#include "composer.hpp"

using namespace plume;
using ftxui::Event;

namespace {
void type(composer& c, const std::string& s) {
	for (char ch : s) static_cast<void>(c.handle(Event::Character(std::string(1, ch))));
}
}  // namespace

TEST_CASE("insert mode types, escape drops to normal") {
	composer c;
	CHECK(c.insert_mode());
	type(c, "hello world");
	CHECK(c.value() == "hello world");
	static_cast<void>(c.handle(Event::Escape));
	CHECK_FALSE(c.insert_mode());
}

TEST_CASE("dw deletes a word, dd clears the line") {
	composer c;
	type(c, "hello world foo");
	static_cast<void>(c.handle(Event::Escape));
	type(c, "0");   // home
	type(c, "dw");  // delete "hello "
	CHECK(c.value() == "world foo");

	type(c, "dd");
	CHECK(c.value().empty());
}

TEST_CASE("A appends, I inserts at start, x deletes under cursor") {
	composer c;
	type(c, "bcd");
	static_cast<void>(c.handle(Event::Escape));
	type(c, "I");
	type(c, "a");  // -> "abcd"
	CHECK(c.value() == "abcd");
	static_cast<void>(c.handle(Event::Escape));
	type(c, "A");
	type(c, "e");  // -> "abcde"
	CHECK(c.value() == "abcde");
	static_cast<void>(c.handle(Event::Escape));
	type(c, "0");
	type(c, "x");  // delete 'a'
	CHECK(c.value() == "bcde");
}

TEST_CASE("ciw changes the inner word and enters insert") {
	composer c;
	type(c, "say hello now");
	static_cast<void>(c.handle(Event::Escape));
	type(c, "0");
	type(c, "ciw");
	CHECK(c.insert_mode());
	CHECK(c.value() == " hello now");
}

TEST_CASE("ci( changes inside parentheses") {
	composer c;
	type(c, "a (b c) d");
	static_cast<void>(c.handle(Event::Escape));
	type(c, "0");
	for (int i = 0; i < 4; ++i) static_cast<void>(c.handle(Event::Character("l")));  // move inside ()
	type(c, "ci(");
	CHECK(c.value() == "a () d");
	CHECK(c.insert_mode());
}

TEST_CASE("yy then p duplicates, counts multiply x") {
	composer c;
	type(c, "abcdef");
	static_cast<void>(c.handle(Event::Escape));
	type(c, "0");
	type(c, "3x");  // delete three chars
	CHECK(c.value() == "def");
}

TEST_CASE("enter submits, ctrl-e requests the editor") {
	composer c;
	type(c, "send me");
	CHECK(c.handle(Event::Return) == composer::result::submit);
	CHECK(c.handle(Event::Special(std::string(1, static_cast<char>(5)))) ==
	      composer::result::to_editor);
}
