// the loom. a conversation is a tree of nodes in the store; there is no separate
// branching feature bolted on. this engine reads and mutates that tree while
// holding two invariants: the active path is always a real root-to-leaf chain,
// and a prune is always restorable (it moves a subtree to the graveyard state,
// never deletes it). graft refuses to form a cycle.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "plume/error.hpp"
#include "plume/ids.hpp"
#include "plume/store.hpp"

namespace plume {

struct tree_node {
	node data;
	std::vector<node_id> children;  // in creation order
	int depth = 0;
};

struct tree_view {
	std::optional<node_id> root;
	std::unordered_map<node_id, tree_node> nodes;
	std::vector<node_id> preorder;  // display order, siblings by created_at
};

class weave {
   public:
	explicit weave(store& s) : store_(s) {}

	// the linear view: root down to the conversation's active leaf.
	[[nodiscard]] result<std::vector<node>> active_path(const convo_id&);

	// the whole tree for the weave pane. pruned nodes are omitted unless asked.
	[[nodiscard]] result<tree_view> view(const convo_id&, bool include_pruned = false);

	// make this node's deepest descendant the active leaf, so the linear view
	// follows through it. with no descendants the node itself becomes the leaf.
	[[nodiscard]] result<void> adopt(const convo_id&, const node_id&);

	// reparent a subtree. refuses if new_parent is the subtree root or lies
	// inside it (that would make a cycle).
	[[nodiscard]] result<void> graft(const node_id& subtree_root, const node_id& new_parent);

	// soft-delete a subtree to the graveyard, and bring it back.
	[[nodiscard]] result<void> prune(const node_id& subtree_root);
	[[nodiscard]] result<void> restore(const node_id& subtree_root);

	// weave-view metadata, persisted in the node's params_json.
	[[nodiscard]] result<void> set_label(const node_id&, std::string);
	[[nodiscard]] result<void> set_bookmark(const node_id&, bool);
	[[nodiscard]] result<std::string> label_of(const node_id&);
	[[nodiscard]] result<bool> bookmarked(const node_id&);

	// export the live tree.
	[[nodiscard]] result<std::string> to_dot(const convo_id&);
	[[nodiscard]] result<std::string> to_json(const convo_id&);

   private:
	store& store_;
};

}  // namespace plume
