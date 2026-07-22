# plume

plume — a quill for terminals. talk to models, weave the branches, keep your history in sqlite where it belongs. no electron, no telemetry, no cloud between you and the api.

a c++ tui client for language models. every conversation is a tree, not a scroll: branch a reply, walk the siblings, graft a subtree where it fits better, prune what didn't work. images render inline for real (kitty graphics, sixel or chafa where they must). storage is one sqlite file you can back up, grep, or query. plugins are lua. it speaks to anthropic and anything openai-compatible, and it can import your claude.ai history so nothing gets left behind.

![plume in a kitty terminal](./assets/screenshot.png)

![weaving a conversation tree](./assets/weave.gif)

## install

from a checkout:

    nix profile install .

as a flake input:

    inputs.plume.url = "github:vmfunc/plume";

or via the home-manager module:

    programs.plume = {
      enable = true;
      settings.provider = "anthropic";
    };

## quickstart

    $ plume                 # first run drops into a wizard: provider, key, model
    / how do i mmap a file  # press / to talk, replies stream in place
    / draw me a diagram     # images from the model render inline
    ctrl-w                  # weave: the conversation as a tree
      j/k h/l               #   parents, children, siblings
      enter                 #   continue from any node
      g / p                 #   graft a subtree elsewhere / prune a branch
      e                     #   export the current branch as markdown
    q                       # back to chat, on whichever branch you left
    :q                      # quit; everything is already in sqlite

## what it is not

not an electron app, it is a terminal program that starts in milliseconds. not a telemetry endpoint, nothing phones home, ever. your keys go straight to the api and your history never leaves the database file.
