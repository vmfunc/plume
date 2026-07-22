// non-interactive entry points: plume ask "...", export, import, themes,
// doctor, config edit. ask streams to stdout honoring config and reads stdin
// when piped; --json, --model and --no-stream tune it.
#pragma once

namespace plume {

// dispatch argv. returns a process exit code. when argv names no subcommand
// (or names a conversation to resume) this returns std::nullopt-equivalent -1
// to signal "fall through to the tui".
[[nodiscard]] int run_cli(int argc, char** argv);

}  // namespace plume
