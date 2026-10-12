{
  description = "SLADE flake";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in {
          default = pkgs.stdenv.mkDerivation {
            pname = "slade";
            version = "3.2.0";

            src = ./.;

            nativeBuildInputs = with pkgs; [
              cmake
              makeWrapper
              pkg-config
              zip
            ];

            buildInputs = with pkgs; [
              bzip2
              fluidsynth
              freetype
              ftgl
              libGL
              libGLU
              libwebp
              lua5_4
              mpg123
              sfml
              wxGTK33
              zlib
            ];

            cmakeFlags = [
              "-DNO_WEBVIEW=ON"
              "-DwxWidgets_CONFIG_EXECUTABLE=${pkgs.wxGTK33}/bin/wx-config"
            ];

            postFixup = ''
              wrapProgram "$out/bin/slade" \
                --set-default GDK_BACKEND x11
            '';

            meta = with pkgs.lib; {
              description = "Modern Doom editor";
              homepage = "https://github.com/sirjuddington/SLADE";
              license = licenses.gpl2Plus;
              mainProgram = "slade";
              platforms = platforms.linux;
            };
          };
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            nativeBuildInputs = with pkgs; [
              cmake
              makeWrapper
              pkg-config
              zip
            ];
          };
        });
    };
}
