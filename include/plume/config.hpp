// config is toml under xdg paths, documented in docs/config.md and hot-reloaded
// on save. this is the parsed shape the rest of the program reads; the loader
// fills in defaults for anything absent and only errors on genuinely malformed
// toml, never on a missing file.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "plume/error.hpp"
#include "plume/provider.hpp"

namespace plume {

struct ui_config {
	std::string theme = "rose-pine";
	bool reduce_motion = false;
	std::string density = "cozy";  // cozy | compact
	bool zen = false;              // hide all chrome
};

struct keys_config {
	std::string preset = "vim";  // vim | emacs
	// action -> key spec, loaded verbatim and resolved by the keymap layer.
	std::map<std::string, std::string> binds;
};

struct price {
	double input = 0;  // usd per 1e6 input tokens
	double output = 0;
	double cache_write = 0;
	double cache_read = 0;
};

struct provider_entry {
	std::string kind;  // anthropic, openai, openrouter, ollama, openai-compatible
	std::string base_url;
	std::string auth_source = "env";  // env | key_cmd | keychain | inline
	std::string auth_value;
	std::string default_model;
};

struct mcp_server_config {
	std::string name;
	std::string transport = "stdio";  // stdio | http
	std::string command;
	std::vector<std::string> args;
	std::string url;
	std::string approval = "ask";    // ask | allowlist | yolo
	std::vector<std::string> allow;  // tool names for the allowlist
};

struct config {
	ui_config ui;
	keys_config keys;
	std::map<std::string, provider_entry> providers;
	std::string default_provider;
	sampling_params defaults;             // model, max_tokens, thinking, effort
	std::map<std::string, price> prices;  // model id -> price, "verify against provider"
	std::vector<std::string> plugins;     // plugin dir names under plugins/
	std::vector<mcp_server_config> mcp;
	std::string notify = "bell";  // bell | osc9 | off

	// resolved xdg locations, filled by the loader.
	std::string config_dir;
	std::string data_dir;
	std::string state_dir;

	[[nodiscard]] const provider_entry* active_provider() const;
};

[[nodiscard]] config default_config();
[[nodiscard]] result<config> load_config(const std::string& path);
[[nodiscard]] std::string default_config_path();  // $XDG_CONFIG_HOME/plume/config.toml
[[nodiscard]] std::string default_data_path();    // $XDG_DATA_HOME/plume
[[nodiscard]] std::string default_state_path();   // $XDG_STATE_HOME/plume

}  // namespace plume
