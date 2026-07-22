// standard base64, used by the osc 52 clipboard write and the kitty image
// transmit. src-local; not part of the public surface.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace plume::detail {

inline std::string base64_encode(std::string_view in) {
	static constexpr char tbl[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	std::size_t i = 0;
	for (; i + 3 <= in.size(); i += 3) {
		const auto a = static_cast<unsigned char>(in[i]);
		const auto b = static_cast<unsigned char>(in[i + 1]);
		const auto c = static_cast<unsigned char>(in[i + 2]);
		out.push_back(tbl[a >> 2]);
		out.push_back(tbl[((a & 0x3) << 4) | (b >> 4)]);
		out.push_back(tbl[((b & 0xf) << 2) | (c >> 6)]);
		out.push_back(tbl[c & 0x3f]);
	}
	if (const std::size_t rem = in.size() - i; rem == 1) {
		const auto a = static_cast<unsigned char>(in[i]);
		out.push_back(tbl[a >> 2]);
		out.push_back(tbl[(a & 0x3) << 4]);
		out.append("==");
	} else if (rem == 2) {
		const auto a = static_cast<unsigned char>(in[i]);
		const auto b = static_cast<unsigned char>(in[i + 1]);
		out.push_back(tbl[a >> 2]);
		out.push_back(tbl[((a & 0x3) << 4) | (b >> 4)]);
		out.push_back(tbl[(b & 0xf) << 2]);
		out.push_back('=');
	}
	return out;
}

}  // namespace plume::detail
