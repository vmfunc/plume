// the provider seam. every backend (anthropic, openai, openrouter, ollama, and
// the generic openai-compatible base) implements this. the ui talks only to
// this interface and never to a specific wire format. streaming runs on a
// worker thread; deltas are handed back through a callback the caller marshals
// onto the render loop.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "plume/error.hpp"
#include "plume/message.hpp"

namespace plume {

// what a model can do, resolved from the models endpoint + per-provider rules.
// prefill is the one that gates mid-message weaving: anthropic rejects it on
// the thinking-capable 4.6+ line, so it is feature-detected per request.
struct features {
	bool prefill = false;
	bool thinking = false;
	bool vision = false;
	bool pdf = false;
	bool tools = false;
	bool prompt_cache = false;
};

struct model_info {
	std::string id;  // never hardcoded; comes from the list endpoint
	std::string display_name;
	std::int64_t context = 0;  // max input tokens
	std::int64_t max_output = 0;
	features caps;
};

enum class thinking_mode : std::uint8_t {
	off,
	adaptive,  // 4.6+ line: let the model decide depth, tuned by effort
	budget,    // older line: a fixed budget_tokens ceiling
};

struct sampling_params {
	std::string model;
	int max_tokens = 4096;
	std::optional<double> temperature;  // dropped for the 4.7+ line
	std::optional<double> top_p;
	thinking_mode thinking = thinking_mode::off;
	int thinking_budget = 0;  // budget mode only
	std::string effort;       // "", low, medium, high, xhigh, max
};

struct tool_def {
	std::string name;
	std::string description;
	std::string input_schema_json;
};

struct request {
	sampling_params params;
	std::optional<std::string> system;
	std::vector<message> messages;
	std::vector<tool_def> tools;

	// weaving: continue an assistant turn from this exact text. the provider
	// must reject (unsupported) when the model can't prefill, rather than
	// silently dropping it.
	std::optional<std::string> assistant_prefill;

	// place a cache_control breakpoint after the system prompt / tool list.
	bool cache_prefix = false;

	// enable the anthropic server-side web search tool for this request.
	bool web_search = false;
};

// one increment off the wire. the ui appends text/thinking as it arrives and
// assembles tool_use inputs from the json fragments.
struct stream_delta {
	enum class kind : std::uint8_t {
		text,       // text holds the delta
		thinking,   // text holds the delta
		signature,  // text holds a thinking signature (on block close)
		tool_use,   // tool_id + text (the tool name) begin a tool_use block
		tool_json,  // text holds a fragment of the current tool's input json
		usage,      // tokens is populated
		done,       // text holds the stop_reason
		ping,       // keepalive; nothing to render
	};
	kind type = kind::text;
	std::string text;
	std::string tool_id;
	usage tokens;
};

struct completion {
	message reply;  // the fully assembled assistant turn
	usage tokens;
	std::string stop_reason;
	std::string model;  // the id that actually served the turn
};

// how a provider finds its credential. resolved once at startup; a key_cmd is
// run in a subprocess, the keychain is queried through the platform backend.
struct auth {
	enum class source : std::uint8_t { env, key_cmd, keychain, inline_key } kind = source::env;
	std::string value;  // env var name, the command, the account, or the key
};

class provider {
   public:
	virtual ~provider() = default;

	[[nodiscard]] virtual std::string_view name() const = 0;

	// runtime discovery. cached by the caller, refreshed on demand.
	[[nodiscard]] virtual result<std::vector<model_info>> list_models() = 0;

	[[nodiscard]] virtual result<std::int64_t> count_tokens(const request&) = 0;

	// stream one completion. on_delta fires per event on this thread. stop is
	// polled between reads so esc cancels promptly. returns the assembled turn.
	[[nodiscard]] virtual result<completion> stream(
	    const request&, const std::function<void(const stream_delta&)>& on_delta,
	    const std::function<bool()>& stop) = 0;
};

// backend selection. the generic openai-compatible base carries a base_url.
struct provider_config {
	std::string kind;      // "anthropic", "openai", "openrouter", "ollama", "openai-compatible"
	std::string base_url;  // empty means the backend's default
	auth credential;
};

[[nodiscard]] result<std::unique_ptr<provider>> make_provider(const provider_config&);

}  // namespace plume
