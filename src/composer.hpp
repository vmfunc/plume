// a modal, vim-keyed line editor for the composer. single line (multi-line goes
// out to $EDITOR): normal and insert modes, hjkl/w/b/e/0/$ motions, i/a/A/I,
// x/D/C, dd/cc/yy, p, counts, and ci"/ca(/ciw-class text objects. src-local.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "plume/theme.hpp"

namespace plume {

class composer {
   public:
	enum class result : std::uint8_t { none, submit, to_editor };

	[[nodiscard]] result handle(const ftxui::Event&);
	[[nodiscard]] ftxui::Element render(const theme&) const;

	[[nodiscard]] const std::string& value() const { return buf_; }
	[[nodiscard]] bool insert_mode() const { return mode_ == mode::insert; }
	void clear();
	void set_text(std::string s);  // after an $EDITOR round-trip

   private:
	enum class mode : std::uint8_t { normal, insert };

	void insert_str(const std::string&);
	[[nodiscard]] std::size_t next_word(std::size_t) const;
	[[nodiscard]] std::size_t prev_word(std::size_t) const;
	[[nodiscard]] std::size_t word_end(std::size_t) const;
	[[nodiscard]] bool text_object(char obj, bool around, std::size_t& lo, std::size_t& hi) const;
	void op_delete(std::size_t lo, std::size_t hi, bool then_insert);

	mode mode_ = mode::insert;  // ready to type on first focus
	std::string buf_;
	std::size_t cur_ = 0;
	std::string reg_;   // yank / change register
	int count_ = 0;     // pending numeric count
	char op_ = 0;       // pending operator: d, c, y
	char obj_mod_ = 0;  // pending text-object modifier: i or a
};

}  // namespace plume
