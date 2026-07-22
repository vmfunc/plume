// luajit plugins through sol2, a neovim-shaped surface. plugins register
// commands, keymaps, statusline segments, themes and slash commands, and hook
// the message lifecycle. they are sandboxed: no io/os unless the plugin.toml
// manifest declares the capability (net, fs, exec) and the user approves it on
// first load.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "plume/error.hpp"

namespace plume {

struct plugin_manifest {
	std::string name;
	std::string version;
	std::string entry = "init.lua";
	std::vector<std::string> capabilities;  // net, fs, exec
};

struct statusline_segment {
	std::string id;
	std::string text;
};

// asked once per capability a plugin requests, on first load. returning false
// loads the plugin with that capability withheld.
using approval_fn = std::function<bool(const std::string& plugin, const std::string& capability)>;

class plugin_host {
   public:
	[[nodiscard]] static result<std::unique_ptr<plugin_host>> create();
	~plugin_host();
	plugin_host(plugin_host&&) noexcept;
	plugin_host& operator=(plugin_host&&) noexcept;

	// load every plugin directory under root, honoring manifests and approval.
	[[nodiscard]] result<void> load_all(const std::string& root, const approval_fn&);

	// the host injects a way for net-approved plugins to call a model, so
	// plume.model.complete works without the plugin holding a provider itself.
	void set_model(std::function<std::string(const std::string& prompt, const std::string& model)>);

	// lifecycle hooks. pre_send may rewrite the outgoing text; on_key returns
	// true if the plugin consumed the key.
	[[nodiscard]] std::string run_pre_send(std::string outgoing);
	void run_on_chunk(std::string_view delta);
	void run_post_receive(std::string_view full);
	[[nodiscard]] bool run_on_key(std::string_view key);

	// a slash/plugin command; nullopt if no plugin claims it.
	[[nodiscard]] std::optional<std::string> run_command(std::string_view name,
	                                                     std::string_view args);

	[[nodiscard]] std::vector<statusline_segment> statusline() const;
	[[nodiscard]] std::vector<std::string> commands() const;

   private:
	plugin_host();
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

}  // namespace plume
