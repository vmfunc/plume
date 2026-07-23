self:
{ config, lib, pkgs, ... }:

let
  inherit (lib) types mkOption mkEnableOption mkIf literalExpression;
  cfg = config.programs.plume;
  toml = pkgs.formats.toml { };

  # drop keys whose value is null so we never write an empty string that would
  # shadow a plume-side default; drop empty sub-tables entirely.
  pruneNull = lib.filterAttrs (_: v: v != null);
  pruneEmpty = lib.filterAttrs (_: v: v != null && v != { } && v != [ ]);

  providerModule = types.submodule {
    options = {
      kind = mkOption {
        type = types.enum [ "anthropic" "openai" "openrouter" "ollama" "openai-compatible" ];
        description = "provider backend.";
      };
      baseUrl = mkOption {
        type = types.nullOr types.str;
        default = null;
        description = "override the api base url (openai-compatible, ollama, self-hosted).";
      };
      authSource = mkOption {
        type = types.enum [ "env" "key_cmd" "keychain" "inline" ];
        default = "env";
        description = ''
          where plume reads the credential. `env` names an environment variable,
          `key_cmd` runs a command (e.g. `pass show`, `op read`), `keychain` uses
          the os keychain, `inline` stores the key in the config file itself.
          prefer anything but `inline` under nix: the generated config lands in the
          world-readable nix store.
        '';
      };
      authValue = mkOption {
        type = types.nullOr types.str;
        default = null;
        example = "ANTHROPIC_API_KEY";
        description = "the env var name, the key command, the keychain account, or the key.";
      };
      defaultModel = mkOption {
        type = types.nullOr types.str;
        default = null;
        description = "model id for this provider; discovered from its list endpoint if unset.";
      };
    };
  };

  priceModule = types.submodule {
    options = {
      input = mkOption { type = types.float; default = 0.0; description = "usd per 1e6 input tokens."; };
      output = mkOption { type = types.float; default = 0.0; description = "usd per 1e6 output tokens."; };
      cacheWrite = mkOption { type = types.float; default = 0.0; description = "usd per 1e6 cache-write tokens."; };
      cacheRead = mkOption { type = types.float; default = 0.0; description = "usd per 1e6 cache-read tokens."; };
    };
  };

  mcpModule = types.submodule {
    options = {
      transport = mkOption {
        type = types.enum [ "stdio" "http" ];
        default = "stdio";
        description = "how plume reaches the server.";
      };
      command = mkOption {
        type = types.nullOr types.str;
        default = null;
        description = "executable to spawn for a stdio server.";
      };
      args = mkOption {
        type = types.listOf types.str;
        default = [ ];
        description = "arguments for the stdio command.";
      };
      url = mkOption {
        type = types.nullOr types.str;
        default = null;
        description = "endpoint for an http server.";
      };
      approval = mkOption {
        type = types.enum [ "ask" "allowlist" "yolo" ];
        default = "ask";
        description = "tool-call gate: prompt every time, allow a named set, or trust all.";
      };
      allow = mkOption {
        type = types.listOf types.str;
        default = [ ];
        description = "tool names auto-approved when approval = \"allowlist\".";
      };
    };
  };
