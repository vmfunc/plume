#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <set>

#include "plume/ids.hpp"
#include "plume/plugin.hpp"

using namespace plume;
namespace fs = std::filesystem;

namespace {
fs::path make_plugin(const std::string& name, const std::string& caps, const std::string& lua) {
	fs::path root = fs::temp_directory_path() / ("plume-plug-" + new_id("t"));
	fs::create_directories(root / name);
	std::ofstream(root / name / "plugin.toml")
	    << "name = \"" << name << "\"\nversion = \"1\"\n"
	    << "entry = \"init.lua\"\ncapabilities = " << caps << "\n";
	std::ofstream(root / name / "init.lua") << lua;
	return root;
}

approval_fn grant(std::set<std::string> caps) {
	return [caps = std::move(caps)](const std::string&, const std::string& c) {
		return caps.contains(c);
	};
}
}  // namespace

TEST_CASE("wordcount plugin registers a statusline segment fed by a hook") {
	auto root = make_plugin("wordcount", "[]", R"(
		local n = 0
		plume.on("post_receive", function(full)
			n = 0
			for _ in full:gmatch("%S+") do n = n + 1 end
		end)
		plume.statusline("wordcount", function() return n .. "w" end)
	)");
	auto host = plugin_host::create();
	REQUIRE(host.has_value());
	REQUIRE((*host)->load_all(root.string(), grant({})).has_value());

	(*host)->run_post_receive("one two three four");
	auto segs = (*host)->statusline();
	REQUIRE(segs.size() == 1);
	CHECK(segs[0].id == "wordcount");
	CHECK(segs[0].text == "4w");
	fs::remove_all(root);
}

TEST_CASE("a net-approved plugin can mutate the outgoing message via a model") {
	auto root = make_plugin("translate", "[\"net\"]", R"(
		plume.on("pre_send", function(text)
			return plume.model.complete({ prompt = text })
		end)
	)");
	auto host = plugin_host::create();
	REQUIRE(host.has_value());
	// the host injects the model; the plugin only sees plume.model.complete.
	(*host)->set_model(
	    [](const std::string& prompt, const std::string&) { return "bonjour: " + prompt; });
	REQUIRE((*host)->load_all(root.string(), grant({"net"})).has_value());

	CHECK((*host)->run_pre_send("hello") == "bonjour: hello");
	fs::remove_all(root);
}

TEST_CASE("net is withheld unless approved, so the model call is refused") {
	auto root = make_plugin("translate", "[\"net\"]", R"(
		plume.on("pre_send", function(text)
			local out = plume.model.complete({ prompt = text })
			if out:sub(1,6) == "[plume" then return text end
			return out
		end)
	)");
	auto host = plugin_host::create();
	REQUIRE(host.has_value());
	(*host)->set_model([](const std::string&, const std::string&) { return "should not run"; });
	// deny net: plume.model.complete returns the unavailable marker.
	REQUIRE((*host)->load_all(root.string(), grant({})).has_value());
	CHECK((*host)->run_pre_send("hello") == "hello");  // unchanged
	fs::remove_all(root);
}

TEST_CASE("io is absent from a plugin that did not earn the fs capability") {
	const char* lua = R"(
		plume.on("pre_send", function(t)
			if io == nil then return "NOIO" else return "HASIO" end
		end)
	)";
	{
		auto root = make_plugin("nofs", "[]", lua);
		auto host = plugin_host::create();
		REQUIRE((*host)->load_all(root.string(), grant({})).has_value());
		CHECK((*host)->run_pre_send("x") == "NOIO");
		fs::remove_all(root);
	}
	{
		auto root = make_plugin("withfs", "[\"fs\"]", lua);
		auto host = plugin_host::create();
		REQUIRE((*host)->load_all(root.string(), grant({"fs"})).has_value());
		CHECK((*host)->run_pre_send("x") == "HASIO");
		fs::remove_all(root);
	}
}
