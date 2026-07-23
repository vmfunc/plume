#include "plume/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

namespace plume {

namespace {

namespace fs = std::filesystem;

std::string env_or(const char* var, const std::string& fallback) {
	if (const char* v = std::getenv(var); v && *v) return v;
	return fallback;
}

std::string home() {
	return env_or("HOME", ".");
}

thinking_mode thinking_from(std::string_view s) {
	if (s == "adaptive") return thinking_mode::adaptive;
	if (s == "budget") return thinking_mode::budget;
	return thinking_mode::off;
}

const char* thinking_to(thinking_mode m) {
	switch (m) {
		case thinking_mode::adaptive: return "adaptive";
		case thinking_mode::budget: return "budget";
		case thinking_mode::off: return "off";
	}
	return "off";
}

std::string quote(std::string_view s) {
	std::string out = "\"";
	for (char c : s) {
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\t': out += "\\t"; break;
			case '\r': out += "\\r"; break;
			default: out.push_back(c);
		}
	}
	out.push_back('"');
	return out;
}

}  // namespace

std::string default_config_path() {
	return env_or("XDG_CONFIG_HOME", home() + "/.config") + "/plume/config.toml";
}
std::string default_data_path() {
	return env_or("XDG_DATA_HOME", home() + "/.local/share") + "/plume";
}
std::string default_state_path() {
	return env_or("XDG_STATE_HOME", home() + "/.local/state") + "/plume";
}

const provider_entry* config::active_provider() const {
	if (auto it = providers.find(default_provider); it != providers.end()) return &it->second;
	if (!providers.empty()) return &providers.begin()->second;
	return nullptr;
}

config default_config() {
	config c;
	c.config_dir = fs::path(default_config_path()).parent_path().string();
	c.data_dir = default_data_path();
	c.state_dir = default_state_path();
	c.defaults.max_tokens = 4096;
	c.defaults.thinking = thinking_mode::off;
	// a starting price snapshot. plume never presents these as authoritative;
	// docs say to verify against the provider's current pricing.
	c.prices["claude-opus-4-8"] = {5.0, 25.0, 6.25, 0.5};
	c.prices["claude-fable-5"] = {10.0, 50.0, 12.5, 1.0};
	c.prices["claude-sonnet-5"] = {3.0, 15.0, 3.75, 0.3};
	c.prices["claude-haiku-4-5"] = {1.0, 5.0, 1.25, 0.1};
	return c;
}

result<config> load_config(const std::string& path) {
	config c = default_config();
	if (!fs::exists(path)) return c;  // absence is not an error; ship defaults

	toml::table tbl;
	try {
		tbl = toml::parse_file(path);
	} catch (const toml::parse_error& e) {
		return fail(errc::config, std::string("config: ") + e.description().data());
	}

	if (auto ui = tbl["ui"].as_table()) {
		c.ui.theme = (*ui)["theme"].value_or(c.ui.theme);
		c.ui.reduce_motion = (*ui)["reduce_motion"].value_or(c.ui.reduce_motion);
		c.ui.density = (*ui)["density"].value_or(c.ui.density);
		c.ui.zen = (*ui)["zen"].value_or(c.ui.zen);
		c.ui.sidebar = (*ui)["sidebar"].value_or(c.ui.sidebar);
	}

	if (auto keys = tbl["keys"].as_table()) {
		c.keys.preset = (*keys)["preset"].value_or(c.keys.preset);
		if (auto binds = (*keys)["binds"].as_table()) {
			for (auto&& [k, v] : *binds)
				if (auto s = v.value<std::string>()) c.keys.binds[std::string(k.str())] = *s;
		}
	}

	c.default_provider = tbl["default_provider"].value_or(std::string{});
	c.notify = tbl["notify"].value_or(c.notify);

	if (auto provs = tbl["providers"].as_table()) {
		for (auto&& [name, node] : *provs) {
			auto t = node.as_table();
			if (!t) continue;
			provider_entry e;
			e.kind = (*t)["kind"].value_or(std::string(name.str()));
			e.base_url = (*t)["base_url"].value_or(std::string{});
			e.auth_source = (*t)["auth_source"].value_or(std::string("env"));
			e.auth_value = (*t)["auth_value"].value_or(std::string{});
			e.default_model = (*t)["default_model"].value_or(std::string{});
			c.providers[std::string(name.str())] = std::move(e);
		}
		if (c.default_provider.empty() && !c.providers.empty())
			c.default_provider = c.providers.begin()->first;
	}

	if (auto d = tbl["defaults"].as_table()) {
		c.defaults.model = (*d)["model"].value_or(std::string{});
		c.defaults.max_tokens = (*d)["max_tokens"].value_or(c.defaults.max_tokens);
		if (auto temp = (*d)["temperature"].value<double>()) c.defaults.temperature = *temp;
		if (auto tp = (*d)["top_p"].value<double>()) c.defaults.top_p = *tp;
		c.defaults.thinking = thinking_from((*d)["thinking"].value_or(std::string("off")));
		c.defaults.thinking_budget = (*d)["thinking_budget"].value_or(0);
		c.defaults.effort = (*d)["effort"].value_or(std::string{});
	}

	if (auto prices = tbl["prices"].as_table()) {
		for (auto&& [model, node] : *prices) {
			auto t = node.as_table();
			if (!t) continue;
			price p;
			p.input = (*t)["input"].value_or(0.0);
			p.output = (*t)["output"].value_or(0.0);
			p.cache_write = (*t)["cache_write"].value_or(0.0);
			p.cache_read = (*t)["cache_read"].value_or(0.0);
			c.prices[std::string(model.str())] = p;
		}
	}

	if (auto roles = tbl["roles"].as_table())
		for (auto&& [name, node] : *roles)
			if (auto s = node.value<std::string>()) c.roles[std::string(name.str())] = *s;

	if (auto snips = tbl["snippets"].as_array())
		for (auto&& node : *snips)
			if (auto t = node.as_table())
				c.snippets.push_back(
				    {(*t)["name"].value_or(std::string{}), (*t)["body"].value_or(std::string{})});

	if (auto plugins = tbl["plugins"].as_array()) {
		c.plugins.clear();
		for (auto&& v : *plugins)
			if (auto s = v.value<std::string>()) c.plugins.push_back(*s);
	}

	if (auto mcp = tbl["mcp"].as_array()) {
		for (auto&& node : *mcp) {
			auto t = node.as_table();
			if (!t) continue;
			mcp_server_config m;
			m.name = (*t)["name"].value_or(std::string{});
			m.transport = (*t)["transport"].value_or(std::string("stdio"));
			m.command = (*t)["command"].value_or(std::string{});
			m.url = (*t)["url"].value_or(std::string{});
			m.approval = (*t)["approval"].value_or(std::string("ask"));
			if (auto args = (*t)["args"].as_array())
				for (auto&& a : *args)
					if (auto s = a.value<std::string>()) m.args.push_back(*s);
			if (auto allow = (*t)["allow"].as_array())
				for (auto&& a : *allow)
					if (auto s = a.value<std::string>()) m.allow.push_back(*s);
			c.mcp.push_back(std::move(m));
		}
	}

	return c;
}

