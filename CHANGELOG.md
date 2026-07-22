# changelog

notable changes to plume. format follows [keep a changelog](https://keepachangelog.com/en/1.1.0/), versions follow semver.

## [unreleased]

nothing yet.

## [0.1.0] - 2026-07-22

initial cut.

### added

- sqlite-backed conversation tree: one database file, wal mode, every chat a weaveable tree
- providers: anthropic and openai-compatible endpoints, both with sse streaming
- inline images via kitty unicode placeholders, sixel and chafa fallbacks for lesser terminals
- weave engine: sibling navigation, graft, prune, adopt, per-branch export
- lua plugin api: hooks on send/receive, custom slash commands, statusline segments
- mcp client for attaching external tool servers
- claude.ai export import: pull a conversations.json dump into the local tree

### experimental

behind `experimental.*` config flags, expect breakage:

- live claude.ai sync (two-way, currently polling)
- tree-sitter syntax highlighting in code blocks
- latex rendering
- batch requests
