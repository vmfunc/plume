#include "plume/store.hpp"

#include <sqlite3.h>

#include <utility>

#include "plume/codec.hpp"

namespace plume {

void store::closer::operator()(sqlite3* db) const noexcept {
	if (db) sqlite3_close_v2(db);
}

std::string_view to_string(node_state s) noexcept {
	switch (s) {
		case node_state::streaming: return "streaming";
		case node_state::complete: return "complete";
		case node_state::error: return "error";
		case node_state::pruned: return "pruned";
	}
	return "complete";
}

std::optional<node_state> node_state_from(std::string_view s) noexcept {
	if (s == "streaming") return node_state::streaming;
	if (s == "complete") return node_state::complete;
	if (s == "error") return node_state::error;
	if (s == "pruned") return node_state::pruned;
	return std::nullopt;
}

namespace {

// RAII around a prepared statement so an early return never leaks it.
class stmt {
   public:
	stmt() = default;
	~stmt() {
		if (s_) sqlite3_finalize(s_);
	}
	stmt(const stmt&) = delete;
	stmt& operator=(const stmt&) = delete;
	stmt(stmt&& o) noexcept : s_(std::exchange(o.s_, nullptr)) {}
	stmt& operator=(stmt&& o) noexcept {
		if (this != &o) {
			if (s_) sqlite3_finalize(s_);
			s_ = std::exchange(o.s_, nullptr);
		}
		return *this;
	}

	int prepare(sqlite3* db, const char* sql) { return sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
	sqlite3_stmt* raw() const { return s_; }

	void text(int i, std::string_view v) {
		sqlite3_bind_text(s_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
	}
	void integer(int i, sqlite3_int64 v) { sqlite3_bind_int64(s_, i, v); }
	void real(int i, double v) { sqlite3_bind_double(s_, i, v); }
	void null(int i) { sqlite3_bind_null(s_, i); }
	int step() { return sqlite3_step(s_); }

	std::string col_text(int i) {
		const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s_, i));
		const int n = sqlite3_column_bytes(s_, i);
		return p ? std::string(p, static_cast<std::size_t>(n)) : std::string{};
	}
	sqlite3_int64 col_int(int i) { return sqlite3_column_int64(s_, i); }
	double col_real(int i) { return sqlite3_column_double(s_, i); }
	bool col_null(int i) { return sqlite3_column_type(s_, i) == SQLITE_NULL; }

