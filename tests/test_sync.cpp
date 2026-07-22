#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "plume/ids.hpp"
#include "plume/store.hpp"
#include "plume/sync.hpp"

using namespace plume;
namespace fs = std::filesystem;

namespace {
fs::path make_export() {
	fs::path dir = fs::temp_directory_path() / ("plume-export-" + new_id("t"));
	fs::create_directories(dir);
	std::ofstream(dir / "conversations.json") << R"([
	  {
	    "uuid": "conv-1",
	    "name": "a chat about quills",
	    "chat_messages": [
	      {"uuid": "m1", "sender": "human",     "text": "what is a quill"},
	      {"uuid": "m2", "sender": "assistant", "text": "a pen cut from a feather"}
	    ]
	  }
	])";
	return dir;
}
}  // namespace

TEST_CASE("claude.ai export imports and re-import updates rather than duplicates") {
	fs::path exp = make_export();
	std::string db =
	    (fs::temp_directory_path() / ("plume-syncdb-" + new_id("t") + ".sqlite")).string();
	{
		auto s = store::open(db);
		REQUIRE(s.has_value());

		auto backend = open_export(exp.string());
		REQUIRE(backend.has_value());

		auto first = (*backend)->import_into(*s);
		REQUIRE(first.has_value());
		CHECK(first->conversations == 1);
		CHECK(first->nodes == 2);
		CHECK(first->created == 1);
		CHECK(first->updated == 0);

		auto convos = s->conversations();
		REQUIRE(convos.has_value());
		CHECK(convos->size() == 1);
		CHECK(convos->front().source == "claude-export");

		// re-import: same rows, marked updated, still exactly one conversation.
		auto again = (*backend)->import_into(*s);
		REQUIRE(again.has_value());
		CHECK(again->updated == 1);
		CHECK(again->created == 0);
		CHECK(s->conversations().value().size() == 1);

		// the imported content is searchable.
		CHECK(s->search("feather").value().size() == 1);
	}
	fs::remove(db);
	fs::remove_all(exp);
}
