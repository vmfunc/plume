// the first-run wizard: a short, animated setup that ends with a working
// conversation. it owns its own state and does its own text input (so it needs
// no ftxui Input component), and it validates a provider on a worker thread the
// same way the chat loop streams. when it finishes it hands back a `config` for
// the app to persist and adopt.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "plume/config.hpp"
#include "plume/terminal.hpp"
#include "plume/theme.hpp"

namespace ftxui {
class ScreenInteractive;
}

namespace plume {

struct wizard {
	enum class step : std::uint8_t { caps, provider, theme, keys, import_, done };

	step at = step::caps;
	bool active = false;
	bool finished = false;  // set once result is ready to apply
	config result;          // the config to persist and adopt

	term::capabilities caps;
	std::int64_t entered_ms = 0;  // when the current step began, for the reveal

	// provider step
	int provider_pick = 0;
	int psub = 0;           // 0 pick provider, 1 pick auth, 2 enter key
	int auth_pick = 0;      // env | paste | key_cmd | keychain
	std::string key_input;  // env var name / pasted key / command / account
	bool editing = false;   // typing into key_input

	// validation, run on a worker
	std::atomic<bool> validating{false};
	std::atomic<int> validated{0};  // 0 unknown, 1 ok, -1 failed
	std::string validate_note;
	std::string validate_model;
	std::thread val_thread;

	int theme_pick = 0;
	int keys_pick = 0;     // vim | emacs
	int density_pick = 0;  // cozy | compact
	int keys_focus = 0;    // which row on the keys step

	std::string import_path;

	wizard() = default;
	~wizard();
	wizard(const wizard&) = delete;
	wizard& operator=(const wizard&) = delete;

	void begin(const config& base, term::capabilities c, std::int64_t ms);
	[[nodiscard]] theme preview_theme(bool dark_bg) const;  // follows theme_pick

	[[nodiscard]] ftxui::Element render(const theme&, std::int64_t ms);
	// consume an event; may spawn validation via the screen. returns true if handled.
	bool handle(const ftxui::Event&, ftxui::ScreenInteractive&);

	[[nodiscard]] bool animating() const { return validating.load() || active; }
};

}  // namespace plume
