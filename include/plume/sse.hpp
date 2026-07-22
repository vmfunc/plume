// server-sent events arrive as a byte stream, not a line stream. a single read
// can split an event down the middle or carry three at once; this parser holds
// the partial state between feeds and dispatches only complete events. it does
// not interpret the payload — the provider decodes the json.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace plume {

struct sse_event {
	std::string type;  // the "event:" field; empty if the stream omitted it
	std::string data;  // "data:" lines joined by '\n', per the spec
};

class sse_parser {
   public:
	using sink = std::function<void(const sse_event&)>;

	// consume a chunk of bytes, dispatching every event the chunk completes.
	// bytes that form only a partial line or a partial event are retained.
	void feed(std::string_view bytes, const sink& out);

	// the connection closed; emit a final event if one was buffered without a
	// terminating blank line (providers sometimes do this on a clean close).
	void finish(const sink& out);

	void reset();

   private:
	void take_line(std::string_view line, const sink& out);
	void dispatch(const sink& out);

	std::string pending_;   // bytes past the last newline
	std::string cur_type_;  // fields accumulating for the in-progress event
	std::string cur_data_;
	bool have_event_ = false;  // any field seen since the last dispatch
};

}  // namespace plume