   private:
	sqlite3_stmt* s_ = nullptr;
};

error sqlite_err(sqlite3* db, std::string_view where) {
	return error{errc::sqlite, std::string(where) + ": " + sqlite3_errmsg(db)};
}

// the text an fts row should index: prose and thinking, never the json syntax.
std::string index_text(const std::string& content_json) {
	auto blocks = codec::decode_blocks(content_json);
	if (!blocks) return content_json;  // best effort if the column isn't block json
	std::string out;
	for (const auto& b : *blocks) {
		if (const auto* t = std::get_if<text_block>(&b)) out += t->text + '\n';
		else if (const auto* th = std::get_if<thinking_block>(&b)) out += th->thinking + '\n';
	}
	return out;
}

node read_node(stmt& q) {
	node n;
	n.id = node_id{q.col_text(0)};
	n.convo = convo_id{q.col_text(1)};
	if (!q.col_null(2)) n.parent = node_id{q.col_text(2)};
	n.role = role_from(q.col_text(3)).value_or(role::user);
	n.content_json = q.col_text(4);
	n.model = q.col_text(5);
	n.params_json = q.col_text(6);
	n.tokens_in = q.col_int(7);
	n.tokens_out = q.col_int(8);
	n.cost = q.col_real(9);
	n.state = node_state_from(q.col_text(10)).value_or(node_state::complete);
	n.created_at = q.col_int(11);
	return n;
}

constexpr const char* kNodeCols =
    "id, convo_id, parent_id, role, content, model, params, tokens_in, tokens_out, cost, state, "
    "created_at";

}  // namespace

result<store> store::open(const std::string& path) {
	sqlite3* raw = nullptr;
	const int rc = sqlite3_open_v2(path.c_str(), &raw,
	                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
	std::unique_ptr<sqlite3, closer> db(raw);
	if (rc != SQLITE_OK) return fail(errc::sqlite, raw ? sqlite3_errmsg(raw) : "cannot open database");

	static constexpr const char* schema = R"sql(
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 3000;

CREATE TABLE IF NOT EXISTS conversations(
  id TEXT PRIMARY KEY, title TEXT, project TEXT, source TEXT,
  created_at INTEGER, active_leaf TEXT);

CREATE TABLE IF NOT EXISTS nodes(
  id TEXT PRIMARY KEY, convo_id TEXT NOT NULL, parent_id TEXT,
  role TEXT, content TEXT, model TEXT, params TEXT,
  tokens_in INTEGER, tokens_out INTEGER, cost REAL,
  state TEXT, created_at INTEGER);
CREATE INDEX IF NOT EXISTS idx_nodes_convo ON nodes(convo_id);
CREATE INDEX IF NOT EXISTS idx_nodes_parent ON nodes(parent_id);

CREATE TABLE IF NOT EXISTS attachments(
  id TEXT PRIMARY KEY, node_id TEXT, kind TEXT, media_type TEXT, path TEXT, bytes INTEGER);
CREATE INDEX IF NOT EXISTS idx_attach_node ON attachments(node_id);

CREATE TABLE IF NOT EXISTS tags(name TEXT PRIMARY KEY);
CREATE TABLE IF NOT EXISTS convo_tags(convo_id TEXT, tag TEXT, PRIMARY KEY(convo_id, tag));

CREATE TABLE IF NOT EXISTS sync_map(
  backend TEXT, remote_id TEXT, local_id TEXT, etag TEXT,
  PRIMARY KEY(backend, remote_id));

CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5(
  content, node_id UNINDEXED, convo_id UNINDEXED, tokenize='porter');
)sql";

	char* errmsg = nullptr;
	if (sqlite3_exec(db.get(), schema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
		std::string m = errmsg ? errmsg : "schema migration failed";
		sqlite3_free(errmsg);
		return fail(errc::sqlite, std::move(m));
	}
	return store(std::move(db));
}

result<void> store::put_conversation(const conversation& c) {
	stmt q;
	if (q.prepare(db_.get(),
	              "INSERT INTO conversations(id,title,project,source,created_at,active_leaf) "
	              "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(id) DO UPDATE SET "
	              "title=?2, project=?3, source=?4, active_leaf=?6") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "put_conversation/prepare"));
	q.text(1, c.id.str());
	q.text(2, c.title);
	q.text(3, c.project);
	q.text(4, c.source);
	q.integer(5, c.created_at);
	if (c.active_leaf) q.text(6, c.active_leaf->str()); else q.null(6);
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "put_conversation"));
	return {};
}

result<conversation> store::conversation_of(const convo_id& id) {
	stmt q;
	if (q.prepare(db_.get(),
	              "SELECT id,title,project,source,created_at,active_leaf FROM conversations WHERE id=?1") !=
	    SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "conversation_of"));
	q.text(1, id.str());
	const int rc = q.step();
	if (rc == SQLITE_DONE) return fail(errc::not_found, "no such conversation: " + id.str());
	if (rc != SQLITE_ROW) return std::unexpected(sqlite_err(db_.get(), "conversation_of"));
	conversation c;
	c.id = convo_id{q.col_text(0)};
	c.title = q.col_text(1);
	c.project = q.col_text(2);
	c.source = q.col_text(3);
	c.created_at = q.col_int(4);
	if (!q.col_null(5)) c.active_leaf = node_id{q.col_text(5)};
	return c;
}

