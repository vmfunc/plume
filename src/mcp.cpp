// model context protocol client. stdio spawns the server and speaks
// newline-delimited json-rpc 2.0 over its pipes; the streamable-http transport
// posts json-rpc to the server url. tools are discovered on connect and cached;
// the ui gates each call through its approval policy before call() runs.
#include "plume/mcp.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <map>
#include <vector>

#include <nlohmann/json.hpp>

#include "http.hpp"

namespace plume {

namespace {
using json = nlohmann::json;

struct child {
	pid_t pid = -1;
	int in_fd = -1;   // parent writes -> child stdin
	int out_fd = -1;  // child stdout -> parent reads
	std::string buf;
};

result<child> spawn(const std::string& cmd, const std::vector<std::string>& args) {
	int inpipe[2], outpipe[2];
	if (pipe(inpipe) != 0 || pipe(outpipe) != 0) return fail(errc::io, "pipe failed");
	const pid_t pid = fork();
	if (pid < 0) return fail(errc::io, "fork failed");
	if (pid == 0) {
		dup2(inpipe[0], STDIN_FILENO);
		dup2(outpipe[1], STDOUT_FILENO);
		close(inpipe[0]);
		close(inpipe[1]);
		close(outpipe[0]);
		close(outpipe[1]);
		std::vector<char*> argv;
		argv.push_back(const_cast<char*>(cmd.c_str()));
		for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
		argv.push_back(nullptr);
		execvp(cmd.c_str(), argv.data());
		_exit(127);
	}
	close(inpipe[0]);
	close(outpipe[1]);
	return child{pid, inpipe[1], outpipe[0], {}};
}

bool write_line(int fd, const std::string& line) {
	const std::string framed = line + "\n";
	return ::write(fd, framed.data(), framed.size()) == static_cast<ssize_t>(framed.size());
}

// read one newline-delimited message with a timeout.
std::optional<std::string> read_line(child& c, int timeout_ms) {
	for (;;) {
		if (const auto nl = c.buf.find('\n'); nl != std::string::npos) {
			std::string line = c.buf.substr(0, nl);
			c.buf.erase(0, nl + 1);
			return line;
		}
		pollfd pfd{c.out_fd, POLLIN, 0};
		if (poll(&pfd, 1, timeout_ms) <= 0) return std::nullopt;
		std::array<char, 4096> tmp{};
		const ssize_t n = ::read(c.out_fd, tmp.data(), tmp.size());
		if (n <= 0) return std::nullopt;
		c.buf.append(tmp.data(), static_cast<std::size_t>(n));
	}
}

}  // namespace

struct mcp_client::impl {
	struct server {
		mcp_server_config cfg;
		std::optional<child> proc;  // stdio only
		bool connected = false;
		std::string detail;
		int next_id = 1;
	};
	std::map<std::string, server> servers;

	// one json-rpc round trip for a stdio server.
	result<json> rpc_stdio(server& s, const std::string& method, const json& params) {
		if (!s.proc) return fail(errc::network, "not connected");
		const int id = s.next_id++;
		json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
		if (!params.is_null()) req["params"] = params;
		if (!write_line(s.proc->in_fd, req.dump())) return fail(errc::io, "write failed");

		for (int i = 0; i < 200; ++i) {  // skip notifications/logs until our id lands
			auto line = read_line(*s.proc, 5000);
			if (!line) return fail(errc::network, "no reply from " + s.cfg.name);
			json msg = json::parse(*line, nullptr, false);
			if (msg.is_discarded()) continue;
			if (msg.value("id", -1) == id) {
				if (msg.contains("error"))
					return fail(errc::http_status, msg["error"].value("message", "rpc error"));
				return msg.value("result", json::object());
			}
		}
		return fail(errc::network, "rpc id never resolved");
	}

