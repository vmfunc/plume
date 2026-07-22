#include "plume/codec.hpp"

#include <nlohmann/json.hpp>

namespace plume::codec {

using json = nlohmann::json;

namespace {

json block_to_json(const content_block& b) {
	return std::visit(
	    [](const auto& v) -> json {
		    using T = std::decay_t<decltype(v)>;
		    if constexpr (std::is_same_v<T, text_block>) {
			    return {{"type", "text"}, {"text", v.text}};
		    } else if constexpr (std::is_same_v<T, thinking_block>) {
			    return {{"type", "thinking"}, {"thinking", v.thinking}, {"signature", v.signature}};
		    } else if constexpr (std::is_same_v<T, image_block>) {
			    return {{"type", "image"},
			            {"media_type", v.media_type},
			            {"data", v.data},
			            {"path", v.path}};
		    } else if constexpr (std::is_same_v<T, document_block>) {
			    return {{"type", "document"},
			            {"media_type", v.media_type},
			            {"data", v.data},
			            {"path", v.path}};
		    } else if constexpr (std::is_same_v<T, tool_use_block>) {
			    json input = json::parse(v.input_json, nullptr, false);
			    if (input.is_discarded()) input = v.input_json;  // keep as a string if not json
			    return {{"type", "tool_use"}, {"id", v.id}, {"name", v.name}, {"input", input}};
		    } else {  // tool_result_block
			    return {{"type", "tool_result"},
			            {"tool_use_id", v.tool_use_id},
			            {"content", v.content},
			            {"is_error", v.is_error}};
		    }
	    },
	    b);
}

std::string str_field(const json& o, const char* k) {
	if (auto it = o.find(k); it != o.end() && it->is_string()) return it->get<std::string>();
	return {};
}

}  // namespace

std::string encode_blocks(const std::vector<content_block>& blocks) {
	json arr = json::array();
	for (const auto& b : blocks) arr.push_back(block_to_json(b));
	return arr.dump();
}

result<std::vector<content_block>> decode_blocks(std::string_view text) {
	json arr = json::parse(text, nullptr, false);
	if (arr.is_discarded() || !arr.is_array())
		return fail(errc::parse, "content is not a json array");

	std::vector<content_block> out;
	out.reserve(arr.size());
	for (const auto& b : arr) {
		const std::string type = str_field(b, "type");
		if (type == "text") {
			out.emplace_back(text_block{str_field(b, "text")});
		} else if (type == "thinking") {
			out.emplace_back(thinking_block{str_field(b, "thinking"), str_field(b, "signature")});
		} else if (type == "image") {
			out.emplace_back(image_block{str_field(b, "media_type"), str_field(b, "data"),
			                             str_field(b, "path")});
		} else if (type == "document") {
			out.emplace_back(document_block{str_field(b, "media_type"), str_field(b, "data"),
			                                str_field(b, "path")});
		} else if (type == "tool_use") {
			std::string input;
			if (auto it = b.find("input"); it != b.end()) {
				input = it->is_string() ? it->get<std::string>() : it->dump();
			}
			out.emplace_back(
			    tool_use_block{str_field(b, "id"), str_field(b, "name"), std::move(input)});
		} else if (type == "tool_result") {
			bool is_err = false;
			if (auto it = b.find("is_error"); it != b.end() && it->is_boolean())
				is_err = it->get<bool>();
			out.emplace_back(
			    tool_result_block{str_field(b, "tool_use_id"), str_field(b, "content"), is_err});
		} else {
			return fail(errc::parse, "unknown content block type: " + type);
		}
	}
	return out;
}

std::string patch_str(std::string_view obj, std::string_view key, std::string_view value) {
	json o = json::parse(obj, nullptr, false);
	if (o.is_discarded() || !o.is_object()) o = json::object();
	o[std::string(key)] = std::string(value);
	return o.dump();
}

std::string patch_bool(std::string_view obj, std::string_view key, bool value) {
	json o = json::parse(obj, nullptr, false);
	if (o.is_discarded() || !o.is_object()) o = json::object();
	o[std::string(key)] = value;
	return o.dump();
}

std::optional<std::string> read_str(std::string_view obj, std::string_view key) {
	json o = json::parse(obj, nullptr, false);
	if (o.is_discarded() || !o.is_object()) return std::nullopt;
	auto it = o.find(std::string(key));
	if (it == o.end() || !it->is_string()) return std::nullopt;
	return it->get<std::string>();
}

std::optional<bool> read_bool(std::string_view obj, std::string_view key) {
	json o = json::parse(obj, nullptr, false);
	if (o.is_discarded() || !o.is_object()) return std::nullopt;
	auto it = o.find(std::string(key));
	if (it == o.end() || !it->is_boolean()) return std::nullopt;
	return it->get<bool>();
}

}  // namespace plume::codec