in
{
  options.programs.plume = {
    enable = mkEnableOption "plume, a quill for terminals";

    package = mkOption {
      type = types.package;
      default = self.packages.${pkgs.system}.default;
      defaultText = literalExpression "plume.packages.\${system}.default";
      description = "the plume package to install.";
    };

    theme = mkOption {
      type = types.str;
      default = "rose-pine";
      example = "rose-pine-moon";
      description = "active theme: a built-in (rose-pine, rose-pine-moon, rose-pine-dawn, va11) or a name from `themes`.";
    };

    density = mkOption {
      type = types.enum [ "cozy" "compact" ];
      default = "cozy";
      description = "spacing of the transcript.";
    };

    reduceMotion = mkOption {
      type = types.bool;
      default = false;
      description = "hold every animation still.";
    };

    zen = mkOption {
      type = types.bool;
      default = false;
      description = "hide all chrome.";
    };

    sidebar = mkOption {
      type = types.bool;
      default = true;
      description = "show the conversation sidebar on start.";
    };

    keybindings = mkOption {
      type = types.enum [ "vim" "emacs" ];
      default = "vim";
      description = "the modal-editing preset for the composer.";
    };

    binds = mkOption {
      type = types.attrsOf types.str;
      default = { };
      example = literalExpression ''{ "weave.spawn" = "ctrl-s"; }'';
      description = "per-action key overrides on top of the preset.";
    };

    notify = mkOption {
      type = types.enum [ "bell" "osc9" "off" ];
      default = "bell";
      description = "how plume nudges you when a turn lands and the window is unfocused.";
    };

    defaultProvider = mkOption {
      type = types.nullOr types.str;
      default = null;
      example = "anthropic";
      description = "which entry of `providers` to use unless a conversation overrides it.";
    };

    providers = mkOption {
      type = types.attrsOf providerModule;
      default = { };
      example = literalExpression ''
        {
          anthropic = { kind = "anthropic"; authSource = "key_cmd"; authValue = "pass show anthropic/api"; };
          local = { kind = "ollama"; baseUrl = "http://localhost:11434/v1"; defaultModel = "llama3.2"; };
        }
      '';
      description = "named provider backends. configuring one skips the first-run wizard.";
    };

    defaults = {
      model = mkOption {
        type = types.nullOr types.str;
        default = null;
        description = "default model id; falls back to the provider's default when unset.";
      };
      maxTokens = mkOption {
        type = types.nullOr types.int;
        default = null;
        description = "max output tokens per turn.";
      };
      temperature = mkOption {
        type = types.nullOr types.float;
        default = null;
        description = "sampling temperature.";
      };
      topP = mkOption {
        type = types.nullOr types.float;
        default = null;
        description = "nucleus sampling cutoff.";
      };
      thinking = mkOption {
        type = types.enum [ "off" "adaptive" "budget" ];
        default = "off";
        description = "extended thinking mode.";
      };
      thinkingBudget = mkOption {
        type = types.nullOr types.int;
        default = null;
        description = "token ceiling for thinking = \"budget\" (older model lines).";
      };
      effort = mkOption {
        type = types.nullOr (types.enum [ "low" "medium" "high" "xhigh" "max" ]);
        default = null;
        description = "reasoning effort for adaptive thinking.";
      };
    };

    prices = mkOption {
      type = types.attrsOf priceModule;
      default = { };
      description = "cost table keyed by model id. a snapshot; verify against your provider.";
    };

    plugins = mkOption {
      type = types.listOf types.str;
      default = [ ];
      example = literalExpression ''[ "wordcount" "translate" ]'';
      description = "plugin directory names under the plugins dir to load on start.";
    };

    pluginDirs = mkOption {
      type = types.attrsOf types.path;
      default = { };
      example = literalExpression ''{ wordcount = ./plugins/wordcount; }'';
      description = "plugin directories linked into ~/.config/plume/plugins/<name>.";
    };

    mcpServers = mkOption {
      type = types.attrsOf mcpModule;
      default = { };
      example = literalExpression ''
        {
          filesystem = { command = "mcp-server-filesystem"; args = [ "/home/me/notes" ]; approval = "allowlist"; allow = [ "read_file" ]; };
        }
      '';
      description = "mcp servers whose tools are offered to models that support tool use.";
    };

    themes = mkOption {
      type = types.attrsOf types.path;
      default = { };
      description = "extra theme files linked into ~/.config/plume/themes/<name>.toml.";
    };

    settings = mkOption {
      type = toml.type;
      default = { };
      description = ''
        escape hatch: raw config merged on top of the typed options above, using
        plume's toml schema verbatim (ui.theme, defaults.max_tokens, and so on).
        anything set here wins.
      '';
    };
  };

  config = mkIf cfg.enable {
    # a nix-store config.toml is world-readable; never let a raw key sit in it.
    warnings = lib.optional
      (lib.any (p: p.authSource == "inline" && p.authValue != null)
        (lib.attrValues cfg.providers))
      "programs.plume: a provider uses authSource = \"inline\", which writes the key into the world-readable nix store. prefer \"env\", \"key_cmd\", or \"keychain\".";

    home.packages = [ cfg.package ];

    xdg.configFile = lib.mkMerge [
      {
        "plume/config.toml".source =
          let
            generated = pruneEmpty {
              ui = pruneNull {
                theme = cfg.theme;
                density = cfg.density;
                reduce_motion = cfg.reduceMotion;
                zen = cfg.zen;
                sidebar = cfg.sidebar;
              };
              keys = pruneNull {
                preset = cfg.keybindings;
                binds = if cfg.binds == { } then null else cfg.binds;
              };
              default_provider = cfg.defaultProvider;
              notify = cfg.notify;
              providers = lib.mapAttrs
                (_: p: pruneNull {
                  kind = p.kind;
                  base_url = p.baseUrl;
                  auth_source = p.authSource;
                  auth_value = p.authValue;
                  default_model = p.defaultModel;
                })
                cfg.providers;
              defaults = pruneNull {
                model = cfg.defaults.model;
                max_tokens = cfg.defaults.maxTokens;
                temperature = cfg.defaults.temperature;
                top_p = cfg.defaults.topP;
                thinking = cfg.defaults.thinking;
                thinking_budget = cfg.defaults.thinkingBudget;
                effort = cfg.defaults.effort;
              };
              prices = lib.mapAttrs
                (_: p: {
                  input = p.input;
                  output = p.output;
                  cache_write = p.cacheWrite;
                  cache_read = p.cacheRead;
                })
                cfg.prices;
              plugins = cfg.plugins;
              # [[mcp]] is a toml array of tables, so fold the attrset into a list.
              mcp = lib.mapAttrsToList
                (name: s: pruneNull {
                  inherit name;
                  transport = s.transport;
                  command = s.command;
                  args = if s.args == [ ] then null else s.args;
                  url = s.url;
                  approval = s.approval;
                  allow = if s.allow == [ ] then null else s.allow;
                })
                cfg.mcpServers;
            };
          in
          toml.generate "config.toml" (lib.recursiveUpdate generated cfg.settings);
      }
      (lib.mapAttrs'
        (name: path: lib.nameValuePair "plume/themes/${name}.toml" { source = path; })
        cfg.themes)
      (lib.mapAttrs'
        (name: path: lib.nameValuePair "plume/plugins/${name}" { source = path; })
        cfg.pluginDirs)
    ];
  };
}
