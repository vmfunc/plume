#include <doctest/doctest.h>

#include "plume/codec.hpp"

using namespace plume;

TEST_CASE("blocks round-trip through json") {
	std::vector<content_block> in{
	    text_block{"hello"},
	    thinking_block{"pondering", "sig123"},
	    tool_use_block{"toolu_1", "get_weather", R"({"city":"sf"})"},
	    tool_result_block{"toolu_1", "72f", false},
	};
	auto enc = codec::encode_blocks(in);
	auto dec = codec::decode_blocks(enc);
	REQUIRE(dec.has_value());
	REQUIRE(dec->size() == 4);
	CHECK(std::get<text_block>((*dec)[0]).text == "hello");
	CHECK(std::get<thinking_block>((*dec)[1]).signature == "sig123");
	CHECK(std::get<tool_use_block>((*dec)[2]).name == "get_weather");
	CHECK(std::get<tool_result_block>((*dec)[3]).tool_use_id == "toolu_1");
}

TEST_CASE("decode rejects non-array") {
	auto dec = codec::decode_blocks("{\"not\":\"array\"}");
	CHECK_FALSE(dec.has_value());
}

TEST_CASE("params_json patches one field at a time") {
	std::string obj = "{}";
	obj = codec::patch_str(obj, "label", "risky path");
	obj = codec::patch_bool(obj, "bookmark", true);
	CHECK(codec::read_str(obj, "label").value_or("") == "risky path");
	CHECK(codec::read_bool(obj, "bookmark").value_or(false) == true);
	// an unrelated field survives a later patch.
	obj = codec::patch_str(obj, "model", "some-model");
	CHECK(codec::read_str(obj, "label").value_or("") == "risky path");
}
