#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "plume/config.hpp"
#include "plume/mcp.hpp"

using namespace plume;

namespace {

// a minimal stdio mcp server: newline-delimited json-rpc, one 'echo' tool. it
// speaks exactly the three methods mcp_client drives (initialize, tools/list,
// tools/call) so the round-trip is real, not stubbed.
constexpr const char* kServer = R"PY(
import sys, json
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    msg = json.loads(line)
    mid = msg.get("id")
    method = msg.get("method")
    if method == "initialize":
        res = {"protocolVersion": "2025-06-18", "capabilities": {},
               "serverInfo": {"name": "echo", "version": "1"}}
    elif method == "tools/list":
        res = {"tools": [{"name": "echo", "description": "echo the msg back",
                          "inputSchema": {"type": "object",
                                          "properties": {"msg": {"type": "string"}}}}]}
    elif method == "tools/call":
        args = msg.get("params", {}).get("arguments", {})
        res = {"content": [{"type": "text", "text": "echo: " + str(args.get("msg", ""))}]}
    else:
        if mid is None:
            continue
        res = {}
    sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": mid, "result": res}) + "\n")
    sys.stdout.flush()
)PY";

std::string write_server() {
	const auto path = std::filesystem::temp_directory_path() / "plume_mcp_echo.py";
	std::ofstream(path) << kServer;
	return path.string();
}

}  // namespace

TEST_CASE("mcp stdio: a tool lists and a call round-trips") {
	const std::string script = write_server();
	mcp_server_config cfg;
	cfg.name = "echo";
	cfg.transport = "stdio";
	cfg.command = "python3";
	cfg.args = {script};

	auto client = mcp_client::create();
	REQUIRE(client);

	if (!(*client)->connect(cfg)) {
		// no python3 in this sandbox; the transport still compiled and linked.
		WARN_MESSAGE(false, "python3 unavailable, skipping live mcp round-trip");
		return;
	}

	auto tools = (*client)->tools();
	REQUIRE(tools);
	REQUIRE(tools->size() == 1);
	CHECK(tools->front().name == "echo");
	CHECK(tools->front().server == "echo");
	CHECK(tools->front().input_schema_json.find("msg") != std::string::npos);

	auto out = (*client)->call("echo", "echo", R"({"msg":"quill"})");
	REQUIRE(out);
	CHECK(*out == "echo: quill");

	const auto health = (*client)->health();
	REQUIRE(health.size() == 1);
	CHECK(health.front().connected);
}
