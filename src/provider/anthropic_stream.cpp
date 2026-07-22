#include "anthropic_stream.hpp"

#include <nlohmann/json.hpp>

namespace plume::anthropic_detail {

using json = nlohmann::json;

void assembler::finish_block() {
	if (cur_type_ == "text") {
		out_.reply.blocks.emplace_back(text_block{text_});
	} else if (cur_type_ == "thinking") {
		out_.reply.blocks.emplace_back(thinking_block{thinking_, sig_});
	} else if (cur_type_ == "tool_use") {
		out_.reply.blocks.emplace_back(tool_use_block{tool_id_, tool_name_, tool_json_});
	}
	cur_type_.clear();
	text_.clear();
	thinking_.clear();
	sig_.clear();
	tool_id_.clear();
	tool_name_.clear();
	tool_json_.clear();
	out_.reply.role = role::assistant;
}

void assembler::event(const sse_event& ev) {
	json j = json::parse(ev.data, nullptr, false);
	if (j.is_discarded()) return;
	const std::string type = j.value("type", ev.type);

	if (type == "message_start") {
		const auto& msg = j["message"];
		out_.model = msg.value("model", out_.model);
		if (msg.contains("usage")) {
			const auto& u = msg["usage"];
			out_.tokens.input = u.value("input_tokens", 0LL);
			out_.tokens.cache_creation = u.value("cache_creation_input_tokens", 0LL);
			out_.tokens.cache_read = u.value("cache_read_input_tokens", 0LL);
		}
	} else if (type == "content_block_start") {
		finish_block();
		cur_type_ = j["content_block"].value("type", "");
		if (cur_type_ == "tool_use") {
			tool_id_ = j["content_block"].value("id", "");
			tool_name_ = j["content_block"].value("name", "");
			on_delta_({stream_delta::kind::tool_use, tool_name_, tool_id_, {}});
		}
	} else if (type == "content_block_delta") {
		const auto& d = j["delta"];
		const std::string dt = d.value("type", "");
		if (dt == "text_delta") {
			const std::string t = d.value("text", "");
			text_ += t;
			on_delta_({stream_delta::kind::text, t, {}, {}});
		} else if (dt == "thinking_delta") {
			const std::string t = d.value("thinking", "");
			thinking_ += t;
			on_delta_({stream_delta::kind::thinking, t, {}, {}});
		} else if (dt == "signature_delta") {
			sig_ += d.value("signature", "");
		} else if (dt == "input_json_delta") {
			const std::string t = d.value("partial_json", "");
			tool_json_ += t;
			on_delta_({stream_delta::kind::tool_json, t, tool_id_, {}});
		}
	} else if (type == "content_block_stop") {
		finish_block();
	} else if (type == "message_delta") {
		if (j.contains("delta"))
			out_.stop_reason = j["delta"].value("stop_reason", out_.stop_reason);
		if (j.contains("usage"))
			out_.tokens.output = j["usage"].value("output_tokens", out_.tokens.output);
		on_delta_({stream_delta::kind::usage, {}, {}, out_.tokens});
	} else if (type == "message_stop") {
		on_delta_({stream_delta::kind::done, out_.stop_reason, {}, {}});
	} else if (type == "ping") {
		on_delta_({stream_delta::kind::ping, {}, {}, {}});
	} else if (type == "error") {
		sse_error_ = j.value("error", json::object()).value("message", "stream error");
	}
}

}  // namespace plume::anthropic_detail
