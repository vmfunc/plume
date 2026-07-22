#include "plume/image.hpp"

#include <chafa.h>

#include <algorithm>
#include <cmath>

#include "kitty_diacritics.hpp"
#include "util_base64.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

namespace plume::img {

namespace {

std::string utf8(std::uint32_t cp) {
	std::string s;
	auto push = [&](unsigned b) { s.push_back(static_cast<char>(b)); };
	if (cp < 0x80) {
		push(cp);
	} else if (cp < 0x800) {
		push(0xC0 | (cp >> 6));
		push(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		push(0xE0 | (cp >> 12));
		push(0x80 | ((cp >> 6) & 0x3F));
		push(0x80 | (cp & 0x3F));
	} else {
		push(0xF0 | (cp >> 18));
		push(0x80 | ((cp >> 12) & 0x3F));
		push(0x80 | ((cp >> 6) & 0x3F));
		push(0x80 | (cp & 0x3F));
	}
	return s;
}

const std::string kPlaceholder = utf8(0x10EEEE);

// a cell footprint that fits max_cols x max_rows while preserving the image's
// aspect against a terminal cell being roughly twice as tall as wide.
void fit(int w, int h, int max_cols, int max_rows, int& cols, int& rows) {
	const double aspect = h > 0 ? static_cast<double>(w) / h : 1.0;
	cols = std::max(1, max_cols);
	rows = std::max(1, static_cast<int>(std::lround(cols / aspect / 2.0)));
	if (rows > max_rows) {
		rows = std::max(1, max_rows);
		cols = std::max(1, static_cast<int>(std::lround(rows * aspect * 2.0)));
	}
	cols = std::min({cols, max_cols, kDiacriticCount});
	rows = std::min({rows, max_rows, kDiacriticCount});
}

result<placement> render_chafa(const bitmap& bm, int cols, int rows) {
	ChafaSymbolMap* map = chafa_symbol_map_new();
	chafa_symbol_map_add_by_tags(
	    map, static_cast<ChafaSymbolTags>(CHAFA_SYMBOL_TAG_HALF | CHAFA_SYMBOL_TAG_SPACE |
	                                      CHAFA_SYMBOL_TAG_BLOCK));

	ChafaCanvasConfig* cfg = chafa_canvas_config_new();
	chafa_canvas_config_set_geometry(cfg, cols, rows);
	chafa_canvas_config_set_symbol_map(cfg, map);
	chafa_canvas_config_set_canvas_mode(cfg, CHAFA_CANVAS_MODE_TRUECOLOR);

	ChafaCanvas* canvas = chafa_canvas_new(cfg);
	chafa_canvas_draw_all_pixels(canvas, CHAFA_PIXEL_RGBA8_UNASSOCIATED, bm.rgba.data(), bm.width,
	                             bm.height, bm.width * 4);
	GString* gs = chafa_canvas_print(canvas, nullptr);

	placement out;
	out.cols = cols;
	out.rows_high = rows;
	std::string ansi(gs->str, gs->len);
	std::size_t start = 0;
	while (start <= ansi.size()) {
		const std::size_t nl = ansi.find('\n', start);
		if (nl == std::string::npos) {
			if (start < ansi.size()) out.rows.push_back(ansi.substr(start));
			break;
		}
		out.rows.push_back(ansi.substr(start, nl - start));
		start = nl + 1;
	}

	g_string_free(gs, TRUE);
	chafa_canvas_unref(canvas);
	chafa_canvas_config_unref(cfg);
	chafa_symbol_map_unref(map);
	return out;
}

}  // namespace

std::string rowcolumn_diacritic(int n) {
	if (n < 0 || n >= kDiacriticCount) return {};
	return utf8(kRowColumnDiacritics[n]);
}

std::string image_id_fg(std::uint32_t id) {
	const unsigned r = (id >> 16) & 0xFF, g = (id >> 8) & 0xFF, b = id & 0xFF;
	return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) +
	       "m";
}

std::string kitty_transmit(std::uint32_t id, const bitmap& bm, int cols, int rows) {
	const std::string data = detail::base64_encode(
	    std::string_view(reinterpret_cast<const char*>(bm.rgba.data()), bm.rgba.size()));

	// first chunk carries the control keys; the payload is split at 4096 base64
	// bytes with m=1 on every chunk but the last, per the protocol.
	std::string keys = "a=T,U=1,q=2,i=" + std::to_string(id) +
	                   ",f=32,s=" + std::to_string(bm.width) + ",v=" + std::to_string(bm.height) +
	                   ",c=" + std::to_string(cols) + ",r=" + std::to_string(rows);

	constexpr std::size_t chunk = 4096;
	std::string out;
	if (data.size() <= chunk) {
		out += "\x1b_G" + keys + ";" + data + "\x1b\\";
		return out;
	}
	for (std::size_t off = 0; off < data.size(); off += chunk) {
		const bool last = off + chunk >= data.size();
		const std::string_view slice(data.data() + off, std::min(chunk, data.size() - off));
		if (off == 0)
			out += "\x1b_G" + keys + ",m=1;" + std::string(slice) + "\x1b\\";
		else
			out += "\x1b_Gm=" + std::string(last ? "0" : "1") + ";" + std::string(slice) + "\x1b\\";
	}
	return out;
}

result<bitmap> decode(std::string_view path) {
	int w = 0, h = 0, n = 0;
	std::string p(path);
	unsigned char* pixels = stbi_load(p.c_str(), &w, &h, &n, 4);
	if (!pixels)
		return fail(errc::io, std::string("cannot decode image: ") + stbi_failure_reason());
	bitmap bm;
	bm.width = w;
	bm.height = h;
	bm.rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
	stbi_image_free(pixels);
	return bm;
}

result<placement> pipeline::render(std::string_view path, int max_cols, int max_rows) {
	auto bm = decode(path);
	if (!bm) return std::unexpected(bm.error());

	int cols = 0, rows = 0;
	fit(bm->width, bm->height, max_cols, max_rows, cols, rows);

	if (mode_ == term::image_mode::kitty_placeholder) {
		placement out;
		out.cols = cols;
		out.rows_high = rows;
		const std::uint32_t id = next_id_++;
		out.setup = kitty_transmit(id, *bm, cols, rows);
		const std::string fg = image_id_fg(id);
		for (int r = 0; r < rows; ++r) {
			std::string line = fg;
			const std::string rd = rowcolumn_diacritic(r);
			for (int c = 0; c < cols; ++c) line += kPlaceholder + rd + rowcolumn_diacritic(c);
			line += "\x1b[39m";
			out.rows.push_back(std::move(line));
		}
		return out;
	}

	// sixel-capable and halfblock terminals both render through chafa; the cell
	// output reflows in the layout like text. native sixel passthrough is a
	// future refinement (see docs).
	return render_chafa(*bm, cols, rows);
}

}  // namespace plume::img
