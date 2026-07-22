// sqlite persistence in wal mode. every conversation is a tree of nodes; the
// linear chat view is just the root-to-active-leaf path. nothing is destroyed —
// prune moves a subtree to the graveyard state, compaction summarizes but keeps
// the originals. the schema below is frozen; the weave engine and sync layer
// build on it.
//
//   conversations(id, title, project, source, created_at, active_leaf)
//   nodes(id, convo_id, parent_id, role, content, model, params,
//         tokens_in, tokens_out, cost, state, created_at)
//   attachments(id, node_id, kind, media_type, path, bytes)
//   tags(name), convo_tags(convo_id, tag)
//   sync_map(backend, remote_id, local_id, etag)
//   nodes_fts  -- fts5 over node content
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plume/error.hpp"
#include "plume/ids.hpp"
#include "plume/message.hpp"

struct sqlite3;

namespace plume {

enum class node_state : std::uint8_t {
	streaming,  // a reply still arriving
	complete,   // settled
	error,      // the turn failed; detail lives in content
	pruned,     // soft-deleted to the graveyard, restorable
};

[[nodiscard]] std::string_view to_string(node_state) noexcept;
[[nodiscard]] std::optional<node_state> node_state_from(std::string_view) noexcept;

struct node {
	node_id id;
	convo_id convo;
	std::optional<node_id> parent;  // null only for the root
	plume::role role = role::user;
	std::string content_json;  // serialized vector<content_block>
	std::string model;         // the model that produced an assistant node
	std::string params_json;   // sampling params snapshot
	std::int64_t tokens_in = 0;
	std::int64_t tokens_out = 0;
	double cost = 0.0;
	node_state state = node_state::complete;
	std::int64_t created_at = 0;  // unix millis

	// bookmark + label are weave-view metadata; stored in params_json to keep
	// the column set frozen. the weave engine reads/writes them there.
};

struct conversation {
	convo_id id;
	std::string title;
	std::string project;
	std::string source = "local";  // local, claude-export, claude-live
	std::int64_t created_at = 0;
	std::optional<node_id> active_leaf;
};

struct attachment {
	attachment_id id;
	node_id node;
	std::string kind;  // image, document
	std::string media_type;
	std::string path;  // on-disk origin
	std::int64_t bytes = 0;
};

struct search_hit {
	convo_id convo;
	node_id node;
	std::string snippet;  // fts5 snippet with the match highlighted
};

struct sync_row {
	std::string backend;
	std::string remote_id;
	std::string local_id;
	std::string etag;
};

class store {
   public:
	// open (creating if absent) and migrate to the current schema. wal mode.
	[[nodiscard]] static result<store> open(const std::string& path);

	store(store&&) noexcept = default;
	store& operator=(store&&) noexcept = default;
	store(const store&) = delete;
	store& operator=(const store&) = delete;
	~store() = default;

	// conversations
	[[nodiscard]] result<void> put_conversation(const conversation&);
	[[nodiscard]] result<conversation> conversation_of(const convo_id&);
	[[nodiscard]] result<std::vector<conversation>> conversations();
	[[nodiscard]] result<void> set_active_leaf(const convo_id&, const node_id&);
	[[nodiscard]] result<void> delete_conversation(const convo_id&);

	// nodes
	[[nodiscard]] result<void> put_node(const node&);  // insert or replace
	[[nodiscard]] result<node> node_of(const node_id&);
	[[nodiscard]] result<std::vector<node>> nodes_of(const convo_id&);
	[[nodiscard]] result<std::vector<node>> children_of(const node_id&);
	[[nodiscard]] result<void> set_state(const node_id&, node_state);
	[[nodiscard]] result<void> set_parent(const node_id&, const std::optional<node_id>&);

	// attachments
	[[nodiscard]] result<void> put_attachment(const attachment&);
	[[nodiscard]] result<std::vector<attachment>> attachments_of(const node_id&);

	// tags
	[[nodiscard]] result<void> tag(const convo_id&, std::string_view);
	[[nodiscard]] result<void> untag(const convo_id&, std::string_view);
	[[nodiscard]] result<std::vector<std::string>> tags_of(const convo_id&);

	// search over the fts index
	[[nodiscard]] result<std::vector<search_hit>> search(std::string_view query, int limit = 50);
	[[nodiscard]] result<std::vector<search_hit>> search_in(const convo_id&, std::string_view,
	                                                        int limit = 50);

	// sync bookkeeping
	[[nodiscard]] result<void> put_sync(const sync_row&);
	[[nodiscard]] result<std::optional<sync_row>> sync_for(std::string_view backend,
	                                                       std::string_view remote_id);

	// integrity, for plume doctor
	[[nodiscard]] result<bool> check_integrity();

   private:
	struct closer {
		void operator()(sqlite3*) const noexcept;
	};
	explicit store(std::unique_ptr<sqlite3, closer> db) : db_(std::move(db)) {}

	std::unique_ptr<sqlite3, closer> db_;
};

}  // namespace plume
