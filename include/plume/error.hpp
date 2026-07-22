// recoverable failure carried by std::expected. exceptions are reserved for
// true boundaries (startup, the plugin sandbox edge); everything inside a
// module returns one of these instead of throwing across the seam.
#pragma once

#include <expected>
#include <string>
#include <utility>

namespace plume {

enum class errc {
	network,      // socket died, dns, tls, timeout
	http_status,  // provider returned a non-2xx; detail holds the body
	parse,        // malformed sse, half a json object, bad wire shape
	sqlite,       // storage layer refused
	config,       // config file is wrong, not just absent
	io,           // filesystem
	auth,         // no usable credential, or the provider rejected it
	unsupported,  // feature-detected off for this model/terminal
	not_found,    // asked for an id that isn't there
	cancelled,    // the user hit esc, or we're shutting down
};

struct error {
	errc code;
	std::string detail;

	error(errc c, std::string d) : code(c), detail(std::move(d)) {}
};

template <typename T>
using result = std::expected<T, error>;

// terse constructors so call sites read `return fail(errc::parse, "...")`
[[nodiscard]] inline std::unexpected<error> fail(errc c, std::string detail) {
	return std::unexpected(error{c, std::move(detail)});
}

[[nodiscard]] const char* name(errc c) noexcept;

}  // namespace plume
