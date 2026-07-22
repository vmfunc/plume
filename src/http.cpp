#include "http.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <mutex>

namespace plume::http {

namespace {

void ensure_global_init() {
	static std::once_flag once;
	std::call_once(once, [] { curl_global_init(CURL_GLOBAL_ALL); });
}

// a curl_slist that frees itself.
struct slist {
	curl_slist* head = nullptr;
	explicit slist(const headers& h) {
		for (const auto& line : h.items) head = curl_slist_append(head, line.c_str());
	}
	~slist() {
		if (head) curl_slist_free_all(head);
	}
	slist(const slist&) = delete;
	slist& operator=(const slist&) = delete;
};

std::size_t append_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
	auto* out = static_cast<std::string*>(userdata);
	out->append(ptr, size * nmemb);
	return size * nmemb;
}

void common_opts(CURL* c, const slist& hdrs) {
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs.head);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);  // safe to use off the main thread
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "plume/0.1");
}

}  // namespace

result<response> get(const std::string& url, const headers& h, int timeout_s) {
	ensure_global_init();
	CURL* c = curl_easy_init();
	if (!c) return fail(errc::network, "curl init failed");
	slist hdrs(h);
	response r;
	common_opts(c, hdrs);
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, append_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
	const CURLcode rc = curl_easy_perform(c);
	if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) return fail(errc::network, curl_easy_strerror(rc));
	return r;
}

result<response> post(const std::string& url, const headers& h, std::string_view body,
                      int timeout_s) {
	ensure_global_init();
	CURL* c = curl_easy_init();
	if (!c) return fail(errc::network, "curl init failed");
	slist hdrs(h);
	response r;
	common_opts(c, hdrs);
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, append_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
	const CURLcode rc = curl_easy_perform(c);
	if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) return fail(errc::network, curl_easy_strerror(rc));
	return r;
}

namespace {

// state threaded through the streaming write callback.
struct stream_ctx {
	const chunk_fn* on_chunk = nullptr;
	long status = 0;
	bool status_known = false;
	std::string error_body;
	bool aborted = false;
};

std::size_t stream_write(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
	auto* ctx = static_cast<stream_ctx*>(userdata);
	const std::size_t n = size * nmemb;
	const std::string_view chunk{ptr, n};

	// once the response is a failure, the body is a small json error, not sse.
	if (ctx->status_known && ctx->status >= 400) {
		ctx->error_body.append(chunk);
		return n;
	}
	if (!(*ctx->on_chunk)(chunk)) {
		ctx->aborted = true;
		return 0;  // signal curl to stop
	}
	return n;
}

std::size_t header_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
	auto* ctx = static_cast<stream_ctx*>(userdata);
	std::string_view line{ptr, size * nmemb};
	if (line.rfind("HTTP/", 0) == 0) {
		// a redirect chain resets this; the last status line wins.
		if (const auto sp = line.find(' '); sp != std::string_view::npos) {
			ctx->status = std::strtol(line.data() + sp + 1, nullptr, 10);
			ctx->status_known = true;
		}
	}
	return size * nmemb;
}

}  // namespace

result<stream_result> post_stream(const std::string& url, const headers& h, std::string_view body,
                                  const chunk_fn& on_chunk, int connect_timeout_s) {
	ensure_global_init();
	CURL* c = curl_easy_init();
	if (!c) return fail(errc::network, "curl init failed");
	slist hdrs(h);
	stream_ctx ctx;
	ctx.on_chunk = &on_chunk;
	common_opts(c, hdrs);
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, &ctx);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, static_cast<long>(connect_timeout_s));
	// no total timeout: a long thinking turn can legitimately run minutes. drop
	// only genuinely dead connections (under 32 bytes/s for a full minute).
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 32L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);

	const CURLcode rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &ctx.status);
	curl_easy_cleanup(c);

	if (ctx.aborted) return fail(errc::cancelled, "stream aborted by caller");
	if (rc != CURLE_OK) return fail(errc::network, curl_easy_strerror(rc));
	return stream_result{ctx.status, std::move(ctx.error_body)};
}

}  // namespace plume::http