	result<json> rpc_http(server& s, const std::string& method, const json& params) {
		const int id = s.next_id++;
		json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
		if (!params.is_null()) req["params"] = params;
		http::headers h;
		h.add("content-type: application/json");
		h.add("accept: application/json");
		auto resp = http::post(s.cfg.url, h, req.dump());
		if (!resp) return std::unexpected(resp.error());
		json msg = json::parse(resp->body, nullptr, false);
		if (msg.is_discarded()) return fail(errc::parse, "bad json-rpc reply");
		if (msg.contains("error"))
			return fail(errc::http_status, msg["error"].value("message", "rpc error"));
		return msg.value("result", json::object());
	}

	result<json> rpc(server& s, const std::string& method, const json& params) {
		return s.cfg.transport == "http" ? rpc_http(s, method, params) : rpc_stdio(s, method, params);
	}
};

mcp_client::mcp_client() : pimpl_(std::make_unique<impl>()) {}
mcp_client::~mcp_client() {
	for (auto& [name, s] : pimpl_->servers)
		if (s.proc && s.proc->pid > 0) {
			kill(s.proc->pid, SIGTERM);
			waitpid(s.proc->pid, nullptr, 0);
		}
}
mcp_client::mcp_client(mcp_client&&) noexcept = default;
mcp_client& mcp_client::operator=(mcp_client&&) noexcept = default;

result<std::unique_ptr<mcp_client>> mcp_client::create() {
	return std::unique_ptr<mcp_client>(new mcp_client());
}

result<void> mcp_client::connect(const mcp_server_config& cfg) {
	impl::server s;
	s.cfg = cfg;

	if (cfg.transport == "stdio") {
		auto proc = spawn(cfg.command, cfg.args);
		if (!proc) return std::unexpected(proc.error());
		s.proc = std::move(*proc);
	}

	pimpl_->servers[cfg.name] = std::move(s);
	impl::server& srv = pimpl_->servers[cfg.name];

	const json init_params = {{"protocolVersion", "2025-06-18"},
	                          {"capabilities", json::object()},
	                          {"clientInfo", {{"name", "plume"}, {"version", "0.1.0"}}}};
	auto init = pimpl_->rpc(srv, "initialize", init_params);
	if (!init) {
		srv.detail = init.error().detail;
		return std::unexpected(init.error());
	}
	if (srv.proc) write_line(srv.proc->in_fd,
	                         json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump());
	srv.connected = true;
	return {};
}

result<std::vector<mcp_tool>> mcp_client::tools() const {
	std::vector<mcp_tool> out;
	for (auto& [name, s] : pimpl_->servers) {
		if (!s.connected) continue;
		auto res = pimpl_->rpc(const_cast<impl::server&>(s), "tools/list", json::object());
		if (!res) continue;
		for (const auto& t : res->value("tools", json::array())) {
			mcp_tool tool;
			tool.server = name;
			tool.name = t.value("name", "");
			tool.description = t.value("description", "");
			if (t.contains("inputSchema")) tool.input_schema_json = t["inputSchema"].dump();
			out.push_back(std::move(tool));
		}
	}
	return out;
}

result<std::string> mcp_client::call(std::string_view server, std::string_view tool,
                                     std::string_view args_json) {
	auto it = pimpl_->servers.find(std::string(server));
	if (it == pimpl_->servers.end()) return fail(errc::not_found, "no such mcp server");
	json args = json::parse(args_json, nullptr, false);
	if (args.is_discarded()) args = json::object();
	auto res = pimpl_->rpc(it->second, "tools/call",
	                       {{"name", std::string(tool)}, {"arguments", args}});
	if (!res) return std::unexpected(res.error());

	std::string text;
	for (const auto& block : res->value("content", json::array()))
		if (block.value("type", "") == "text") text += block.value("text", "");
	return text;
}

std::vector<mcp_health> mcp_client::health() const {
	std::vector<mcp_health> out;
	for (auto& [name, s] : pimpl_->servers)
		out.push_back({name, s.connected, s.detail});
	return out;
}

}  // namespace plume
