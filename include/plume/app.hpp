// the ui shell. the ui thread owns the terminal outright: all network runs on
// worker threads, and results cross back through a thread-safe queue that wakes
// the ftxui loop with a custom event. nothing writes to stdout except the
// renderer. app::run drives the loop and returns a process exit code.
#pragma once

#include <memory>

#include "plume/config.hpp"
#include "plume/error.hpp"

namespace plume {

class app {
   public:
	[[nodiscard]] static result<app> create(config cfg);

	app(app&&) noexcept;
	app& operator=(app&&) noexcept;
	app(const app&) = delete;
	app& operator=(const app&) = delete;
	~app();

	// run the first-run wizard when the config has no usable provider, then
	// drop into the conversation loop. blocks until the user quits.
	[[nodiscard]] int run();

   private:
	app();
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

}  // namespace plume
