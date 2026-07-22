#include <doctest/doctest.h>

#include <filesystem>

#include "plume/codec.hpp"
#include "plume/store.hpp"

using namespace plume;

namespace {
std::string tmp_db() {
	auto p = std::filesystem::temp_directory_path() / ("plume-test-" + new_id("db") + ".sqlite");
	return p.string();
}

node text_node(const convo_id& c, std::optional<node_id> parent, role r, const std::string& body,
               std::int64_t ts) {
	node n;
	n.id = node_id{new_id("node")};
	n.convo = c;
	n.parent = std::move(parent);
	n.role = r;
	n.content_json = codec::encode_blocks({text_block{body}});
	n.state = node_state::complete;
	n.created_at = ts;
	return n;
}
}  // namespace

TEST_CASE("store round-trips a conversation and its nodes") {
	const std::string path = tmp_db();
	{
		auto s = store::open(path);
		REQUIRE(s.has_value());

		conversation c;
		c.id = convo_id{new_id("convo")};
		c.title = "first light";
		c.created_at = 1;
		REQUIRE(s->put_conversation(c).has_value());

		auto u = text_node(c.id, std::nullopt, role::user, "what is a quill", 1);
		auto a = text_node(c.id, u.id, role::assistant, "a pen made from a feather", 2);
		REQUIRE(s->put_node(u).has_value());
		REQUIRE(s->put_node(a).has_value());
		REQUIRE(s->set_active_leaf(c.id, a.id).has_value());

		auto got = s->conversation_of(c.id);
		REQUIRE(got.has_value());
		CHECK(got->title == "first light");
		REQUIRE(got->active_leaf.has_value());
		CHECK(*got->active_leaf == a.id);

		auto kids = s->children_of(u.id);
		REQUIRE(kids.has_value());
		REQUIRE(kids->size() == 1);
		CHECK((*kids)[0].id == a.id);
	}
	std::filesystem::remove(path);
}

TEST_CASE("fts finds a node by prose, not json syntax") {
	const std::string path = tmp_db();
	{
		auto s = store::open(path);
		REQUIRE(s.has_value());
		conversation c;
		c.id = convo_id{new_id("convo")};
		REQUIRE(s->put_conversation(c).has_value());
		auto n =
		    text_node(c.id, std::nullopt, role::assistant, "weaving branches through the loom", 1);
		REQUIRE(s->put_node(n).has_value());

		auto hits = s->search("weav*");
		REQUIRE(hits.has_value());
		REQUIRE(hits->size() == 1);
		CHECK((*hits)[0].node == n.id);
		// the block-type token must not be indexed.
		auto none = s->search("tool_use OR content OR type");
		REQUIRE(none.has_value());
		CHECK(none->empty());
	}
	std::filesystem::remove(path);
}

TEST_CASE("tags and sync_map persist") {
	const std::string path = tmp_db();
	{
		auto s = store::open(path);
		REQUIRE(s.has_value());
		conversation c;
		c.id = convo_id{new_id("convo")};
		REQUIRE(s->put_conversation(c).has_value());

		REQUIRE(s->tag(c.id, "research").has_value());
		REQUIRE(s->tag(c.id, "wip").has_value());
		REQUIRE(s->tag(c.id, "wip").has_value());  // idempotent
		auto tags = s->tags_of(c.id);
		REQUIRE(tags.has_value());
		CHECK(tags->size() == 2);

		REQUIRE(s->put_sync({"claude-export", "remote-1", c.id.str(), "etag-a"}).has_value());
		REQUIRE(s->put_sync({"claude-export", "remote-1", c.id.str(), "etag-b"}).has_value());
		auto row = s->sync_for("claude-export", "remote-1");
		REQUIRE(row.has_value());
		REQUIRE(row->has_value());
		CHECK((*row)->etag == "etag-b");  // upserted, not duplicated

		CHECK(s->check_integrity().value_or(false));
	}
	std::filesystem::remove(path);
}
