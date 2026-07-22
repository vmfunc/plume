// claude.ai link. the export backend reads the official data export and folds it
// into plume's schema; it is idempotent — node ids are derived from the export's
// message uuids and conversations are keyed through sync_map, so re-importing
// updates in place rather than duplicating. the live backend is experimental
// scaffolding: it constructs, but two-way traffic is not wired up in this build.
#include "plume/sync.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "plume/codec.hpp"
#include "plume/ids.hpp"

namespace plume {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// claude.ai timestamps are iso-8601 strings; we only need a monotone-ish order,
// so fall back to a counter when parsing is not worth it.
std::int64_t seq_from(std::int64_t& counter) {
	return ++counter;
}

class export_backend final : public sync_backend {
   public:
	explicit export_backend(std::string dir) : dir_(std::move(dir)) {}

	std::string_view name() const override { return "claude-export"; }

	result<import_stats> import_into(store& s) override {
		const fs::path file = fs::path(dir_) / "conversations.json";
		if (!fs::exists(file)) return fail(errc::io, "no conversations.json in " + dir_);

		std::ifstream in(file, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		json convos = json::parse(ss.str(), nullptr, false);
		if (convos.is_discarded() || !convos.is_array())
			return fail(errc::parse, "conversations.json is not a json array");

		import_stats stats;
		std::int64_t counter = 0;

		for (const auto& c : convos) {
			const std::string remote = c.value("uuid", c.value("id", std::string{}));
			if (remote.empty()) continue;
			++stats.conversations;

			auto prior = s.sync_for("claude-export", remote);
			const bool existed = prior && prior->has_value();
			if (existed)
				++stats.updated;
			else
				++stats.created;

			convo_id cid{existed ? (*prior)->local_id : new_id("convo")};
			conversation conv;
			conv.id = cid;
			conv.title = c.value("name", std::string{"imported"});
			conv.source = "claude-export";
			conv.created_at = seq_from(counter);
			if (auto r = s.put_conversation(conv); !r) return std::unexpected(r.error());

			std::optional<node_id> parent;
			node_id last;
			for (const auto& m : c.value("chat_messages", json::array())) {
				const std::string muuid = m.value("uuid", std::string{});
				node n;
				n.id = node_id{muuid.empty() ? new_id("node") : ("node_" + muuid)};
				n.convo = cid;
				n.parent = parent;
				const std::string sender = m.value("sender", std::string{"human"});
				n.role = sender == "assistant" ? role::assistant : role::user;
				n.content_json = codec::encode_blocks({text_block{m.value("text", std::string{})}});
				n.created_at = seq_from(counter);
				if (auto r = s.put_node(n); !r) return std::unexpected(r.error());
				parent = n.id;
				last = n.id;
				++stats.nodes;
			}
			if (!last.empty())
				if (auto r = s.set_active_leaf(cid, last); !r) return std::unexpected(r.error());

			sync_row row{"claude-export", remote, cid.str(), c.value("updated_at", std::string{})};
			if (auto r = s.put_sync(row); !r) return std::unexpected(r.error());
		}
		return stats;
	}

   private:
	std::string dir_;
};

// experimental: constructs so the config path is exercised, but import is not
// wired to the live endpoints in this build (see docs/claude-ai-link.md).
class live_backend final : public sync_backend {
   public:
	live_backend(std::string cookie, bool two_way)
	    : cookie_(std::move(cookie)), two_way_(two_way) {}

	std::string_view name() const override { return "claude-live"; }

	result<import_stats> import_into(store&) override {
		return fail(errc::unsupported,
		            "live claude.ai sync is experimental and not enabled in this build");
	}

   private:
	[[maybe_unused]] std::string cookie_;
	[[maybe_unused]] bool two_way_;
};

}  // namespace

result<std::unique_ptr<sync_backend>> open_export(const std::string& export_dir) {
	if (!fs::exists(export_dir)) return fail(errc::not_found, "no such export dir: " + export_dir);
	return std::make_unique<export_backend>(export_dir);
}

result<std::unique_ptr<sync_backend>> open_live(const std::string& session_cookie, bool two_way) {
	if (session_cookie.empty()) return fail(errc::auth, "live sync needs a session cookie");
	return std::make_unique<live_backend>(session_cookie, two_way);
}

}  // namespace plume
