#!/usr/bin/env bash
# scaffold a plugin under $XDG_CONFIG_HOME/plume/plugins/<name>.
set -euo pipefail

name="${1:-}"
if [ -z "$name" ]; then
	echo "usage: just plugin-new <name>" >&2
	exit 1
fi

dir="${XDG_CONFIG_HOME:-$HOME/.config}/plume/plugins/$name"
if [ -e "$dir" ]; then
	echo "plugin already exists: $dir" >&2
	exit 1
fi
mkdir -p "$dir"

cat > "$dir/plugin.toml" <<TOML
name = "$name"
version = "0.1.0"
entry = "init.lua"
# request only what you use. the user approves each on first load.
capabilities = []
TOML

cat > "$dir/init.lua" <<'LUA'
-- hooks: setup, pre_send(text)->text, on_chunk(delta), post_receive(full),
--        on_key(key)->bool. register with plume.on(name, fn).
-- also: plume.command(name, fn), plume.keymap(key, fn),
--       plume.statusline(id, fn)->text, plume.model.complete{prompt=...}.

plume.statusline("hello", function()
	return "hi"
end)
LUA

echo "scaffolded $dir"
