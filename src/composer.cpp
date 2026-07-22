#include "composer.hpp"

#include <algorithm>
#include <cctype>

namespace plume {

using namespace ftxui;

namespace {
// 0 whitespace, 1 word (alnum or _), 2 punctuation — the vim motion classes.
int cls(char c) {
	const auto u = static_cast<unsigned char>(c);
	if (std::isspace(u)) return 0;
	if (std::isalnum(u) || c == '_') return 1;
	return 2;
}
}  // namespace

void composer::clear() {
	buf_.clear();
	cur_ = 0;
	count_ = 0;
	op_ = 0;
	obj_mod_ = 0;
	mode_ = mode::insert;
}

void composer::set_text(std::string s) {
	buf_ = std::move(s);
	cur_ = buf_.size();
	mode_ = mode::insert;
}

void composer::insert_str(const std::string& s) {
	buf_.insert(cur_, s);
	cur_ += s.size();
}

std::size_t composer::next_word(std::size_t i) const {
	const std::size_t n = buf_.size();
	if (i >= n) return n;
	const int c = cls(buf_[i]);
	if (c != 0)
		while (i < n && cls(buf_[i]) == c) ++i;
	while (i < n && cls(buf_[i]) == 0) ++i;
	return i;
}

std::size_t composer::prev_word(std::size_t i) const {
	if (i == 0) return 0;
	--i;
	while (i > 0 && cls(buf_[i]) == 0) --i;
	const int c = cls(buf_[i]);
	while (i > 0 && cls(buf_[i - 1]) == c) --i;
	return i;
}

std::size_t composer::word_end(std::size_t i) const {
	const std::size_t n = buf_.size();
	if (i + 1 >= n) return n == 0 ? 0 : n - 1;
	++i;
	while (i < n && cls(buf_[i]) == 0) ++i;
	if (i >= n) return n - 1;
	const int c = cls(buf_[i]);
	while (i + 1 < n && cls(buf_[i + 1]) == c) ++i;
	return i;
}

bool composer::text_object(char obj, bool around, std::size_t& lo, std::size_t& hi) const {
	const std::size_t n = buf_.size();
	if (n == 0) return false;
	const std::size_t at = std::min(cur_, n - 1);

	if (obj == 'w') {
		const int c = cls(buf_[at]);
		std::size_t s = at, e = at;
		while (s > 0 && cls(buf_[s - 1]) == c) --s;
		while (e < n && cls(buf_[e]) == c) ++e;
		if (around)
			while (e < n && cls(buf_[e]) == 0) ++e;  // trailing space
		lo = s;
		hi = e;
		return true;
	}

	auto pair = [&](char open, char close) -> bool {
		std::size_t o = 0;
		bool found_o = false;
		int bal = 0;
		for (std::size_t i = at + 1; i-- > 0;) {
			if (buf_[i] == close && i != at)
				++bal;
			else if (buf_[i] == open) {
				if (bal == 0) {
					o = i;
					found_o = true;
					break;
				}
				--bal;
			}
			if (i == 0) break;
		}
		if (!found_o) return false;
		bal = 0;
		for (std::size_t i = o + 1; i < n; ++i) {
			if (buf_[i] == open)
				++bal;
			else if (buf_[i] == close) {
				if (bal == 0) {
					lo = around ? o : o + 1;
					hi = around ? i + 1 : i;
					return true;
				}
				--bal;
			}
		}
		return false;
	};

	if (obj == '"' || obj == '\'' || obj == '`') {
		std::size_t a = std::string::npos, b = std::string::npos;
		std::size_t last = std::string::npos;
		for (std::size_t i = 0; i < n; ++i) {
			if (buf_[i] != obj) continue;
			if (last == std::string::npos) {
				last = i;
			} else {
				if (last <= at && at <= i) {
					a = last;
					b = i;
					break;
				}
				last = std::string::npos;
			}
		}
		if (a == std::string::npos) return false;
		lo = around ? a : a + 1;
		hi = around ? b + 1 : b;
		return true;
	}
	if (obj == '(' || obj == ')' || obj == 'b') return pair('(', ')');
	if (obj == '[' || obj == ']') return pair('[', ']');
	if (obj == '{' || obj == '}' || obj == 'B') return pair('{', '}');
	return false;
}

void composer::op_delete(std::size_t lo, std::size_t hi, bool then_insert) {
	if (lo > hi) std::swap(lo, hi);
	lo = std::min(lo, buf_.size());
	hi = std::min(hi, buf_.size());
	reg_ = buf_.substr(lo, hi - lo);
	buf_.erase(lo, hi - lo);
	cur_ = lo;
	if (then_insert) mode_ = mode::insert;
}

composer::result composer::handle(const Event& e) {
	const std::size_t n = buf_.size();

	if (e == Event::Return) return result::submit;
	if (e.input() == std::string(1, static_cast<char>(5))) return result::to_editor;  // ctrl-e

	if (mode_ == mode::insert) {
		if (e == Event::Escape) {
			mode_ = mode::normal;
			if (cur_ > 0) --cur_;
		} else if (e == Event::Backspace) {
			if (cur_ > 0) {
				buf_.erase(cur_ - 1, 1);
				--cur_;
			}
		} else if (e.is_character()) {
			insert_str(e.character());
		}
		return result::none;
	}

	// normal mode.
	const int cnt = std::max(1, count_);

	// a pending operator (d/c/y) awaits a motion, a doubled key, or a text object.
	if (op_ != 0) {
		const char op = op_;
		if (obj_mod_ != 0 && e.is_character()) {  // ci"/ca( etc.
			std::size_t lo = 0, hi = 0;
			if (text_object(e.character()[0], obj_mod_ == 'a', lo, hi)) {
				if (op == 'y')
					reg_ = buf_.substr(lo, hi - lo);
				else
					op_delete(lo, hi, op == 'c');
			}
			op_ = 0;
			obj_mod_ = 0;
			count_ = 0;
			return result::none;
		}
		if (e == Event::Character("i") || e == Event::Character("a")) {
			obj_mod_ = e.character()[0];
			return result::none;
		}
		std::size_t lo = cur_, hi = cur_;
		bool ok = true;
		if (e == Event::Character(std::string(1, op))) {  // dd / cc / yy: whole line
			lo = 0;
			hi = n;
		} else if (e == Event::Character("w")) {
			hi = next_word(cur_);
		} else if (e == Event::Character("b")) {
			lo = prev_word(cur_);
		} else if (e == Event::Character("e")) {
			hi = word_end(cur_) + 1;
		} else if (e == Event::Character("$")) {
			hi = n;
		} else if (e == Event::Character("0")) {
			lo = 0;
		} else {
			ok = false;
		}
		if (ok) {
			if (op == 'y')
				reg_ = buf_.substr(std::min(lo, hi), (lo < hi ? hi - lo : lo - hi));
			else
				op_delete(lo, hi, op == 'c');
		}
		op_ = 0;
		count_ = 0;
		return result::none;
	}

	if (e.is_character() && e.character().size() == 1) {
		const char c = e.character()[0];
		if (c >= '1' && c <= '9') {
			count_ = count_ * 10 + (c - '0');
			return result::none;
		}
		if (c == '0' && count_ > 0) {
			count_ = count_ * 10;
			return result::none;
		}
		switch (c) {
			case 'i': mode_ = mode::insert; break;
			case 'a':
				mode_ = mode::insert;
				if (cur_ < n) ++cur_;
				break;
			case 'A':
				cur_ = n;
				mode_ = mode::insert;
				break;
			case 'I':
				cur_ = 0;
				mode_ = mode::insert;
				break;
			case 'h': cur_ = cur_ > static_cast<std::size_t>(cnt) ? cur_ - cnt : 0; break;
			case 'l': cur_ = std::min(n, cur_ + cnt); break;
			case '0': cur_ = 0; break;
			case '$': cur_ = n == 0 ? 0 : n - 1; break;
			case 'w':
				for (int k = 0; k < cnt; ++k) cur_ = next_word(cur_);
				break;
			case 'b':
				for (int k = 0; k < cnt; ++k) cur_ = prev_word(cur_);
				break;
			case 'e':
				for (int k = 0; k < cnt; ++k) cur_ = word_end(cur_);
				break;
			case 'x':
				for (int k = 0; k < cnt && cur_ < buf_.size(); ++k) buf_.erase(cur_, 1);
				if (cur_ > 0 && cur_ >= buf_.size()) --cur_;
				break;
			case 'D': op_delete(cur_, n, false); break;
			case 'C': op_delete(cur_, n, true); break;
			case 'd': op_ = 'd'; break;
			case 'c': op_ = 'c'; break;
			case 'y': op_ = 'y'; break;
			case 'p':
				if (!reg_.empty()) {
					if (cur_ < n) ++cur_;
					insert_str(reg_);
					if (cur_ > 0) --cur_;
				}
				break;
			default: break;
		}
		count_ = 0;
	}
	return result::none;
}

Element composer::render(const theme& th) const {
	const auto C = [&](rgb c) { return color(Color::RGB(c.r, c.g, c.b)); };
	const bool ins = mode_ == mode::insert;

	Element badge =
	    text(ins ? " INSERT " : " NORMAL ") | bold |
	    color(Color::RGB(th.p.base.r, th.p.base.g, th.p.base.b)) |
	    bgcolor(Color::RGB((ins ? th.p.foam : th.p.gold).r, (ins ? th.p.foam : th.p.gold).g,
	                       (ins ? th.p.foam : th.p.gold).b));

	const std::string before = buf_.substr(0, cur_);
	const std::string under = cur_ < buf_.size() ? buf_.substr(cur_, 1) : " ";
	const std::string after = cur_ < buf_.size() ? buf_.substr(cur_ + 1) : "";

	Element cursor = ins ? (text("▏") | C(th.p.iris) | blink)
	                     : (text(under) | color(Color::RGB(th.p.base.r, th.p.base.g, th.p.base.b)) |
	                        bgcolor(Color::RGB(th.p.iris.r, th.p.iris.g, th.p.iris.b)) | blink);

	Elements line = {badge, text(" › ") | C(th.p.iris) | bold, text(before) | C(th.p.text), cursor};
	if (ins) line.push_back(text(under == " " ? "" : under) | C(th.p.text));
	line.push_back(text(after) | C(th.p.text));
	if (buf_.empty() && ins)
		line.push_back(text(" type to talk, ctrl-w to weave") | C(th.p.muted) | dim);
	return hbox(std::move(line)) |
	       bgcolor(Color::RGB(th.p.surface.r, th.p.surface.g, th.p.surface.b));
}

}  // namespace plume