result<std::vector<conversation>> store::conversations() {
	stmt q;
	if (q.prepare(db_.get(),
	              "SELECT id,title,project,source,created_at,active_leaf FROM conversations "
	              "ORDER BY created_at DESC") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "conversations"));
	std::vector<conversation> out;
	while (q.step() == SQLITE_ROW) {
		conversation c;
		c.id = convo_id{q.col_text(0)};
		c.title = q.col_text(1);
		c.project = q.col_text(2);
		c.source = q.col_text(3);
		c.created_at = q.col_int(4);
		if (!q.col_null(5)) c.active_leaf = node_id{q.col_text(5)};
		out.push_back(std::move(c));
	}
	return out;
}

result<void> store::set_active_leaf(const convo_id& c, const node_id& leaf) {
	stmt q;
	if (q.prepare(db_.get(), "UPDATE conversations SET active_leaf=?2 WHERE id=?1") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "set_active_leaf"));
	q.text(1, c.str());
	q.text(2, leaf.str());
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "set_active_leaf"));
	return {};
}

result<void> store::delete_conversation(const convo_id& id) {
	char* err = nullptr;
	if (sqlite3_exec(db_.get(), "BEGIN", nullptr, nullptr, &err) != SQLITE_OK) {
		sqlite3_free(err);
		return std::unexpected(sqlite_err(db_.get(), "delete_conversation/begin"));
	}
	auto run = [&](const char* sql) -> bool {
		stmt q;
		if (q.prepare(db_.get(), sql) != SQLITE_OK) return false;
		q.text(1, id.str());
		return q.step() == SQLITE_DONE;
	};
	const bool ok =
	    run("DELETE FROM nodes_fts WHERE convo_id=?1") &&
	    run("DELETE FROM attachments WHERE node_id IN (SELECT id FROM nodes WHERE convo_id=?1)") &&
	    run("DELETE FROM nodes WHERE convo_id=?1") &&
	    run("DELETE FROM convo_tags WHERE convo_id=?1") &&
	    run("DELETE FROM conversations WHERE id=?1");
	sqlite3_exec(db_.get(), ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
	if (!ok) return std::unexpected(sqlite_err(db_.get(), "delete_conversation"));
	return {};
}

result<void> store::put_node(const node& n) {
	stmt q;
	if (q.prepare(db_.get(),
	              "INSERT INTO nodes(id,convo_id,parent_id,role,content,model,params,tokens_in,"
	              "tokens_out,cost,state,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12) "
	              "ON CONFLICT(id) DO UPDATE SET content=?5, model=?6, params=?7, tokens_in=?8, "
	              "tokens_out=?9, cost=?10, state=?11") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "put_node"));
	q.text(1, n.id.str());
	q.text(2, n.convo.str());
	if (n.parent) q.text(3, n.parent->str()); else q.null(3);
	q.text(4, to_string(n.role));
	q.text(5, n.content_json);
	q.text(6, n.model);
	q.text(7, n.params_json);
	q.integer(8, n.tokens_in);
	q.integer(9, n.tokens_out);
	q.real(10, n.cost);
	q.text(11, to_string(n.state));
	q.integer(12, n.created_at);
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "put_node"));

	// refresh the fts row for this node.
	stmt del;
	if (del.prepare(db_.get(), "DELETE FROM nodes_fts WHERE node_id=?1") == SQLITE_OK) {
		del.text(1, n.id.str());
		del.step();
	}
	if (n.state != node_state::pruned) {
		stmt ins;
		if (ins.prepare(db_.get(),
		                "INSERT INTO nodes_fts(content,node_id,convo_id) VALUES(?1,?2,?3)") ==
		    SQLITE_OK) {
			const std::string text = index_text(n.content_json);
			ins.text(1, text);
			ins.text(2, n.id.str());
			ins.text(3, n.convo.str());
			ins.step();
		}
	}
	return {};
}

