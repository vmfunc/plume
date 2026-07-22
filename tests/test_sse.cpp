#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "plume/sse.hpp"

using namespace plume;

namespace {
std::vector<sse_event> collect_all(std::string_view whole, std::size_t chunk) {
	sse_parser p;
	std::vector<sse_event> got;
	auto sink = [&](const sse_event& e) { got.push_back(e); };
	for (std::size_t i = 0; i < whole.size(); i += chunk) p.feed(whole.substr(i, chunk), sink);
	p.finish(sink);
	return got;
}
}  // namespace

TEST_CASE("sse parses a well-formed event") {
	auto got = collect_all("event: message_start\ndata: {\"a\":1}\n\n", 999);
	REQUIRE(got.size() == 1);
	CHECK(got[0].type == "message_start");
	CHECK(got[0].data == "{\"a\":1}");
}

TEST_CASE("sse joins multiple data lines with newlines") {
	auto got = collect_all("data: line one\ndata: line two\n\n", 999);
	REQUIRE(got.size() == 1);
	CHECK(got[0].data == "line one\nline two");
}

TEST_CASE("sse survives byte-at-a-time delivery") {
	const std::string wire =
	    "event: content_block_delta\ndata: {\"t\":\"he\"}\n\n"
	    "event: content_block_delta\ndata: {\"t\":\"llo\"}\n\n";
	// feeding one byte per read must yield the same two events.
	auto got = collect_all(wire, 1);
	REQUIRE(got.size() == 2);
	CHECK(got[0].data == "{\"t\":\"he\"}");
	CHECK(got[1].data == "{\"t\":\"llo\"}");
}

TEST_CASE("sse handles crlf line endings, including a split pair") {
	// the \r\n after the first data line is split across two feeds.
	sse_parser p;
	std::vector<sse_event> got;
	auto sink = [&](const sse_event& e) { got.push_back(e); };
	p.feed("event: ping\r", sink);
	p.feed("\ndata: {}\r\n\r\n", sink);
	p.finish(sink);
	REQUIRE(got.size() == 1);
	CHECK(got[0].type == "ping");
	CHECK(got[0].data == "{}");
}

TEST_CASE("sse ignores comments and keepalives") {
	auto got = collect_all(": this is a keepalive\ndata: real\n\n", 999);
	REQUIRE(got.size() == 1);
	CHECK(got[0].data == "real");
}

TEST_CASE("sse strips exactly one leading space from a value") {
	auto got = collect_all("data:  two spaces\n\n", 999);
	REQUIRE(got.size() == 1);
	CHECK(got[0].data == " two spaces");
}
