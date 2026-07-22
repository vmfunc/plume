// the luajit plugin host. the surface is neovim-shaped: plugins call plume.on
// to hook the message lifecycle, plume.command / plume.keymap / plume.statusline
// to register, and plume.model.complete to call a model. the sandbox is by
// omission — io and os are never in a plugin's environment unless its manifest
// declares fs / exec and the user approves. net gates plume.model.
#include "plume/plugin.hpp"

#include <filesystem>
#include <map>
#include <vector>

#include <sol/sol.hpp>
#include <toml++/toml.hpp>

namespace plume {

namespace fs = std::filesystem;

struct plugin_host::impl {
	sol::state lua;
	sol::table io_lib;
	sol::table os_lib;
	bool net_granted = false;
	std::function<std::string(const std::string&, const std::string&)> model_fn;

	std::vector<sol::protected_function> pre_send, on_chunk, post_receive, on_key, setup;
	std::map<std::string, sol::protected_function> commands;
	std::map<std::string, sol::protected_function> keymaps;
	std::vector<std::pair<std::string, sol::protected_function>> segments;
};

plugin_host::plugin_host() : pimpl_(std::make_unique<impl>()) {}
plugin_host::~plugin_host() = default;
plugin_host::plugin_host(plugin_host&&) noexcept = default;
plugin_host& plugin_host::operator=(plugin_host&&) noexcept = default;

result<std::unique_ptr<plugin_host>> plugin_host::create() {
	auto host = std::unique_ptr<plugin_host>(new plugin_host());
	impl& h = *host->pimpl_;

	h.lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math,
	                     sol::lib::io, sol::lib::os);
	// keep io/os out of the default environment; hand them to a plugin only when
	// its manifest earns them.
	h.io_lib = h.lua["io"];
	h.os_lib = h.lua["os"];
	h.lua["io"] = sol::nil;
	h.lua["os"] = sol::nil;
	h.lua["package"] = sol::nil;
	h.lua["dofile"] = sol::nil;
	h.lua["loadfile"] = sol::nil;

	sol::table plume = h.lua.create_named_table("plume");
	impl* hp = &h;

	plume.set_function("on", [hp](const std::string& ev, sol::protected_function fn) {
		if (ev == "pre_send")
			hp->pre_send.push_back(fn);
		else if (ev == "on_chunk")
			hp->on_chunk.push_back(fn);
		else if (ev == "post_receive")
			hp->post_receive.push_back(fn);
		else if (ev == "on_key")
			hp->on_key.push_back(fn);
		else if (ev == "setup")
			hp->setup.push_back(fn);
	});
	plume.set_function("command", [hp](const std::string& name, sol::protected_function fn) {
		hp->commands[name] = fn;
	});
	plume.set_function("keymap", [hp](const std::string& key, sol::protected_function fn) {
		hp->keymaps[key] = fn;
	});
	plume.set_function("statusline", [hp](const std::string& id, sol::protected_function fn) {
		hp->segments.emplace_back(id, fn);
	});
	plume.set_function("notify", [](const std::string&) { /* surfaced by the ui host */ });

	sol::table model = plume.create_named("model");
	model.set_function("complete", [hp](sol::table opts) -> std::string {
		if (!hp->net_granted || !hp->model_fn) return "[plume.model unavailable: net not granted]";
		const std::string prompt = opts.get_or("prompt", std::string{});
		const std::string m = opts.get_or("model", std::string{});
		return hp->model_fn(prompt, m);
	});

	return host;
}

void plugin_host::set_model(std::function<std::string(const std::string&, const std::string&)> fn) {
	pimpl_->model_fn = std::move(fn);
}

result<void> plugin_host::load_all(const std::string& root, const approval_fn& approve) {
	impl& h = *pimpl_;
	if (!fs::exists(root)) return {};  // no plugins dir is not an error

	for (const auto& entry : fs::directory_iterator(root)) {
		if (!entry.is_directory()) continue;
		const fs::path manifest = entry.path() / "plugin.toml";
		if (!fs::exists(manifest)) continue;

		plugin_manifest man;
		try {
			toml::table t = toml::parse_file(manifest.string());
			man.name = t["name"].value_or(entry.path().filename().string());
			man.version = t["version"].value_or(std::string{"0"});
			man.entry = t["entry"].value_or(std::string{"init.lua"});
			if (auto caps = t["capabilities"].as_array())
				for (auto&& c : *caps)
					if (auto s = c.value<std::string>()) man.capabilities.push_back(*s);
		} catch (const toml::parse_error&) {
			continue;  // a broken manifest skips that plugin, not the whole host
		}

		sol::environment env(h.lua, sol::create, h.lua.globals());
		for (const auto& cap : man.capabilities) {
			if (!approve(man.name, cap)) continue;
			if (cap == "fs")
				env["io"] = h.io_lib;
			else if (cap == "exec")
				env["os"] = h.os_lib;
			else if (cap == "net")
				h.net_granted = true;
		}

		const fs::path script = entry.path() / man.entry;
		auto res = h.lua.safe_script_file(script.string(), env, sol::script_pass_on_error);
		if (!res.valid()) continue;  // a plugin that fails to load is skipped
	}

	for (auto& fn : h.setup) fn();
	return {};
}

std::string plugin_host::run_pre_send(std::string outgoing) {
	for (auto& fn : pimpl_->pre_send) {
		auto r = fn(outgoing);
		if (r.valid()) {
			if (auto s = r.get<std::optional<std::string>>()) outgoing = *s;
		}
	}
	return outgoing;
}

void plugin_host::run_on_chunk(std::string_view delta) {
	const std::string d(delta);
	for (auto& fn : pimpl_->on_chunk) fn(d);
}

void plugin_host::run_post_receive(std::string_view full) {
	const std::string f(full);
	for (auto& fn : pimpl_->post_receive) fn(f);
}

bool plugin_host::run_on_key(std::string_view key) {
	const std::string k(key);
	for (auto& fn : pimpl_->on_key) {
		auto r = fn(k);
		if (r.valid() && r.get_type() == sol::type::boolean && r.get<bool>()) return true;
	}
	return false;
}

std::optional<std::string> plugin_host::run_command(std::string_view name, std::string_view args) {
	auto it = pimpl_->commands.find(std::string(name));
	if (it == pimpl_->commands.end()) return std::nullopt;
	auto r = it->second(std::string(args));
	if (r.valid()) {
		if (auto s = r.get<std::optional<std::string>>()) return *s;
	}
	return std::string{};
}

std::vector<statusline_segment> plugin_host::statusline() const {
	std::vector<statusline_segment> out;
	for (auto& [id, fn] : pimpl_->segments) {
		auto r = fn();
		std::string text;
		if (r.valid())
			if (auto s = r.get<std::optional<std::string>>()) text = *s;
		out.push_back({id, std::move(text)});
	}
	return out;
}

std::vector<std::string> plugin_host::commands() const {
	std::vector<std::string> out;
	out.reserve(pimpl_->commands.size());
	for (auto& [name, _] : pimpl_->commands) out.push_back(name);
	return out;
}

}  // namespace plume
