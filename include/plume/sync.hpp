// the claude.ai link. two backends behind one interface: the official data
// export (the stable, supported path) and a live session-cookie adapter
// (experimental, off unless the user sets unofficial = true). import is
// idempotent — re-importing updates rather than duplicates, keyed through the
// store's sync_map.
#pragma once

#include <memory>
#include <string>

#include "plume/error.hpp"
#include "plume/store.hpp"

namespace plume {

struct import_stats {
	int conversations = 0;
	int nodes = 0;
	int created = 0;
	int updated = 0;
};

class sync_backend {
   public:
	virtual ~sync_backend() = default;
	[[nodiscard]] virtual std::string_view name() const = 0;
	// pull remote conversations into the store, updating rows the sync_map
	// already knows and inserting the rest. mirrored conversations are marked
	// with source "claude-export" or "claude-live".
	[[nodiscard]] virtual result<import_stats> import_into(store&) = 0;
};

// read an unpacked claude.ai data export directory (conversations.json, etc.).
[[nodiscard]] result<std::unique_ptr<sync_backend>> open_export(const std::string& export_dir);

// experimental: speak the endpoints the web app uses with a session cookie.
// read-only unless two_way is set (a second explicit opt-in).
[[nodiscard]] result<std::unique_ptr<sync_backend>> open_live(const std::string& session_cookie,
                                                              bool two_way);

}  // namespace plume
