// the provider-neutral content model. a message is a role plus an ordered list
// of blocks; every provider maps its wire shape onto these. tool input and
// params stay as serialized json strings so this header pulls in no json
// dependency — the provider owns that parsing.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace plume {

enum class role : std::uint8_t { system, user, assistant };

[[nodiscard]] std::string_view to_string(role) noexcept;
[[nodiscard]] std::optional<role> role_from(std::string_view) noexcept;

struct text_block {
	std::string text;
};

// extended thinking. signature is the opaque token the provider hands back; it
// must be replayed verbatim on the same model or the turn is rejected.
struct thinking_block {
	std::string thinking;
	std::string signature;
};

// base64 payload; media_type like "image/png". path, when set, is the on-disk
// origin kept for previews and re-export — the wire only ever sees data.
struct image_block {
	std::string media_type;
	std::string data;
	std::string path;
};

struct document_block {
	std::string media_type;  // "application/pdf"
	std::string data;
	std::string path;
};

struct tool_use_block {
	std::string id;
	std::string name;
	std::string input_json;
};

struct tool_result_block {
	std::string tool_use_id;
	std::string content;
	bool is_error = false;
};

using content_block = std::variant<text_block, thinking_block, image_block, document_block,
                                   tool_use_block, tool_result_block>;

struct message {
	plume::role role = role::user;
	std::vector<content_block> blocks;

	// the common case: a message that is just prose.
	static message text(plume::role r, std::string body) {
		return {r, {text_block{std::move(body)}}};
	}

	[[nodiscard]] std::string plain_text() const;  // concatenated text blocks
};

// token accounting for one turn, surfaced in the nerd-stats line.
struct usage {
	std::int64_t input = 0;
	std::int64_t output = 0;
	std::int64_t cache_creation = 0;
	std::int64_t cache_read = 0;
	double cost = 0.0;  // usd, against the local price table; never authoritative
};

}  // namespace plume
