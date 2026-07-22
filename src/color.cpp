#include "plume/color.hpp"

#include <array>
#include <cctype>

namespace plume {

namespace {

std::optional<int> hex_nibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	return std::nullopt;
}

std::optional<std::uint8_t> hex_byte(char hi, char lo) {
	auto h = hex_nibble(hi);
	auto l = hex_nibble(lo);
	if (!h || !l) return std::nullopt;
	return static_cast<std::uint8_t>((*h << 4) | *l);
}

}  // namespace

std::optional<rgb> parse_hex(std::string_view s) {
	if (!s.empty() && s.front() == '#') s.remove_prefix(1);
	if (s.size() != 6) return std::nullopt;
	auto r = hex_byte(s[0], s[1]);
	auto g = hex_byte(s[2], s[3]);
	auto b = hex_byte(s[4], s[5]);
	if (!r || !g || !b) return std::nullopt;
	return rgb{*r, *g, *b};
}

std::string to_hex(rgb c) {
	static constexpr char d[] = "0123456789abcdef";
	std::array<char, 7> buf{'#',          d[c.r >> 4], d[c.r & 0xf], d[c.g >> 4],
	                        d[c.g & 0xf], d[c.b >> 4], d[c.b & 0xf]};
	return std::string(buf.data(), buf.size());
}

bool is_dark(rgb c) noexcept {
	// rec. 601 luma; the threshold is the usual midpoint.
	const double luma = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
	return luma < 128.0;
}

}  // namespace plume
