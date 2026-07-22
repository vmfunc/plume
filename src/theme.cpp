#include "plume/theme.hpp"

#include <filesystem>

#include <toml++/toml.hpp>

namespace plume {

namespace {

// the palettes are known-good hex; parse_hex never fails on them.
rgb H(std::string_view hex) {
	return parse_hex(hex).value();
}

}  // namespace

theme rose_pine() {
	return {"rose-pine",
	        true,
	        {H("#191724"), H("#1f1d2e"), H("#26233a"), H("#6e6a86"), H("#908caa"), H("#e0def4"),
	         H("#eb6f92"), H("#f6c177"), H("#ebbcba"), H("#31748f"), H("#9ccfd8"), H("#c4a7e7"),
	         H("#21202e"), H("#403d52"), H("#524f67")}};
}

theme rose_pine_moon() {
	return {"rose-pine-moon",
	        true,
	        {H("#232136"), H("#2a273f"), H("#393552"), H("#6e6a86"), H("#908caa"), H("#e0def4"),
	         H("#eb6f92"), H("#f6c177"), H("#ea9a97"), H("#3e8fb0"), H("#9ccfd8"), H("#c4a7e7"),
	         H("#2a283e"), H("#44415a"), H("#56526e")}};
}

theme rose_pine_dawn() {
	return {"rose-pine-dawn",
	        false,
	        {H("#faf4ed"), H("#fffaf3"), H("#f2e9e1"), H("#9893a5"), H("#797593"), H("#575279"),
	         H("#b4637a"), H("#ea9d34"), H("#d7827e"), H("#286983"), H("#56949f"), H("#907aa9"),
	         H("#f4ede8"), H("#dfdad9"), H("#cecacd")}};
}

theme va11() {
	// warm amber base, magenta and cyan neon accents — for the late shift.
	return {"va11",
	        true,
	        {H("#16111a"), H("#1e1622"), H("#2a1f30"), H("#7a6a80"), H("#b39ac0"), H("#f4e8d8"),
	         H("#ff5c8a"), H("#ffb454"), H("#ff7edb"), H("#36c5f0"), H("#5ef1ff"), H("#b57edc"),
	         H("#201826"), H("#3a2c44"), H("#4e3a5a")}};
}

namespace {

void override_role(const toml::table& t, const char* key, rgb& out) {
	if (auto s = t[key].value<std::string>())
		if (auto c = parse_hex(*s)) out = *c;
}

}  // namespace

result<theme> load_theme(const std::string& path) {
	if (!std::filesystem::exists(path)) return fail(errc::not_found, "theme not found: " + path);

	toml::table tbl;
	try {
		tbl = toml::parse_file(path);
	} catch (const toml::parse_error& e) {
		return fail(errc::config, std::string("theme: ") + e.description().data());
	}

	theme th = rose_pine();  // missing roles inherit
	th.name = tbl["name"].value_or(std::filesystem::path(path).stem().string());
	th.dark = tbl["dark"].value_or(true);

	if (auto pal = tbl["palette"].as_table()) {
		override_role(*pal, "base", th.p.base);
		override_role(*pal, "surface", th.p.surface);
		override_role(*pal, "overlay", th.p.overlay);
		override_role(*pal, "muted", th.p.muted);
		override_role(*pal, "subtle", th.p.subtle);
		override_role(*pal, "text", th.p.text);
		override_role(*pal, "love", th.p.love);
		override_role(*pal, "gold", th.p.gold);
		override_role(*pal, "rose", th.p.rose);
		override_role(*pal, "pine", th.p.pine);
		override_role(*pal, "foam", th.p.foam);
		override_role(*pal, "iris", th.p.iris);
		override_role(*pal, "highlight_low", th.p.hl_low);
		override_role(*pal, "highlight_med", th.p.hl_med);
		override_role(*pal, "highlight_high", th.p.hl_high);
	}
	return th;
}

result<theme> resolve_theme(const std::string& name, const std::string& themes_dir, bool dark_bg) {
	if (name == "rose-pine") return dark_bg ? rose_pine() : rose_pine_dawn();
	if (name == "rose-pine-moon") return rose_pine_moon();
	if (name == "rose-pine-dawn") return rose_pine_dawn();
	if (name == "va11") return va11();

	const std::string path = themes_dir + "/" + name + ".toml";
	if (std::filesystem::exists(path)) return load_theme(path);
	return rose_pine();  // unknown name: fall back rather than fail the ui
}

}  // namespace plume
