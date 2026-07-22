// themes are toml palettes, hot-reloaded on save. the palette follows the
// rosé pine role names; the renderer maps roles to chrome (base is background,
// text is foreground, iris/foam carry accents, love signals errors). two
// variants ship built in — rosé pine and va11 — plus moon and dawn.
#pragma once

#include <string>

#include "plume/color.hpp"
#include "plume/error.hpp"

namespace plume {

struct palette {
	rgb base, surface, overlay, muted, subtle, text;
	rgb love, gold, rose, pine, foam, iris;
	rgb hl_low, hl_med, hl_high;
};

struct theme {
	std::string name;
	bool dark = true;
	palette p;
};

[[nodiscard]] theme rose_pine();
[[nodiscard]] theme rose_pine_moon();
[[nodiscard]] theme rose_pine_dawn();
[[nodiscard]] theme va11();

// load a theme.toml. missing roles inherit from rosé pine so a partial file
// still renders.
[[nodiscard]] result<theme> load_theme(const std::string& path);

// resolve a theme by name: a built-in, or <themes_dir>/<name>.toml. when the
// terminal reported a light background, a bare "rose-pine" resolves to dawn.
[[nodiscard]] result<theme> resolve_theme(const std::string& name, const std::string& themes_dir,
                                          bool dark_bg);

}  // namespace plume
