#include "plume/message.hpp"

namespace plume {

std::string_view to_string(role r) noexcept {
	switch (r) {
		case role::system: return "system";
		case role::user: return "user";
		case role::assistant: return "assistant";
	}
	return "user";
}

std::optional<role> role_from(std::string_view s) noexcept {
	if (s == "system") return role::system;
	if (s == "user") return role::user;
	if (s == "assistant") return role::assistant;
	return std::nullopt;
}

std::string message::plain_text() const {
	std::string out;
	for (const auto& b : blocks) {
		if (const auto* t = std::get_if<text_block>(&b)) {
			if (!out.empty()) out.push_back('\n');
			out += t->text;
		}
	}
	return out;
}

}  // namespace plume
