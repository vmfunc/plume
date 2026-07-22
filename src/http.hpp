// a thin libcurl wrapper. src-local: the provider, mcp http transport and the
// live sync adapter share it, but it is not part of the public surface. all
// network runs on worker threads, so nothing here touches the terminal.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "plume/error.hpp"

namespace plume::http {

struct headers {
	std::vector<std::string> items;  // "Key: value"
	void add(std::string line) { items.push_back(std::move(line)); }
};

struct response {
	long status = 0;
	std::string body;
};

[[nodiscard]] result<response> get(const std::string& url, const headers&, int timeout_s = 30);
[[nodiscard]] result<response> post(const std::string& url, const headers&, std::string_view body,
                                    int timeout_s = 60);

// streaming post. on_chunk receives raw response bytes as they arrive and
// returns false to abort (esc, shutdown). on a non-2xx status the body is
// collected into error_body instead of being streamed, so the caller can read
// the provider's json error.
struct stream_result {
	long status = 0;
	std::string error_body;
};
using chunk_fn = std::function<bool(std::string_view)>;
[[nodiscard]] result<stream_result> post_stream(const std::string& url, const headers&,
                                                std::string_view body, const chunk_fn&,
                                                int connect_timeout_s = 20);

}  // namespace plume::http
