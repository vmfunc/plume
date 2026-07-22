self:
{ config, lib, pkgs, ... }:

let
  cfg = config.programs.plume;
  toml = pkgs.formats.toml { };
in
{
  options.programs.plume = {
    enable = lib.mkEnableOption "plume, a quill for terminals";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.system}.default;
      description = "the plume package to install.";
    };

    settings = lib.mkOption {
      type = toml.type;
      default = { };
      description = ''
        config written to ~/.config/plume/config.toml. mirrors the toml
        schema documented in docs/config.md.
      '';
      example = lib.literalExpression ''
        {
          theme = "rose-pine";
          keys.preset = "vim";
          providers.anthropic.key_cmd = "pass show anthropic/api";
        }
      '';
    };

    themes = lib.mkOption {
      type = lib.types.attrsOf lib.types.path;
      default = { };
      description = "extra theme files linked into ~/.config/plume/themes.";
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];

    xdg.configFile = lib.mkMerge [
      (lib.mkIf (cfg.settings != { }) {
        "plume/config.toml".source = toml.generate "config.toml" cfg.settings;
      })
      (lib.mapAttrs'
        (name: path: lib.nameValuePair "plume/themes/${name}.toml" { source = path; })
        cfg.themes)
    ];
  };
}
