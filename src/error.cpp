#include "plume/error.hpp"

namespace plume {

const char* name(errc c) noexcept {
	switch (c) {
		case errc::network: return "network";
		case errc::http_status: return "http_status";
		case errc::parse: return "parse";
		case errc::sqlite: return "sqlite";
		case errc::config: return "config";
		case errc::io: return "io";
		case errc::auth: return "auth";
		case errc::unsupported: return "unsupported";
		case errc::not_found: return "not_found";
		case errc::cancelled: return "cancelled";
	}
	return "unknown";
}

}  // namespace plume
