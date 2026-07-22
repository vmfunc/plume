#include <doctest/doctest.h>

#include <filesystem>
#include <vector>

#include "plume/ids.hpp"
#include "plume/image.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

using namespace plume;

namespace {
// a small rgba test image written to a temp png so decode has something real.
std::string make_png(int w, int h) {
	std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			auto* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
			p[0] = static_cast<unsigned char>(x * 255 / w);
			p[1] = static_cast<unsigned char>(y * 255 / h);
			p[2] = 128;
			p[3] = 255;
		}
	auto path = std::filesystem::temp_directory_path() / (new_id("img") + ".png");
	stbi_write_png(path.string().c_str(), w, h, 4, px.data(), w * 4);
	return path.string();
}
}  // namespace

TEST_CASE("rowcolumn diacritics match the kitty table and bound at 297") {
	// n=0 is U+0305 (combining overline), the first entry in the canonical list.
	CHECK(img::rowcolumn_diacritic(0) == "\xCC\x85");  // utf-8 of U+0305
	CHECK_FALSE(img::rowcolumn_diacritic(296).empty());
	CHECK(img::rowcolumn_diacritic(297).empty());  // out of range
	CHECK(img::rowcolumn_diacritic(-1).empty());
}

TEST_CASE("image id rides in the cell foreground color") {
	// id 0x010203 -> r=1 g=2 b=3.
	CHECK(img::image_id_fg(0x010203) == "\x1b[38;2;1;2;3m");
}

TEST_CASE("kitty transmit is a well-formed graphics escape") {
	img::bitmap bm;
	bm.width = 2;
	bm.height = 2;
	bm.rgba.assign(2 * 2 * 4, 0xAB);
	auto esc = img::kitty_transmit(7, bm, 4, 2);
	CHECK(esc.rfind("\x1b_G", 0) == 0);              // starts with the APC introducer
	CHECK(esc.find("i=7") != std::string::npos);     // image id
	CHECK(esc.find("U=1") != std::string::npos);     // unicode placeholder mode
	CHECK(esc.find("c=4") != std::string::npos);     // virtual placement columns
	CHECK(esc.find("r=2") != std::string::npos);     // rows
	CHECK(esc.find("\x1b\\") != std::string::npos);  // string terminator
}

TEST_CASE("pipeline places an image as kitty placeholder cells") {
	const std::string png = make_png(32, 16);
	img::pipeline pipe(term::image_mode::kitty_placeholder, "/tmp");
	auto pl = pipe.render(png, 20, 20);
	REQUIRE(pl.has_value());
	CHECK(pl->cols >= 1);
	CHECK(pl->cols <= 20);
	CHECK(static_cast<int>(pl->rows.size()) == pl->rows_high);
	CHECK(pl->setup.rfind("\x1b_G", 0) == 0);
	// every placement cell carries the U+10EEEE placeholder (utf-8 f4 8e bb ae).
	CHECK(pl->rows.front().find("\xF4\x8E\xBB\xAE") != std::string::npos);
	std::filesystem::remove(png);
}

TEST_CASE("pipeline falls back to chafa cell rows when kitty is absent") {
	const std::string png = make_png(24, 24);
	img::pipeline pipe(term::image_mode::halfblock, "/tmp");
	auto pl = pipe.render(png, 12, 12);
	REQUIRE(pl.has_value());
	CHECK_FALSE(pl->rows.empty());
	std::filesystem::remove(png);
}
