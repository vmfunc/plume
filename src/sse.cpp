#include "plume/sse.hpp"

namespace plume {

void sse_parser::feed(std::string_view bytes, const sink& out) {
	pending_.append(bytes);

	std::size_t start = 0;
	std::size_t i = 0;
	const std::string_view buf{pending_};
	while (i < buf.size()) {
		const char c = buf[i];
		if (c == '\n') {
			take_line(buf.substr(start, i - start), out);
			++i;
			start = i;
		} else if (c == '\r') {
			// a lone trailing '\r' might be the first half of a CRLF split
			// across two reads; leave it pending and wait for the next feed.
			if (i + 1 >= buf.size()) break;
			take_line(buf.substr(start, i - start), out);
			++i;
			if (buf[i] == '\n') ++i;  // consume the LF of a CRLF pair
			start = i;
		} else {
			++i;
		}
	}
	pending_.erase(0, start);
}

void sse_parser::finish(const sink& out) {
	if (!pending_.empty()) {
		take_line(pending_, out);
		pending_.clear();
	}
	dispatch(out);
}

void sse_parser::reset() {
	pending_.clear();
	cur_type_.clear();
	cur_data_.clear();
	have_event_ = false;
}

void sse_parser::take_line(std::string_view line, const sink& out) {
	if (line.empty()) {
		dispatch(out);
		return;
	}
	if (line.front() == ':') return;  // comment / keepalive

	const auto colon = line.find(':');
	std::string_view field = line;
	std::string_view value;
	if (colon != std::string_view::npos) {
		field = line.substr(0, colon);
		value = line.substr(colon + 1);
		if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
	}

	if (field == "event") {
		cur_type_.assign(value);
		have_event_ = true;
	} else if (field == "data") {
		cur_data_.append(value);
		cur_data_.push_back('\n');
		have_event_ = true;
	}
	// id and retry fields are meaningless to us; ignore them.
}

void sse_parser::dispatch(const sink& out) {
	if (!have_event_) return;
	std::string data = cur_data_;
	if (!data.empty() && data.back() == '\n') data.pop_back();  // spec: strip trailing lf
	out(sse_event{cur_type_, std::move(data)});
	cur_type_.clear();
	cur_data_.clear();
	have_event_ = false;
}

}  // namespace plume
