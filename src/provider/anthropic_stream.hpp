// the anthropic streaming assembler, factored out so it can be driven from a
// recorded fixture in tests as well as from a live socket in the provider. it
// turns the sequence of sse events into stream_deltas (for the ui) and a final
// assembled completion. src-local.
#pragma once

#include <functional>
#include <string>

#include "plume/provider.hpp"
#include "plume/sse.hpp"

namespace plume::anthropic_detail {

class assembler {
   public:
	explicit assembler(std::function<void(const stream_delta&)> on_delta)
	    : on_delta_(std::move(on_delta)) {}

	// feed one decoded sse event.
	void event(const sse_event&);
	// call once the stream ends, to flush the last open block.
	void finish() { finish_block(); }

	[[nodiscard]] const completion& result() const { return out_; }
	[[nodiscard]] completion take() { return std::move(out_); }
	[[nodiscard]] const std::string& error() const { return sse_error_; }
	void set_fallback_model(std::string m) { out_.model = std::move(m); }

   private:
	void finish_block();

	std::function<void(const stream_delta&)> on_delta_;
	completion out_;
	std::string sse_error_;
	std::string cur_type_, text_, thinking_, sig_, tool_id_, tool_name_, tool_json_;
};

}  // namespace plume::anthropic_detail
