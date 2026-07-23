// the anthropic backend. wire shapes verified against the messages, streaming,
// models and token-counting docs. model ids are never hardcoded — list_models
// reads /v1/models at runtime. the one sharp constraint the ui leans on:
// assistant prefill (the mechanism behind mid-message weaving) is rejected on
// the thinking-capable 4.6+ line and is incompatible with extended thinking, so
// we feature-detect and fail loudly rather than send a request the api rejects.
#include <nlohmann/json.hpp>

#include "../http.hpp"
#include "anthropic_stream.hpp"
#include "plume/provider.hpp"
#include "plume/sse.hpp"

namespace plume {

namespace {

using json = nlohmann::json;

constexpr const char* kDefaultBase = "https://api.anthropic.com";
constexpr const char* kVersion = "2023-06-01";

bool contains(const std::string& id, const char* needle) {
	return id.find(needle) != std::string::npos;
}

// the 4.6+ line and Fable/Mythos reject assistant prefill outright.
bool prefill_blocked(const std::string& id) {
	return contains(id, "opus-4-6") || contains(id, "opus-4-7") || contains(id, "opus-4-8") ||
	       contains(id, "sonnet-4-6") || contains(id, "sonnet-5") || contains(id, "fable") ||
	       contains(id, "mythos");
}

// the dynamic-filtering web search tool ships on the same line that blocks
// prefill (4.6+, sonnet-5, fable/mythos); older models take only the basic tool.
const char* web_search_type(const std::string& id) {
	return prefill_blocked(id) ? "web_search_20260209" : "web_search_20250305";
}

features infer_features(const std::string& id) {
	features f;
	f.prefill = !prefill_blocked(id);
	f.thinking = prefill_blocked(id) || contains(id, "opus-4-5") || contains(id, "sonnet-4-5") ||
	             contains(id, "haiku-4-5");
	f.vision = true;
	f.pdf = true;
	f.tools = true;
	f.prompt_cache = true;
	return f;
}

json source_of(const std::string& media_type, const std::string& data) {
	return {{"type", "base64"}, {"media_type", media_type}, {"data", data}};
}

json block_to_wire(const content_block& b) {
	return std::visit(
	    [](const auto& v) -> json {
		    using T = std::decay_t<decltype(v)>;
		    if constexpr (std::is_same_v<T, text_block>) {
			    return {{"type", "text"}, {"text", v.text}};
		    } else if constexpr (std::is_same_v<T, thinking_block>) {
			    return {{"type", "thinking"}, {"thinking", v.thinking}, {"signature", v.signature}};
		    } else if constexpr (std::is_same_v<T, image_block>) {
			    return {{"type", "image"}, {"source", source_of(v.media_type, v.data)}};
		    } else if constexpr (std::is_same_v<T, document_block>) {
			    return {{"type", "document"}, {"source", source_of(v.media_type, v.data)}};
		    } else if constexpr (std::is_same_v<T, tool_use_block>) {
			    json input = json::parse(v.input_json, nullptr, false);
			    if (input.is_discarded()) input = json::object();
			    return {{"type", "tool_use"}, {"id", v.id}, {"name", v.name}, {"input", input}};
		    } else {  // tool_result_block
			    return {{"type", "tool_result"},
			            {"tool_use_id", v.tool_use_id},
			            {"content", v.content},
			            {"is_error", v.is_error}};
		    }
	    },
	    b);
}

class anthropic_provider final : public provider {
   public:
	anthropic_provider(std::string base, std::string key)
	    : base_(base.empty() ? kDefaultBase : std::move(base)), key_(std::move(key)) {}

	std::string_view name() const override { return "anthropic"; }

	result<std::vector<model_info>> list_models() override {
		std::vector<model_info> out;
		std::string url = base_ + "/v1/models?limit=100";
		for (;;) {
			auto resp = http::get(url, headers());
			if (!resp) return std::unexpected(resp.error());
			if (resp->status != 200)
				return fail(errc::http_status,
				            "models: http " + std::to_string(resp->status) + ": " + resp->body);
			json j = json::parse(resp->body, nullptr, false);
			if (j.is_discarded()) return fail(errc::parse, "models: bad json");
			for (const auto& m : j.value("data", json::array())) {
				model_info info;
				info.id = m.value("id", "");
				info.display_name = m.value("display_name", info.id);
				info.context = m.value("max_input_tokens", 0LL);
				info.max_output = m.value("max_tokens", 0LL);
				info.caps = infer_features(info.id);
				out.push_back(std::move(info));
			}
			if (!j.value("has_more", false)) break;
			const std::string last = j.value("last_id", "");
			if (last.empty()) break;
			url = base_ + "/v1/models?limit=100&after_id=" + last;
		}
		return out;
	}

	result<std::int64_t> count_tokens(const request& req) override {
		auto body = build_body(req, false);
		if (!body) return std::unexpected(body.error());
		auto resp = http::post(base_ + "/v1/messages/count_tokens", headers(), body->dump());
		if (!resp) return std::unexpected(resp.error());
		if (resp->status != 200)
			return fail(errc::http_status,
			            "count_tokens: http " + std::to_string(resp->status) + ": " + resp->body);
		json j = json::parse(resp->body, nullptr, false);
		if (j.is_discarded()) return fail(errc::parse, "count_tokens: bad json");
		return j.value("input_tokens", 0LL);
	}

