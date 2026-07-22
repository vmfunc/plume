#include "plume/terminal.hpp"

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "util_base64.hpp"

namespace plume::term {

namespace {

bool env_is(const char* var, std::string_view want) {
	const char* v = std::getenv(var);
	return v && want == v;
}

bool env_has(const char* var) {
	const char* v = std::getenv(var);
	return v && *v;
}

std::string env_str(const char* var) {
	const char* v = std::getenv(var);
	return v ? v : "";
}

// a single interactive query against the controlling terminal: raw mode, write
// the request, poll for a reply until a terminator byte or a short timeout.
// returns "" when there is no tty or nothing answered.
std::string query(std::string_view request, char terminator, int timeout_ms) {
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return {};

	termios saved{};
	if (tcgetattr(STDIN_FILENO, &saved) != 0) return {};
	termios raw = saved;
	raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return {};

	std::string reply;
	const ssize_t wrote = ::write(STDOUT_FILENO, request.data(), request.size());
	if (wrote == static_cast<ssize_t>(request.size())) {
		pollfd pfd{STDIN_FILENO, POLLIN, 0};
		std::array<char, 256> buf{};
		while (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
			const ssize_t n = ::read(STDIN_FILENO, buf.data(), buf.size());
			if (n <= 0) break;
			reply.append(buf.data(), static_cast<std::size_t>(n));
			if (reply.find(terminator) != std::string::npos) break;
		}
	}

	tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	return reply;
}

// parse a 16-bit-per-channel OSC 11 reply: "]11;rgb:rrrr/gggg/bbbb".
std::optional<rgb> parse_osc11(std::string_view s) {
	const auto pos = s.find("rgb:");
	if (pos == std::string_view::npos) return std::nullopt;
	s.remove_prefix(pos + 4);
	unsigned r = 0, g = 0, b = 0;
	if (std::sscanf(std::string(s).c_str(), "%x/%x/%x", &r, &g, &b) != 3) return std::nullopt;
	// channels may be 8- or 16-bit wide; take the high byte of a 16-bit value.
	auto hi = [](unsigned v) { return static_cast<std::uint8_t>(v > 0xff ? (v >> 8) : v); };
	return rgb{hi(r), hi(g), hi(b)};
}

}  // namespace

image_mode capabilities::images() const noexcept {
	if (kitty_graphics) return image_mode::kitty_placeholder;
	if (sixel) return image_mode::sixel;
	return image_mode::halfblock;
}

capabilities probe() {
	capabilities caps;
	const std::string term = env_str("TERM");
	const std::string term_prog = env_str("TERM_PROGRAM");

	caps.truecolor = env_is("COLORTERM", "truecolor") || env_is("COLORTERM", "24bit") ||
	                 term.find("truecolor") != std::string::npos || term == "xterm-kitty";

	caps.kitty_graphics = term == "xterm-kitty" || env_has("KITTY_WINDOW_ID") ||
	                      term_prog == "ghostty" || term_prog == "WezTerm";

	caps.italics = term != "linux" && term != "dumb" && !term.empty();
	caps.osc52 = caps.italics;                 // same class of terminal
	caps.synchronized_output = caps.italics;   // unsupported terminals ignore the escape

	// device attributes: sixel support shows as attribute 4 in the DA1 reply.
	if (const std::string da1 = query("\x1b[c", 'c', 120); !da1.empty()) {
		const auto semi = da1.find(';');
		caps.sixel = da1.find(";4;") != std::string::npos || da1.find(";4c") != std::string::npos ||
		             (semi != std::string::npos && da1.compare(semi, 3, ";4c") == 0);
	}

	// background color, for choosing a light vs dark variant.
	if (const std::string bg = query("\x1b]11;?\x1b\\", '\\', 120); !bg.empty()) {
		if (auto c = parse_osc11(bg)) {
			caps.background = *c;
			caps.dark = is_dark(*c);
		}
	}
	return caps;
}

std::string osc52_copy(std::string_view utf8) {
	return "\x1b]52;c;" + detail::base64_encode(utf8) + "\a";
}

std::string osc8_link(std::string_view url, std::string_view text) {
	std::string out = "\x1b]8;;";
	out.append(url);
	out.push_back('\a');
	out.append(text);
	out.append("\x1b]8;;\a");
	return out;
}

std::string sync_begin() { return "\x1b[?2026h"; }
std::string sync_end() { return "\x1b[?2026l"; }
std::string bell() { return "\a"; }

std::string osc9_notify(std::string_view text) {
	std::string out = "\x1b]9;";
	out.append(text);
	out.push_back('\a');
	return out;
}

}  // namespace plume::term
