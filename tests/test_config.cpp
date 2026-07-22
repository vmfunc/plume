#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "plume/config.hpp"
#include "plume/ids.hpp"

using namespace plume;
namespace fs = std::filesystem;

TEST_CASE("config written by the wizard round-trips through the loader") {
	config c = default_config();
	c.ui.theme = "va11";
	c.ui.density = "compact";
	c.ui.reduce_motion = true;
	c.keys.preset = "emacs";
	c.default_provider = "anthropic";
	c.defaults.model = "claude-opus-4-8";
	c.defaults.thinking = thinking_mode::adaptive;
	c.defaults.effort = "high";
	c.providers["anthropic"] = {"anthropic", "", "env", "ANTHROPIC_API_KEY", "claude-opus-4-8"};

	const std::string path =
	    (fs::temp_directory_path() / ("plume-cfg-" + new_id("t") + ".toml")).string();
	REQUIRE(save_config(c, path).has_value());

	auto back = load_config(path);
	REQUIRE(back.has_value());
	CHECK(back->ui.theme == "va11");
	CHECK(back->ui.density == "compact");
	CHECK(back->ui.reduce_motion == true);
	CHECK(back->keys.preset == "emacs");
	CHECK(back->default_provider == "anthropic");
	CHECK(back->defaults.model == "claude-opus-4-8");
	CHECK(back->defaults.thinking == thinking_mode::adaptive);
	CHECK(back->defaults.effort == "high");

	REQUIRE(back->providers.contains("anthropic"));
	const auto& p = back->providers.at("anthropic");
	CHECK(p.kind == "anthropic");
	CHECK(p.auth_source == "env");
	CHECK(p.auth_value == "ANTHROPIC_API_KEY");
	CHECK(p.default_model == "claude-opus-4-8");

	fs::remove(path);
}

TEST_CASE("an inline key is flagged in plaintext, never silent") {
	config c = default_config();
	c.default_provider = "openai";
	c.providers["openai"] = {"openai", "", "inline", "sk-secret", ""};
	const std::string path =
	    (fs::temp_directory_path() / ("plume-cfg-" + new_id("t") + ".toml")).string();
	REQUIRE(save_config(c, path).has_value());

	std::ifstream in(path);
	std::string body((std::istreambuf_iterator<char>(in)), {});
	CHECK(body.find("plaintext") != std::string::npos);  // the visible warning
	fs::remove(path);
}