result<node> store::node_of(const node_id& id) {
	stmt q;
	if (q.prepare(db_.get(), (std::string("SELECT ") + kNodeCols + " FROM nodes WHERE id=?1").c_str()) !=
	    SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "node_of"));
	q.text(1, id.str());
	const int rc = q.step();
	if (rc == SQLITE_DONE) return fail(errc::not_found, "no such node: " + id.str());
	if (rc != SQLITE_ROW) return std::unexpected(sqlite_err(db_.get(), "node_of"));
	return read_node(q);
}

result<std::vector<node>> store::nodes_of(const convo_id& id) {
	stmt q;
	if (q.prepare(db_.get(), (std::string("SELECT ") + kNodeCols +
	                          " FROM nodes WHERE convo_id=?1 ORDER BY created_at ASC")
	                             .c_str()) != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "nodes_of"));
	q.text(1, id.str());
	std::vector<node> out;
	while (q.step() == SQLITE_ROW) out.push_back(read_node(q));
	return out;
}

result<std::vector<node>> store::children_of(const node_id& id) {
	stmt q;
	if (q.prepare(db_.get(), (std::string("SELECT ") + kNodeCols +
	                          " FROM nodes WHERE parent_id=?1 ORDER BY created_at ASC")
	                             .c_str()) != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "children_of"));
	q.text(1, id.str());
	std::vector<node> out;
	while (q.step() == SQLITE_ROW) out.push_back(read_node(q));
	return out;
}

result<void> store::set_state(const node_id& id, node_state s) {
	stmt q;
	if (q.prepare(db_.get(), "UPDATE nodes SET state=?2 WHERE id=?1") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "set_state"));
	q.text(1, id.str());
	q.text(2, to_string(s));
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "set_state"));
	return {};
}

result<void> store::set_parent(const node_id& id, const std::optional<node_id>& parent) {
	stmt q;
	if (q.prepare(db_.get(), "UPDATE nodes SET parent_id=?2 WHERE id=?1") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "set_parent"));
	q.text(1, id.str());
	if (parent) q.text(2, parent->str()); else q.null(2);
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "set_parent"));
	return {};
}

result<void> store::put_attachment(const attachment& a) {
	stmt q;
	if (q.prepare(db_.get(),
	              "INSERT OR REPLACE INTO attachments(id,node_id,kind,media_type,path,bytes) "
	              "VALUES(?1,?2,?3,?4,?5,?6)") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "put_attachment"));
	q.text(1, a.id.str());
	q.text(2, a.node.str());
	q.text(3, a.kind);
	q.text(4, a.media_type);
	q.text(5, a.path);
	q.integer(6, a.bytes);
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "put_attachment"));
	return {};
}

result<std::vector<attachment>> store::attachments_of(const node_id& id) {
	stmt q;
	if (q.prepare(db_.get(),
	              "SELECT id,node_id,kind,media_type,path,bytes FROM attachments WHERE node_id=?1") !=
	    SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "attachments_of"));
	q.text(1, id.str());
	std::vector<attachment> out;
	while (q.step() == SQLITE_ROW) {
		attachment a;
		a.id = attachment_id{q.col_text(0)};
		a.node = node_id{q.col_text(1)};
		a.kind = q.col_text(2);
		a.media_type = q.col_text(3);
		a.path = q.col_text(4);
		a.bytes = q.col_int(5);
		out.push_back(std::move(a));
	}
	return out;
}

result<void> store::tag(const convo_id& c, std::string_view name) {
	{
		stmt q;
		if (q.prepare(db_.get(), "INSERT OR IGNORE INTO tags(name) VALUES(?1)") != SQLITE_OK)
			return std::unexpected(sqlite_err(db_.get(), "tag"));
		q.text(1, name);
		q.step();
	}
	stmt q;
	if (q.prepare(db_.get(), "INSERT OR IGNORE INTO convo_tags(convo_id,tag) VALUES(?1,?2)") !=
	    SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "tag"));
	q.text(1, c.str());
	q.text(2, name);
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "tag"));
	return {};
}

