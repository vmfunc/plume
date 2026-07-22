#include "plume/weave.hpp"

#include <algorithm>
#include <functional>

#include "plume/codec.hpp"

namespace plume {

namespace {

struct loaded {
	std::unordered_map<node_id, node> by_id;
	std::unordered_map<node_id, std::vector<node_id>> children;  // created order
	std::optional<node_id> root;
};

// build the whole tree for a conversation, pruned nodes included. nodes_of
// returns rows in created order, so each children list is already ordered.
result<loaded> load_tree(store& s, const convo_id& c) {
	auto ns = s.nodes_of(c);
	if (!ns) return std::unexpected(ns.error());
	loaded t;
	for (auto& n : *ns) {
		const node_id id = n.id;
		if (!n.parent) {
			if (!t.root) t.root = id;
		} else {
			t.children[*n.parent].push_back(id);
		}
		t.by_id.emplace(id, std::move(n));
	}
	return t;
}

// every node in the subtree rooted at `root`, root included.
std::vector<node_id> subtree(const loaded& t, const node_id& root) {
	std::vector<node_id> out;
	std::function<void(const node_id&)> walk = [&](const node_id& id) {
		out.push_back(id);
		if (auto it = t.children.find(id); it != t.children.end())
			for (const auto& child : it->second) walk(child);
	};
	walk(root);
	return out;
}

}  // namespace

result<std::vector<node>> weave::active_path(const convo_id& c) {
	auto conv = store_.conversation_of(c);
	if (!conv) return std::unexpected(conv.error());
	std::vector<node> path;
	if (!conv->active_leaf) return path;

	std::optional<node_id> cur = conv->active_leaf;
	while (cur) {
		auto n = store_.node_of(*cur);
		if (!n) return std::unexpected(n.error());
		path.push_back(*n);
		cur = n->parent;
	}
	std::reverse(path.begin(), path.end());
	return path;
}

result<tree_view> weave::view(const convo_id& c, bool include_pruned) {
	auto t = load_tree(store_, c);
	if (!t) return std::unexpected(t.error());

	tree_view v;
	v.root = t->root;

	std::function<void(const node_id&, int)> walk = [&](const node_id& id, int depth) {
		auto it = t->by_id.find(id);
		if (it == t->by_id.end()) return;
		if (!include_pruned && it->second.state == node_state::pruned) return;

		tree_node tn;
		tn.data = it->second;
		tn.depth = depth;
		if (auto ch = t->children.find(id); ch != t->children.end()) {
			for (const auto& kid : ch->second) {
				auto kit = t->by_id.find(kid);
				if (!include_pruned && kit != t->by_id.end() &&
				    kit->second.state == node_state::pruned)
					continue;
				tn.children.push_back(kid);
			}
		}
		v.nodes.emplace(id, std::move(tn));
		v.preorder.push_back(id);
		if (auto ch = t->children.find(id); ch != t->children.end())
			for (const auto& kid : ch->second) walk(kid, depth + 1);
	};
	if (t->root) walk(*t->root, 0);
	return v;
}

result<void> weave::adopt(const convo_id& c, const node_id& target) {
	auto t = load_tree(store_, c);
	if (!t) return std::unexpected(t.error());
	if (!t->by_id.contains(target)) return fail(errc::not_found, "adopt: no such node");

	// walk down the newest non-pruned child until we hit a leaf.
	node_id leaf = target;
	while (true) {
		auto it = t->children.find(leaf);
		if (it == t->children.end()) break;
		node_id next;
		bool found = false;
		for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
			auto n = t->by_id.find(*rit);
			if (n != t->by_id.end() && n->second.state != node_state::pruned) {
				next = *rit;
				found = true;
				break;
			}
		}
		if (!found) break;
		leaf = next;
	}
	return store_.set_active_leaf(c, leaf);
}

result<void> weave::graft(const node_id& subtree_root, const node_id& new_parent) {
	auto sub = store_.node_of(subtree_root);
	if (!sub) return std::unexpected(sub.error());
	auto par = store_.node_of(new_parent);
	if (!par) return std::unexpected(par.error());
	if (sub->convo != par->convo)
		return fail(errc::unsupported, "graft across conversations is not supported");
	if (subtree_root == new_parent) return fail(errc::unsupported, "graft onto itself");

	auto t = load_tree(store_, sub->convo);
	if (!t) return std::unexpected(t.error());
	for (const auto& id : subtree(*t, subtree_root))
		if (id == new_parent) return fail(errc::unsupported, "graft would form a cycle");

	return store_.set_parent(subtree_root, new_parent);
}

