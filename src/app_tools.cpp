// the mcp tool-use loop: gather tools, resolve each requested call against its
// server's approval policy, and continue the conversation with the results.
// split out of app_impl.hpp so it stays under the length limit.
#include <algorithm>

#include <nlohmann/json.hpp>

#include "app_impl.hpp"

namespace plume {

using namespace ftxui;

std::vector<tool_def> app::impl::tool_defs() const {
	std::vector<tool_def> out;
	out.reserve(mcp_tools.size());
	for (const auto& t : mcp_tools)
		out.push_back(tool_def{t.name, t.description, t.input_schema_json});
	return out;
}

bool app::impl::allowed(const pending_tool& pt) const {
	if (pt.server.empty()) return true;  // unknown tool: run_tool records the error
	if (pt.policy == "yolo") return true;
	if (pt.policy == "allowlist")
		for (const auto& sv : cfg.mcp)
			if (sv.name == pt.server)
				return std::find(sv.allow.begin(), sv.allow.end(), pt.name) != sv.allow.end();
	return false;  // "ask" (or anything unrecognized): prompt the user
}

void app::impl::advance_tools() {
	while (!tool_queue.empty()) {
		const pending_tool pt = tool_queue.front();
		if (!allowed(pt)) {
			ov = overlay::tool_approve;  // hand it to the approval UI
			return;
		}
		run_tool(pt);
		tool_queue.erase(tool_queue.begin());
	}
	if (!tool_results.empty()) submit_tool_results();
}

void app::impl::run_tool(const pending_tool& pt) {
	std::string content;
	bool err = false;
	if (!mcp || pt.server.empty()) {
		content = "no mcp server provides the tool '" + pt.name + "'";
		err = true;
	} else if (auto r = mcp->call(pt.server, pt.name, pt.args_json)) {
		content = *r;
	} else {
		content = r.error().detail;
		err = true;
	}
	tool_results.push_back(tool_result_block{pt.id, content, err});
	toast = (err ? "tool error: " : "tool ran: ") + pt.name;
}

void app::impl::submit_tool_results() {
	if (!db) return;
	node tr;
	tr.id = node_id{new_id("node")};
	tr.convo = convo;
	tr.parent = tool_parent;
	tr.role = role::user;  // anthropic expects tool_result blocks in a user turn
	std::vector<content_block> blocks;
	blocks.reserve(tool_results.size());
	for (const auto& r : tool_results) blocks.push_back(r);
	tr.content_json = codec::encode_blocks(blocks);
	tr.created_at = now_ms();
	static_cast<void>(db->put_node(tr));
	static_cast<void>(db->set_active_leaf(convo, tr.id));
	tool_results.clear();
	reload_transcript();
	stream_reply(tr.id);  // the model reads the results and keeps going
}

void app::impl::stream_reply(const node_id& parent) {
	if (streaming || !db || !prov) return;
	if (worker.joinable()) worker.join();

	request req;
	req.params = cfg.defaults;
	req.params.model = model_id();
	if (req.params.max_tokens < 1024) req.params.max_tokens = 4096;
	std::string sys = system_prompt;
	if (!compaction_summary.empty())
		sys += (sys.empty() ? "" : "\n\n") + std::string("summary of earlier turns: ") +
		       compaction_summary;
	if (!sys.empty()) req.system = sys;
	req.messages = context_upto(parent);
	req.tools = tool_defs();
	req.cache_prefix = true;

	live_text.clear();
	live_think.clear();
	status_error.clear();
	streaming = true;
	stop_flag = false;
	ttft_ms = 0;
	const std::int64_t start = now_ms();

	worker = std::thread([this, req, start, parent] {
		auto on_delta = [&](const stream_delta& d) {
			screen.Post([this, d, start] {
				if (ttft_ms == 0 &&
				    (d.type == stream_delta::kind::text || d.type == stream_delta::kind::thinking))
					ttft_ms = now_ms() - start;
				if (d.type == stream_delta::kind::text) {
					live_text += d.text;
					if (plugins) plugins->run_on_chunk(d.text);
				} else if (d.type == stream_delta::kind::thinking) {
					live_think += d.text;
				} else if (d.type == stream_delta::kind::usage) {
					last_usage = d.tokens;
				}
			});
			screen.PostEvent(Event::Custom);
		};
		auto stop = [this] { return stop_flag.load(); };
		auto out = prov->stream(req, on_delta, stop);
		screen.Post([this, out = std::move(out), parent] { finish_stream(out, parent); });
		screen.PostEvent(Event::Custom);
	});
}

Element app::impl::tool_args_table(const std::string& args_json) const {
	Elements rows;
	const auto j = nlohmann::json::parse(args_json, nullptr, false);  // no throw
	if (j.is_discarded()) {
		rows.push_back(paragraph(args_json) | color(col(th.p.text)));
	} else if (j.is_object() && !j.empty()) {
		for (const auto& [k, v] : j.items()) {
			const std::string val = v.is_string() ? v.get<std::string>() : v.dump();
			rows.push_back(hbox({text(k + "  ") | color(col(th.p.foam)) | size(WIDTH, EQUAL, 16),
			                     paragraph(val) | color(col(th.p.text))}));
		}
	} else {
		rows.push_back(paragraph(j.dump()) | color(col(th.p.text)));
	}
	if (rows.empty()) rows.push_back(text("(no arguments)") | color(col(th.p.muted)) | dim);
	return vbox(std::move(rows));
}

}  // namespace plume
