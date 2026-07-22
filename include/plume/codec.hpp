// json (de)serialization for the domain types that get stored as text columns.
// keeping it here means store.hpp, weave.hpp and the ui never include the json
// library; they trade in std::string and the plain structs.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plume/error.hpp"
#include "plume/message.hpp"

namespace plume::codec {

// node content column <-> the block list.
[[nodiscard]] std::string encode_blocks(const std::vector<content_block>&);
[[nodiscard]] result<std::vector<content_block>> decode_blocks(std::string_view json);

// params_json is a free-form object holding the sampling snapshot plus weave
// metadata (label, bookmark). these patch or read one field, leaving the rest
// untouched, so the weave engine and the composer can share the column.
[[nodiscard]] std::string patch_str(std::string_view obj, std::string_view key,
                                    std::string_view value);
[[nodiscard]] std::string patch_bool(std::string_view obj, std::string_view key, bool value);
[[nodiscard]] std::optional<std::string> read_str(std::string_view obj, std::string_view key);
[[nodiscard]] std::optional<bool> read_bool(std::string_view obj, std::string_view key);

}  // namespace plume::codec