result<void> weave::prune(const node_id& subtree_root) {
	auto root = store_.node_of(subtree_root);
	if (!root) return std::unexpected(root.error());
	auto t = load_tree(store_, root->convo);
	if (!t) return std::unexpected(t.error());

	const auto ids = subtree(*t, subtree_root);
	for (const auto& id : ids) {
		node n = t->by_id[id];
		n.state = node_state::pruned;
		if (auto r = store_.put_node(n); !r) return r;  // put_node drops the fts row for pruned
	}

	// keep the active path valid: if it ran through the graveyard, retreat to
	// the pruned subtree's parent.
	auto conv = store_.conversation_of(root->convo);
	if (conv && conv->active_leaf) {
		const bool in_grave = std::find(ids.begin(), ids.end(), *conv->active_leaf) != ids.end();
		if (in_grave && root->parent) {
			if (auto r = store_.set_active_leaf(root->convo, *root->parent); !r) return r;
		}
	}
	return {};
}

result<void> weave::restore(const node_id& subtree_root) {
	auto root = store_.node_of(subtree_root);
	if (!root) return std::unexpected(root.error());
	auto t = load_tree(store_, root->convo);
	if (!t) return std::unexpected(t.error());
	for (const auto& id : subtree(*t, subtree_root)) {
		node n = t->by_id[id];
		n.state = node_state::complete;
		if (auto r = store_.put_node(n); !r) return r;  // reindexes the fts row
	}
	return {};
}

result<void> weave::set_label(const node_id& id, std::string label) {
	auto n = store_.node_of(id);
	if (!n) return std::unexpected(n.error());
	n->params_json = codec::patch_str(n->params_json, "label", label);
	return store_.put_node(*n);
}

result<void> weave::set_bookmark(const node_id& id, bool on) {
	auto n = store_.node_of(id);
	if (!n) return std::unexpected(n.error());
	n->params_json = codec::patch_bool(n->params_json, "bookmark", on);
	return store_.put_node(*n);
}

result<std::string> weave::label_of(const node_id& id) {
	auto n = store_.node_of(id);
	if (!n) return std::unexpected(n.error());
	return codec::read_str(n->params_json, "label").value_or(std::string{});
}

result<bool> weave::bookmarked(const node_id& id) {
	auto n = store_.node_of(id);
	if (!n) return std::unexpected(n.error());
	return codec::read_bool(n->params_json, "bookmark").value_or(false);
}

result<std::string> weave::to_dot(const convo_id& c) {
	auto v = view(c, false);
	if (!v) return std::unexpected(v.error());
	std::string out = "digraph weave {\n  rankdir=LR;\n  node [shape=box, style=rounded];\n";
	for (const auto& id : v->preorder) {
		const auto& tn = v->nodes.at(id);
		std::string label = std::string(to_string(tn.data.role));
		if (!tn.data.model.empty()) label += "\\n" + tn.data.model;
		out += "  \"" + id.str() + "\" [label=\"" + label + "\"];\n";
		for (const auto& kid : tn.children)
			out += "  \"" + id.str() + "\" -> \"" + kid.str() + "\";\n";
	}
	out += "}\n";
	return out;
}

result<std::string> weave::to_json(const convo_id& c) {
	auto v = view(c, false);
	if (!v) return std::unexpected(v.error());
	auto conv = store_.conversation_of(c);

	std::string out = "{\n  \"conversation\": \"" + c.str() + "\",\n";
	if (conv && conv->active_leaf)
		out += "  \"active_leaf\": \"" + conv->active_leaf->str() + "\",\n";
	out += "  \"nodes\": [\n";
	bool first = true;
	for (const auto& id : v->preorder) {
		const auto& tn = v->nodes.at(id);
		if (!first) out += ",\n";
		first = false;
		out += "    {\"id\": \"" + id.str() + "\", \"role\": \"" +
		       std::string(to_string(tn.data.role)) + "\"";
		if (tn.data.parent) out += ", \"parent\": \"" + tn.data.parent->str() + "\"";
		if (!tn.data.model.empty()) out += ", \"model\": \"" + tn.data.model + "\"";
		out += "}";
	}
	out += "\n  ]\n}\n";
	return out;
}

}  // namespace plume
