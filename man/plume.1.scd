plume(1)

# NAME

plume - a quill for terminals

# SYNOPSIS

*plume* [_command_] [_options_...]

# DESCRIPTION

plume is a tui client for talking to language models. conversations are
trees, not logs: any message can branch, and every branch is kept.
history lives in a local sqlite database. images render inline where the
terminal allows it. syncs with claude.ai when asked to. extensible with
lua plugins. no electron, no telemetry, no cloud between you and the api.

run with no arguments to open the tui.

# COMMANDS

*plume*
	open the tui. the default when no command is given.

*plume ask* "_question_"
	ask one question, print the answer to stdout, exit. when stdin is
	a pipe, its contents are attached to the question as context. the
	exchange is saved like any other conversation.

	*--model* _name_
		model to use for this question. overrides the configured
		default.

	*--json*
		print the full response as json instead of plain text.
		implies *--no-stream*.

	*--no-stream*
		wait for the complete answer before printing anything.
		useful when the output feeds another program.

*plume export* _conversation_ [*--format* _md_|_json_|_html_]
	write a conversation to stdout. _conversation_ is an id or a
	unique title prefix. default format is _md_. json keeps the whole
	tree; md and html follow the currently selected branch, because
	flat formats cannot hold a fork honestly.

*plume import* _file_
	import conversations from _file_ into the local database. accepts
	plume's own json export and claude.ai export archives. existing
	conversations with the same id are left alone.

*plume themes*
	list installed themes, each previewed in its own colors so you
	can judge them where they will actually live.

*plume doctor*
	check the environment: config parses, database is intact, api key
	is present, terminal capabilities (truecolor, image protocol).
	exits nonzero if something needs attention.

*plume config edit*
	open the config in $EDITOR, creating it from the commented
	default first if it does not exist. validated on save so a typo
	does not greet you at next startup.

# FILES

_$XDG_CONFIG_HOME/plume/config.toml_
	configuration. _~/.config/plume/config.toml_ when the variable is
	unset.

_$XDG_DATA_HOME/plume_
	the sqlite database, attachments, installed themes and plugins.
	_~/.local/share/plume_ when unset.

_$XDG_STATE_HOME/plume_
	logs and runtime state. safe to delete; nothing you would miss
	lives here. _~/.local/state/plume_ when unset.

# SEE ALSO

*sqlite3*(1), *lua*(1)

the plugin api and theme format are documented in the repository under
_doc/_.
