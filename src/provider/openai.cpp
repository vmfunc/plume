// the openai-compatible backend. one implementation covers openai, openrouter,
// ollama and any base-url that speaks /chat/completions. it handles the common
// path — streamed text and vision — well; the anthropic-only surface (extended
// thinking, prompt-cache breakpoints, prefill weaving) stays on the anthropic
// backend, and this one reports those as unsupported rather than faking them.
#include <nlohmann/json.hpp>

#include "../http.hpp"
#include "plume/provider.hpp"
#include "plume/sse.hpp"

namespace plume {

namespace {

using json = nlohmann::json;

json message_to_wire(const message& m) {
	// text-only messages ride as a plain string; anything richer becomes the
	// array form with image_url parts.
	bool only_text = true;
	for (const auto& b : m.blocks)
		if (!std::holds_alternative<text_block>(b)) only_text = false;

	if (only_text) return {{"role", to_string(m.role)}, {"content", m.plain_text()}};

	json content = json::array();
	for (const auto& b : m.blocks) {
		if (const auto* t = std::get_if<text_block>(&b)) {
			content.push_back({{"type", "text"}, {"text", t->text}});
		} else if (const auto* im = std::get_if<image_block>(&b)) {
			content.push_back({{"type", "image_url"},
			                   {"image_url", {{"url", "data:" + im->media_type + ";base64," + im->data}}}});
		}
		// documents and tool blocks have no portable openai shape here.
	}
	return {{"role", to_string(m.role)}, {"content", content}};
}

class openai_provider final : public provider {
   public:
	openai_provider(std::string kind, std::string base, std::string key)
	    : kind_(std::move(kind)), base_(std::move(base)), key_(std::move(key)) {}

	std::string_view name() const override { return kind_; }

	result<std::vector<model_info>> list_models() override {
		auto resp = http::get(base_ + "/models", headers());
		if (!resp) return std::unexpected(resp.error());
		if (resp->status != 200)
			return fail(errc::http_status, "models: http " + std::to_string(resp->status));
		json j = json::parse(resp->body, nullptr, false);
		if (j.is_discarded()) return fail(errc::parse, "models: bad json");
		std::vector<model_info> out;
		for (const auto& m : j.value("data", json::array())) {
			model_info info;
			info.id = m.value("id", "");
			info.display_name = info.id;
			info.caps.vision = true;
			info.caps.tools = true;
			out.push_back(std::move(info));
		}
		return out;
	}

	result<std::int64_t> count_tokens(const request&) override {
		return fail(errc::unsupported, kind_ + " has no token-counting endpoint");
	}

	result<completion> stream(const request& req,
	                          const std::function<void(const stream_delta&)>& on_delta,
	                          const std::function<bool()>& stop) override {
		if (req.assistant_prefill && !req.assistant_prefill->empty())
			return fail(errc::unsupported, "prefill weaving is anthropic-only");

		json body;
		body["model"] = req.params.model;
		body["max_tokens"] = req.params.max_tokens;
		body["stream"] = true;
		body["stream_options"] = {{"include_usage", true}};
		if (req.params.temperature) body["temperature"] = *req.params.temperature;

		json messages = json::array();
		if (req.system && !req.system->empty())
			messages.push_back({{"role", "system"}, {"content", *req.system}});
		for (const auto& m : req.messages) messages.push_back(message_to_wire(m));
		body["messages"] = std::move(messages);

		completion out;
		out.model = req.params.model;
		std::string text_buf;
		sse_parser parser;

		auto on_event = [&](const sse_event& ev) {
			if (ev.data == "[DONE]") return;
			json j = json::parse(ev.data, nullptr, false);
			if (j.is_discarded()) return;
			if (j.contains("usage") && !j["usage"].is_null()) {
				out.tokens.input = j["usage"].value("prompt_tokens", out.tokens.input);
				out.tokens.output = j["usage"].value("completion_tokens", out.tokens.output);
				on_delta({stream_delta::kind::usage, {}, {}, out.tokens});
			}
			for (const auto& ch : j.value("choices", json::array())) {
				const std::string piece = ch.value("delta", json::object()).value("content", "");
				if (!piece.empty()) {
					text_buf += piece;
					on_delta({stream_delta::kind::text, piece, {}, {}});
				}
				if (ch.contains("finish_reason") && !ch["finish_reason"].is_null())
					out.stop_reason = ch["finish_reason"].get<std::string>();
			}
		};

		auto sink = [&](std::string_view bytes) -> bool {
			parser.feed(bytes, on_event);
			return !stop();
		};

		auto r = http::post_stream(base_ + "/chat/completions", headers(), body.dump(), sink);
		if (!r) return std::unexpected(r.error());
		parser.finish(on_event);
		if (r->status != 200)
			return fail(errc::http_status, "http " + std::to_string(r->status) + ": " + r->error_body);

		out.reply = message::text(role::assistant, text_buf);
		on_delta({stream_delta::kind::done, out.stop_reason, {}, {}});
		return out;
	}

   private:
	http::headers headers() const {
		http::headers h;
		if (!key_.empty()) h.add("Authorization: Bearer " + key_);
		h.add("content-type: application/json");
		return h;
	}

	std::string kind_, base_, key_;
};

}  // namespace

std::unique_ptr<provider> make_openai_compatible(const std::string& kind, const std::string& base,
                                                 const std::string& key) {
	return std::make_unique<openai_provider>(kind, base, key);
}

}  // namespace plume
