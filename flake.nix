{
  description = "Game Boy emulator development environment for Web and M5Stack CoreS3";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
    nixpkgs-unstable.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      nixpkgs,
      nixpkgs-unstable,
      flake-utils,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        pkgsUnstable = nixpkgs-unstable.legacyPackages.${system};
        pioPythonPackages = with pkgs.python312Packages; [
          bitstring
          intelhex
          pip
          pyserial
          reedsolo
        ];
        pioPythonPath = pkgs.lib.concatMapStringsSep ":" (
          package: "${package}/lib/python3.12/site-packages"
        ) pioPythonPackages;
      in
      {
        formatter = pkgs.nixfmt-rfc-style;

        devShells.default = pkgs.mkShell {
          packages = [
            pkgs.actionlint
            pkgs.cmake
            pkgs.llvmPackages.libcxxClang
            pkgs.clang-tools
            pkgs.emscripten
            pkgs.gitleaks
            pkgs.just
            pkgs.lefthook
            pkgs.nixfmt-rfc-style
            pkgs.platformio-core
            pkgs.ripgrep
            pkgs.ruff
            pkgsUnstable.pinact
          ];

          shellHook = ''
            export PYTHONPATH="${pioPythonPath}''${PYTHONPATH:+:$PYTHONPATH}"
            export GB_CLANG_CXX_INCLUDE="${pkgs.libcxx.dev}/include/c++/v1"
            export GB_CLANG_RESOURCE_INCLUDE="$(clang -print-resource-dir)/include"
            if [ -d .git ]; then lefthook install --force > /dev/null; fi
            export EM_CACHE="$HOME/.cache/emscripten-${pkgs.emscripten.version}"
            if [ ! -d "$EM_CACHE" ]; then
              mkdir -p "$EM_CACHE"
              cp -r ${pkgs.emscripten}/share/emscripten/cache/. "$EM_CACHE"
              chmod -R u+w "$EM_CACHE"
            fi
          '';
        };
      }
    );
}
