// inline images without fighting ftxui for the cursor. the kitty graphics
// protocol in unicode-placeholder mode turns an image into ordinary grid cells:
// each cell carries U+10EEEE plus row/column combining diacritics, and the
// image id rides in the cell's foreground color. those cells scroll, reflow and
// composite exactly like text — which is the whole reason a beautiful tui with
// images is tractable. where placeholders are absent we fall to sixel, then to
// chafa halfblocks. scaled renders are cached on disk.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "plume/error.hpp"
#include "plume/terminal.hpp"

namespace plume::img {

// raw pixels, decoded from a file on disk.
struct bitmap {
	int width = 0;
	int height = 0;
	std::vector<std::uint8_t> rgba;  // width*height*4, row-major
};

[[nodiscard]] result<bitmap> decode(std::string_view path);

// a render ready to drop into the grid. `setup` is written once (it transmits
// pixels for kitty, and is empty for the cell-based fallbacks). `rows` are the
// per-line strings to place in the layout; each already carries whatever escape
// state it needs and resets it at end of line.
struct placement {
	int cols = 0;
	int rows_high = 0;
	std::string setup;
	std::vector<std::string> rows;  // rows_high entries
};

// the kitty unicode-placeholder primitives, pulled out so they can be tested in
// isolation (§0). n is a 0-based row or column index; the returned string is the
// utf-8 combining diacritic the protocol assigns to it.
[[nodiscard]] std::string rowcolumn_diacritic(int n);

// the fg color carrying an image id in a cell (24-bit id in the rgb channels).
[[nodiscard]] std::string image_id_fg(std::uint32_t id);

// transmit an rgba image under a given id and create a virtual placement of
// cols x rows cells, chunked as the protocol requires. this is the string
// written once before the placeholder cells are laid into the grid.
[[nodiscard]] std::string kitty_transmit(std::uint32_t id, const bitmap&, int cols, int rows);

class pipeline {
   public:
	pipeline(term::image_mode mode, std::string cache_dir)
	    : mode_(mode), cache_dir_(std::move(cache_dir)) {}

	// render an image file to fit inside max_cols x max_rows character cells,
	// preserving aspect against the terminal's cell shape.
	[[nodiscard]] result<placement> render(std::string_view path, int max_cols, int max_rows);

   private:
	term::image_mode mode_;
	std::string cache_dir_;
	std::uint32_t next_id_ = 1;
};

}  // namespace plume::img