result<void> store::untag(const convo_id& c, std::string_view name) {
	stmt q;
	if (q.prepare(db_.get(), "DELETE FROM convo_tags WHERE convo_id=?1 AND tag=?2") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "untag"));
	q.text(1, c.str());
	q.text(2, name);
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "untag"));
	return {};
}

result<std::vector<std::string>> store::tags_of(const convo_id& c) {
	stmt q;
	if (q.prepare(db_.get(), "SELECT tag FROM convo_tags WHERE convo_id=?1 ORDER BY tag") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "tags_of"));
	q.text(1, c.str());
	std::vector<std::string> out;
	while (q.step() == SQLITE_ROW) out.push_back(q.col_text(0));
	return out;
}

namespace {
result<std::vector<search_hit>> run_search(sqlite3* db, const char* sql, std::string_view query,
                                           const convo_id* scope, int limit) {
	stmt q;
	if (q.prepare(db, sql) != SQLITE_OK) return std::unexpected(sqlite_err(db, "search/prepare"));
	int idx = 1;
	q.text(idx++, query);
	if (scope) q.text(idx++, scope->str());
	q.integer(idx, limit);
	std::vector<search_hit> out;
	while (q.step() == SQLITE_ROW) {
		search_hit h;
		h.convo = convo_id{q.col_text(0)};
		h.node = node_id{q.col_text(1)};
		h.snippet = q.col_text(2);
		out.push_back(std::move(h));
	}
	return out;
}
}  // namespace

result<std::vector<search_hit>> store::search(std::string_view query, int limit) {
	return run_search(db_.get(),
	                  "SELECT convo_id, node_id, snippet(nodes_fts,0,'[',']','…',12) FROM nodes_fts "
	                  "WHERE nodes_fts MATCH ?1 ORDER BY rank LIMIT ?2",
	                  query, nullptr, limit);
}

result<std::vector<search_hit>> store::search_in(const convo_id& c, std::string_view query,
                                                 int limit) {
	return run_search(db_.get(),
	                  "SELECT convo_id, node_id, snippet(nodes_fts,0,'[',']','…',12) FROM nodes_fts "
	                  "WHERE nodes_fts MATCH ?1 AND convo_id=?2 ORDER BY rank LIMIT ?3",
	                  query, &c, limit);
}

result<void> store::put_sync(const sync_row& r) {
	stmt q;
	if (q.prepare(db_.get(),
	              "INSERT INTO sync_map(backend,remote_id,local_id,etag) VALUES(?1,?2,?3,?4) "
	              "ON CONFLICT(backend,remote_id) DO UPDATE SET local_id=?3, etag=?4") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "put_sync"));
	q.text(1, r.backend);
	q.text(2, r.remote_id);
	q.text(3, r.local_id);
	q.text(4, r.etag);
	if (q.step() != SQLITE_DONE) return std::unexpected(sqlite_err(db_.get(), "put_sync"));
	return {};
}

result<std::optional<sync_row>> store::sync_for(std::string_view backend, std::string_view remote) {
	stmt q;
	if (q.prepare(db_.get(),
	              "SELECT backend,remote_id,local_id,etag FROM sync_map WHERE backend=?1 AND "
	              "remote_id=?2") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "sync_for"));
	q.text(1, backend);
	q.text(2, remote);
	if (q.step() != SQLITE_ROW) return std::optional<sync_row>{};
	return std::optional<sync_row>{sync_row{q.col_text(0), q.col_text(1), q.col_text(2), q.col_text(3)}};
}

result<bool> store::check_integrity() {
	stmt q;
	if (q.prepare(db_.get(), "PRAGMA integrity_check") != SQLITE_OK)
		return std::unexpected(sqlite_err(db_.get(), "check_integrity"));
	if (q.step() != SQLITE_ROW) return std::unexpected(sqlite_err(db_.get(), "check_integrity"));
	return q.col_text(0) == "ok";
}

}  // namespace plume
