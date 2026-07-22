// the terminal is hostile until proven otherwise. we probe for what it can do
// at startup and degrade rather than print garbage into someone's session. the
// escape builders are pure string functions (easy to test); probe() does the
// tty query dance behind a raw-mode guard.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "plume/color.hpp"

namespace plume::term {

// how inline images can be drawn, best first.
enum class image_mode { kitty_placeholder, sixel, halfblock };

struct capabilities {
	bool truecolor = false;
	bool kitty_graphics = false;  // unicode-placeholder transport available
	bool sixel = false;
	bool italics = false;
	bool osc52 = false;                // clipboard write
	bool synchronized_output = false;  // mode 2026
	std::optional<rgb> background;     // from OSC 11; nullopt if no reply
	bool dark = true;                  // derived from background, else assumed

	[[nodiscard]] image_mode images() const noexcept;
};

// query the terminal. reads $TERM/$COLORTERM/$TERM_PROGRAM and, when stdin is a
// tty, sends OSC 11 and the kitty/sixel probes with a short timeout. never
// blocks longer than a couple hundred milliseconds.
[[nodiscard]] capabilities probe();

// pure escape builders --------------------------------------------------------

// osc 52: copy to the system clipboard through the terminal (works over ssh).
[[nodiscard]] std::string osc52_copy(std::string_view utf8);

// osc 8: a hyperlink wrapping visible text.
[[nodiscard]] std::string osc8_link(std::string_view url, std::string_view text);

// mode 2026: wrap a frame so streaming never tears.
[[nodiscard]] std::string sync_begin();
[[nodiscard]] std::string sync_end();

// completion notifications when unfocused.
[[nodiscard]] std::string bell();                         // BEL
[[nodiscard]] std::string osc9_notify(std::string_view);  // desktop toast

}  // namespace plume::term
