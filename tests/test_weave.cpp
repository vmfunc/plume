#include <doctest/doctest.h>

#include <filesystem>

#include "plume/codec.hpp"
#include "plume/store.hpp"
#include "plume/weave.hpp"

using namespace plume;

namespace {
std::string tmp_db() {
	return (std::filesystem::temp_directory_path() / ("plume-weave-" + new_id("db") + ".sqlite"))
	    .string();
}

// a small fixture: a store with one conversation and a helper to append nodes.
struct fixture {
	std::string path = tmp_db();
	store s = store::open(path).value();
	convo_id convo{new_id("convo")};
	std::int64_t clock = 0;

	fixture() {
		conversation c;
		c.id = convo;
		c.created_at = ++clock;
		s.put_conversation(c).value();
	}
	~fixture() { std::filesystem::remove(path); }

	node_id add(std::optional<node_id> parent, role r, const std::string& body) {
		node n;
		n.id = node_id{new_id("node")};
		n.convo = convo;
		n.parent = std::move(parent);
		n.role = r;
		n.content_json = codec::encode_blocks({text_block{body}});
		n.created_at = ++clock;
		s.put_node(n).value();
		return n.id;
	}
};

// the load-bearing invariant: a resolved active path is a real chain from the
// root down to the active leaf, each node the parent of the next.
bool is_root_to_leaf(const std::vector<node>& path, const node_id& leaf) {
	if (path.empty()) return false;
	if (path.front().parent.has_value()) return false;  // starts at the root
	if (path.back().id != leaf) return false;           // ends at the leaf
	for (std::size_t i = 1; i < path.size(); ++i)
		if (!path[i].parent || *path[i].parent != path[i - 1].id) return false;
	return true;
}
}  // namespace

TEST_CASE("active path is always a root-to-leaf chain") {
	fixture f;
	weave w(f.s);
	auto root = f.add(std::nullopt, role::user, "root");
	auto a = f.add(root, role::assistant, "a");
	auto b = f.add(a, role::user, "b");
	auto leaf = f.add(b, role::assistant, "leaf");
	f.s.set_active_leaf(f.convo, leaf).value();

	auto path = w.active_path(f.convo);
	REQUIRE(path.has_value());
	CHECK(is_root_to_leaf(*path, leaf));
	CHECK(path->size() == 4);
}

TEST_CASE("adopt makes a branch the active path and holds the invariant") {
	fixture f;
	weave w(f.s);
	auto root = f.add(std::nullopt, role::user, "root");
	auto a1 = f.add(root, role::assistant, "sibling one");
	auto a2 = f.add(root, role::assistant, "sibling two");
	auto a2child = f.add(a2, role::user, "follow-up under two");

	// adopt the second sibling: the active leaf should descend to its newest leaf.
	REQUIRE(w.adopt(f.convo, a2).has_value());
	auto path = w.active_path(f.convo);
	REQUIRE(path.has_value());
	CHECK(is_root_to_leaf(*path, a2child));
	// the first sibling is not on the active path.
	for (const auto& n : *path) CHECK(n.id != a1);
}

TEST_CASE("prune is always restorable and clears the graveyard from search") {
	fixture f;
	weave w(f.s);
	auto root = f.add(std::nullopt, role::user, "root");
	auto keep = f.add(root, role::assistant, "kept branch");
	auto doomed = f.add(root, role::assistant, "doomed branch with mango");
	auto doomed_child = f.add(doomed, role::user, "child of doomed");
	f.s.set_active_leaf(f.convo, doomed_child).value();

	REQUIRE(w.prune(doomed).has_value());

	// the pruned subtree is gone from the default view but present with pruned.
	auto live = w.view(f.convo, false);
	REQUIRE(live.has_value());
	CHECK_FALSE(live->nodes.contains(doomed));
	CHECK_FALSE(live->nodes.contains(doomed_child));
	CHECK(live->nodes.contains(keep));

	// search no longer surfaces pruned content.
	CHECK(f.s.search("mango").value().empty());

	// the active path retreated out of the graveyard to a valid chain.
	auto path = w.active_path(f.convo);
	REQUIRE(path.has_value());
	for (const auto& n : *path) {
		CHECK(n.id != doomed);
		CHECK(n.id != doomed_child);
	}

	// restore brings it all back, content and search alike.
	REQUIRE(w.restore(doomed).has_value());
	auto back = w.view(f.convo, false);
	REQUIRE(back.has_value());
	CHECK(back->nodes.contains(doomed));
	CHECK(back->nodes.contains(doomed_child));
	CHECK(f.s.search("mango").value().size() == 1);
}

TEST_CASE("graft moves a subtree and refuses to make a cycle") {
	fixture f;
	weave w(f.s);
	auto root = f.add(std::nullopt, role::user, "root");
	auto a = f.add(root, role::assistant, "a");
	auto b = f.add(a, role::user, "b");
	auto c = f.add(root, role::assistant, "c");

	// valid: move b under c.
	REQUIRE(w.graft(b, c).has_value());
	auto kids = f.s.children_of(c);
	REQUIRE(kids.has_value());
	bool found = false;
	for (const auto& k : *kids)
		if (k.id == b) found = true;
	CHECK(found);

	// cycle: b now sits under c, so c is b's ancestor; grafting c under b loops.
	auto err = w.graft(c, b);
	REQUIRE_FALSE(err.has_value());
	CHECK(err.error().code == errc::unsupported);

	// onto itself is refused too.
	CHECK_FALSE(w.graft(a, a).has_value());
}

TEST_CASE("dot and json export the live tree") {
	fixture f;
	weave w(f.s);
	auto root = f.add(std::nullopt, role::user, "root");
	f.add(root, role::assistant, "leaf");

	auto dot = w.to_dot(f.convo);
	REQUIRE(dot.has_value());
	CHECK(dot->find("digraph weave") != std::string::npos);
	CHECK(dot->find("->") != std::string::npos);

	auto js = w.to_json(f.convo);
	REQUIRE(js.has_value());
	CHECK(js->find("\"nodes\"") != std::string::npos);
}
