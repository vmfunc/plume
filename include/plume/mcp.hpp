// model context protocol client. servers are declared in config and speak
// either stdio or streamable-http; their tools are surfaced to any model that
// supports tool use. tool calls pass through an approval policy (always-ask,
// per-tool allowlist, or per-server yolo) before they run.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "plume/config.hpp"
#include "plume/error.hpp"

namespace plume {

struct mcp_tool {
	std::string server;
	std::string name;
	std::string description;
	std::string input_schema_json;
};

struct mcp_health {
	std::string server;
	bool connected = false;
	std::string detail;
};

class mcp_client {
   public:
	[[nodiscard]] static result<std::unique_ptr<mcp_client>> create();
	~mcp_client();
	mcp_client(mcp_client&&) noexcept;
	mcp_client& operator=(mcp_client&&) noexcept;

	// spin up / connect to a server. stdio spawns the command; http dials the url.
	[[nodiscard]] result<void> connect(const mcp_server_config&);

	[[nodiscard]] result<std::vector<mcp_tool>> tools() const;

	// invoke a tool. args_json is the tool input; returns the tool result text.
	[[nodiscard]] result<std::string> call(std::string_view server, std::string_view tool,
	                                       std::string_view args_json);

	// for plume doctor.
	[[nodiscard]] std::vector<mcp_health> health() const;

   private:
	mcp_client();
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

}  // namespace plume
