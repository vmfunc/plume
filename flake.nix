{
  description = "plume — a quill for terminals";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    let
      overlay = final: prev: {
        # clang 19 + libstdc++, matching the dev shell so there is one compiler
        # and one warning story across `just` and `nix build`.
        plume = final.callPackage ./nix/package.nix {
          stdenv = final.llvmPackages_19.stdenv;
        };
      };
    in
    flake-utils.lib.eachSystem [
      "x86_64-linux"
      "aarch64-linux"
      "aarch64-darwin"
    ]
      (system:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [ overlay ];
          };

          # libraries every build needs, host-portable
          libs = with pkgs; [
            ftxui
            curl
            sqlite
            nlohmann_json
            tomlplusplus
            luajit
            sol2
            tree-sitter
            chafa
            spdlog
            doctest
            stb
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ libsecret ];

          tools = with pkgs; [
            llvmPackages_19.clang
            cmake
            ninja
            just
            llvmPackages_19.clang-tools
            lldb
            scdoc
            pkg-config
          ];
        in
        {
          packages.default = pkgs.plume;
          packages.plume = pkgs.plume;

          apps.default = {
            type = "app";
            program = "${pkgs.plume}/bin/plume";
            meta.description = "a quill for terminals";
          };

          devShells.default = pkgs.mkShell {
            packages = tools ++ libs;
            # clang, not gcc, and let cmake find the deps by pkg-config / cmake config
            shellHook = ''
              export CC=clang
              export CXX=clang++
              echo "plume dev shell. 'just' lists recipes."
            '';
          };

          checks = {
            build = pkgs.plume;
            test = pkgs.plume.overrideAttrs (old: {
              pname = "plume-tests";
              doCheck = true;
            });

            # every tracked source is clang-format clean.
            format = pkgs.runCommand "plume-format"
              { nativeBuildInputs = [ pkgs.llvmPackages_19.clang-tools ]; } ''
              cd ${self}
              files=$(find src include tests -type f \( -name '*.cpp' -o -name '*.hpp' \))
              clang-format --dry-run --Werror $files
              touch $out
            '';

            # clang-tidy over the tree. reuse the package's own configure so the
            # pkg-config closure (chafa -> glib) is intact, then run clang-tidy
            # directly: the python run-clang-tidy wrapper needs /usr/bin/env, which
            # the sandbox lacks, and the clang wrapper hides the libstdc++ include
            # path from raw clang-tidy, so we replay it through CPLUS_INCLUDE_PATH.
            lint = pkgs.plume.overrideAttrs (old: {
              pname = "plume-lint";
              nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.llvmPackages_19.clang-tools ];
              buildPhase = ''
                runHook preBuild
                ccdir=$(dirname $(find "$NIX_BUILD_TOP" -name compile_commands.json | head -1))
                export CPLUS_INCLUDE_PATH=$(clang++ -xc++ -E -v /dev/null 2>&1 \
                  | sed -n 's|^ \(/nix/store.*\)|\1|p' | paste -sd:)
                for f in $(find "$NIX_BUILD_TOP" -path '*/src/*.cpp'); do
                  clang-tidy -p "$ccdir" -quiet "$f"
                done
                runHook postBuild
              '';
              installPhase = "touch $out";
              doCheck = false;
              dontFixup = true;
            });
          };
        })
    // {
      overlays.default = overlay;
      homeManagerModules.plume = import ./nix/hm-module.nix self;
    };
}
