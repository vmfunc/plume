# plume

plume — a quill for terminals. talk to models, weave the branches, keep your history in sqlite where it belongs. no electron, no telemetry, no cloud between you and the api.

a c++ tui client for language models. every conversation is a tree, not a scroll: branch a reply, walk the siblings, graft a subtree where it fits better, prune what didn't work. images render inline for real: kitty graphics where the terminal has them, a chafa half-block fallback everywhere else. storage is one sqlite file you can back up, grep, or query. plugins are lua. it speaks to anthropic and anything openai-compatible, and it can import your claude.ai history so nothing gets left behind.

![plume in a kitty terminal](./assets/screenshot.png)

![weaving a conversation tree](./assets/weave.gif)

## install

from a checkout:

    nix profile install .

as a flake input, then add the package to your profile or use the home-manager
module below:

    inputs.plume.url = "github:vmfunc/plume";

## configure

on nix, drive everything from the `programs.plume` home-manager module. it writes
`config.toml` from typed options, so first run lands in a working chat instead of
the wizard:

    programs.plume = {
      enable = true;
      theme = "rose-pine-moon";     # rose-pine | rose-pine-moon | rose-pine-dawn | va11, or a themes.<name>
      keybindings = "vim";          # vim | emacs
      density = "cozy";             # cozy | compact
      notify = "bell";              # bell | osc9 | off
      defaultProvider = "anthropic";
      providers.anthropic = {
        kind = "anthropic";         # anthropic | openai | openrouter | ollama | openai-compatible
        authSource = "key_cmd";     # env | key_cmd | keychain | inline
        authValue = "pass show anthropic/api";
      };
      defaults = {                  # per-conversation sampling, overridable at runtime
        thinking = "adaptive";      # off | adaptive | budget
        effort = "high";            # low | medium | high | xhigh | max
        # model, maxTokens, temperature, topP, thinkingBudget also live here
      };
      mcpServers.notes = {          # tools offered to models that support tool use
        command = "mcp-server-filesystem";
        args = [ "/home/me/notes" ];
        approval = "allowlist";     # ask | allowlist | yolo
        allow = [ "read_file" ];
      };
      # prices, plugins, pluginDirs, binds and themes are options too; anything
      # not surfaced goes in `settings`, merged on top using the raw toml schema.
    };

keep keys out of the nix store: `key_cmd` (or `env` / `keychain`) reads the key at
runtime. an `inline` key is written into the world-readable store, and the module
warns you when you set one.

off nix, first run walks a wizard (provider, key, model, theme, keys), then writes
`~/.config/plume/config.toml`, hot-reloaded on save. re-run it any time with `plume
setup`; `plume doctor` checks config, storage, terminal and provider health.

## quickstart

    $ plume                 # first run drops into a wizard: provider, key, model
    how do i mmap a file    # just type; enter sends, replies stream in place
    /model                  # a leading slash runs a command (ctrl-k for the palette)
    ctrl-w                  # weave: the conversation as a tree
      j/k                   #   walk parents, children, siblings
      enter                 #   adopt: make this node the active path
      s / r                 #   spawn 3 alternatives / regenerate one
      c                     #   compare two leaves side by side
      g / p                 #   graft a subtree elsewhere / prune a branch
      x                     #   export the tree as graphviz dot
    q                       # back to chat, on whichever branch you left
    ctrl-c                  # quit; everything is already in sqlite

## what it is not

not an electron app, it is a terminal program that starts in milliseconds. not a telemetry endpoint, nothing phones home, ever. your keys go straight to the api and your history never leaves the database file.
