// a truecolor triple and hex parsing. shared by the theme loader and the
// terminal background query.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace plume {

struct rgb {
	std::uint8_t r = 0, g = 0, b = 0;
	auto operator<=>(const rgb&) const = default;
};

// "#rrggbb" or "rrggbb". returns nullopt on anything else.
[[nodiscard]] std::optional<rgb> parse_hex(std::string_view);
[[nodiscard]] std::string to_hex(rgb);

// perceived luminance, for picking a light vs dark variant from OSC 11.
[[nodiscard]] bool is_dark(rgb) noexcept;

}  // namespace plume