result<void> save_config(const config& c, const std::string& path) {
	std::ostringstream o;
	o << "# plume config. every option is documented in docs/config.md.\n";
	o << "# hot-reloaded on save.\n\n";

	o << "[ui]\n";
	o << "theme = " << quote(c.ui.theme) << "\n";
	o << "density = " << quote(c.ui.density) << "\n";
	o << "reduce_motion = " << (c.ui.reduce_motion ? "true" : "false") << "\n";
	o << "zen = " << (c.ui.zen ? "true" : "false") << "\n";
	o << "sidebar = " << (c.ui.sidebar ? "true" : "false") << "\n\n";

	o << "[keys]\n";
	o << "preset = " << quote(c.keys.preset) << "\n\n";

	if (!c.default_provider.empty())
		o << "default_provider = " << quote(c.default_provider) << "\n";
	o << "notify = " << quote(c.notify) << "\n\n";

	for (const auto& [name, p] : c.providers) {
		o << "[providers." << name << "]\n";
		o << "kind = " << quote(p.kind) << "\n";
		if (!p.base_url.empty()) o << "base_url = " << quote(p.base_url) << "\n";
		o << "auth_source = " << quote(p.auth_source) << "\n";
		if (p.auth_source == "inline")
			o << "# an inline key is stored here in plaintext. prefer env / key_cmd / keychain.\n";
		if (!p.auth_value.empty()) o << "auth_value = " << quote(p.auth_value) << "\n";
		if (!p.default_model.empty()) o << "default_model = " << quote(p.default_model) << "\n";
		o << "\n";
	}

	o << "[defaults]\n";
	if (!c.defaults.model.empty()) o << "model = " << quote(c.defaults.model) << "\n";
	o << "max_tokens = " << c.defaults.max_tokens << "\n";
	if (c.defaults.temperature) o << "temperature = " << *c.defaults.temperature << "\n";
	if (c.defaults.top_p) o << "top_p = " << *c.defaults.top_p << "\n";
	o << "thinking = " << quote(thinking_to(c.defaults.thinking)) << "\n";
	if (c.defaults.thinking_budget > 0)
		o << "thinking_budget = " << c.defaults.thinking_budget << "\n";
	if (!c.defaults.effort.empty()) o << "effort = " << quote(c.defaults.effort) << "\n";
	o << "\n";

	o << "# a price snapshot. verify against your provider's current pricing.\n";
	for (const auto& [model, p] : c.prices) {
		o << "[prices." << quote(model) << "]\n";
		o << "input = " << p.input << "\noutput = " << p.output << "\n";
		o << "cache_write = " << p.cache_write << "\ncache_read = " << p.cache_read << "\n\n";
	}

	if (!c.plugins.empty()) {
		o << "plugins = [";
		for (std::size_t i = 0; i < c.plugins.size(); ++i)
			o << (i ? ", " : "") << quote(c.plugins[i]);
		o << "]\n";
	}

	if (!c.roles.empty()) {
		o << "\n[roles]\n";
		for (const auto& [name, prompt] : c.roles)
			o << quote(name) << " = " << quote(prompt) << "\n";
	}
	for (const auto& s : c.snippets) {
		o << "\n[[snippets]]\n";
		o << "name = " << quote(s.name) << "\n";
		o << "body = " << quote(s.body) << "\n";
	}

	const fs::path p(path);
	if (!p.parent_path().empty()) {
		std::error_code ec;
		fs::create_directories(p.parent_path(), ec);
	}
	std::ofstream f(path, std::ios::trunc);
	if (!f) return fail(errc::io, "cannot write config: " + path);
	f << o.str();
	if (!f) return fail(errc::io, "write failed: " + path);
	return {};
}

}  // namespace plume
