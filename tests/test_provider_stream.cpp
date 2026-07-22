#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "plume/sse.hpp"
#include "provider/anthropic_stream.hpp"

using namespace plume;

namespace {
// a recorded anthropic messages stream: a thinking block, a text block, and a
// final message_delta carrying the stop reason and output tokens. shapes match
// the streaming docs.
constexpr const char* kFixture =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"model\":\"claude-opus-4-8\","
    "\"usage\":{\"input_tokens\":42,\"cache_read_input_tokens\":10}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\","
    "\"thinking\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
    "\"thinking\":\"let me think\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\","
    "\"signature\":\"abc123\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\","
    "\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\","
    "\"text\":\"a quill \"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\","
    "\"text\":\"is a pen\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":18}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";
}  // namespace

TEST_CASE("anthropic stream assembles thinking + text across byte-split reads") {
	std::vector<stream_delta> deltas;
	anthropic_detail::assembler a([&](const stream_delta& d) { deltas.push_back(d); });

	// drive the assembler through the sse parser one byte at a time — this is
	// the worst case the wire can throw at it.
	sse_parser parser;
	const std::string wire = kFixture;
	for (char c : wire) parser.feed(std::string_view(&c, 1), [&](const sse_event& e) { a.event(e); });
	parser.finish([&](const sse_event& e) { a.event(e); });
	a.finish();

	const completion& out = a.result();
	CHECK(out.model == "claude-opus-4-8");
	CHECK(out.stop_reason == "end_turn");
	CHECK(out.tokens.input == 42);
	CHECK(out.tokens.cache_read == 10);
	CHECK(out.tokens.output == 18);

	REQUIRE(out.reply.blocks.size() == 2);
	const auto& think = std::get<thinking_block>(out.reply.blocks[0]);
	CHECK(think.thinking == "let me think");
	CHECK(think.signature == "abc123");
	const auto& text = std::get<text_block>(out.reply.blocks[1]);
	CHECK(text.text == "a quill is a pen");

	// the ui saw incremental text deltas and a terminal done delta.
	bool saw_text = false, saw_done = false;
	for (const auto& d : deltas) {
		if (d.type == stream_delta::kind::text) saw_text = true;
		if (d.type == stream_delta::kind::done) saw_done = true;
	}
	CHECK(saw_text);
	CHECK(saw_done);
}

TEST_CASE("anthropic stream surfaces an error event") {
	anthropic_detail::assembler a([](const stream_delta&) {});
	sse_parser parser;
	const std::string wire =
	    "event: error\n"
	    "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
	    "\"message\":\"overloaded\"}}\n\n";
	parser.feed(wire, [&](const sse_event& e) { a.event(e); });
	parser.finish([&](const sse_event& e) { a.event(e); });
	CHECK(a.error() == "overloaded");
}