	result<completion> stream(const request& req,
	                          const std::function<void(const stream_delta&)>& on_delta,
	                          const std::function<bool()>& stop) override {
		auto body = build_body(req, true);
		if (!body) return std::unexpected(body.error());

		anthropic_detail::assembler asm_(on_delta);
		asm_.set_fallback_model(req.params.model);
		sse_parser parser;

		auto sink = [&](std::string_view bytes) -> bool {
			parser.feed(bytes, [&](const sse_event& ev) { asm_.event(ev); });
			return !stop();
		};

		auto r = http::post_stream(base_ + "/v1/messages", headers(), body->dump(), sink);
		if (!r) return std::unexpected(r.error());
		parser.finish([&](const sse_event& ev) { asm_.event(ev); });
		asm_.finish();

		if (r->status != 200) {
			std::string msg = r->error_body;
			if (json e = json::parse(r->error_body, nullptr, false);
			    !e.is_discarded() && e.contains("error"))
				msg = e["error"].value("message", msg);
			return fail(errc::http_status, "http " + std::to_string(r->status) + ": " + msg);
		}
		if (!asm_.error().empty()) return fail(errc::http_status, asm_.error());
		return asm_.take();
	}

   private:
	http::headers headers() const {
		http::headers h;
		h.add("x-api-key: " + key_);
		h.add(std::string("anthropic-version: ") + kVersion);
		h.add("content-type: application/json");
		return h;
	}

	result<json> build_body(const request& req, bool stream) const {
		const auto& p = req.params;
		json body;
		body["model"] = p.model;
		body["max_tokens"] = p.max_tokens;
		if (stream) body["stream"] = true;

		// system prompt, with an optional cache breakpoint after it.
		if (req.system && !req.system->empty()) {
			if (req.cache_prefix) {
				body["system"] = json::array({{{"type", "text"},
				                               {"text", *req.system},
				                               {"cache_control", {{"type", "ephemeral"}}}}});
			} else {
				body["system"] = *req.system;
			}
		}

		json messages = json::array();
		std::string hoisted_system;
		for (const auto& m : req.messages) {
			if (m.role == role::system) {
				hoisted_system += m.plain_text() + "\n";
				continue;
			}
			json content = json::array();
			for (const auto& b : m.blocks) content.push_back(block_to_wire(b));
			messages.push_back({{"role", to_string(m.role)}, {"content", content}});
		}
		if (!hoisted_system.empty() && !body.contains("system")) body["system"] = hoisted_system;

		// assistant prefill: continue an assistant turn from exact text. this is
		// the seam mid-message weaving rides on; guard it hard.
		if (req.assistant_prefill && !req.assistant_prefill->empty()) {
			if (prefill_blocked(p.model))
				return fail(errc::unsupported,
				            "model " + p.model + " does not support assistant prefill");
			if (p.thinking != thinking_mode::off)
				return fail(errc::unsupported,
				            "assistant prefill is incompatible with extended thinking");
			messages.push_back(
			    {{"role", "assistant"},
			     {"content", json::array({{{"type", "text"}, {"text", *req.assistant_prefill}}})}});
		}
		body["messages"] = std::move(messages);

		json tools = json::array();
		for (const auto& t : req.tools) {
			json schema = json::parse(t.input_schema_json, nullptr, false);
			if (schema.is_discarded()) schema = {{"type", "object"}};
			tools.push_back(
			    {{"name", t.name}, {"description", t.description}, {"input_schema", schema}});
		}
		// the cache breakpoint sits on the last custom tool; the server-side web
		// search tool is appended after it, uncached, so claude runs the search.
		if (req.cache_prefix && !tools.empty())
			tools.back()["cache_control"] = {{"type", "ephemeral"}};
		if (req.web_search)
			tools.push_back({{"type", web_search_type(p.model)}, {"name", "web_search"}});
		if (!tools.empty()) body["tools"] = std::move(tools);

		switch (p.thinking) {
			case thinking_mode::adaptive:
				body["thinking"] = {{"type", "adaptive"}, {"display", "summarized"}};
				break;
			case thinking_mode::budget:
				body["thinking"] = {{"type", "enabled"}, {"budget_tokens", p.thinking_budget}};
				break;
			case thinking_mode::off: break;
		}
		if (!p.effort.empty()) body["output_config"] = {{"effort", p.effort}};

		// sampling params only make sense with thinking off; the 4.7+ line
		// rejects them, so the ui simply doesn't set them there.
		if (p.thinking == thinking_mode::off) {
			if (p.temperature) body["temperature"] = *p.temperature;
			if (p.top_p) body["top_p"] = *p.top_p;
		}
		return body;
	}

	std::string base_;
	std::string key_;
};

}  // namespace

// exposed to the factory in the same translation-unit group.
std::unique_ptr<provider> make_anthropic(const std::string& base, const std::string& key) {
	return std::make_unique<anthropic_provider>(base, key);
}

}  // namespace plume
