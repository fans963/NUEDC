{
  description = "NUEDC — C++26 component framework with reflection";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          pkgsCross = import nixpkgs {
            inherit system;
            crossSystem = nixpkgs.lib.systems.examples.aarch64-multiplatform;
          };
          crossGcc = pkgsCross.buildPackages.gcc16;
        in {
          default = pkgs.mkShell {
            nativeBuildInputs = with pkgs; [
              cmake ninja pkg-config flatbuffers gdb libusb1
              crossGcc
            ];

            shellHook = ''
              export VCPKG_FORCE_SYSTEM_BINARIES=1
              export CC=aarch64-unknown-linux-gnu-gcc
              export CXX=aarch64-unknown-linux-gnu-g++
              echo "NUEDC cross shell: aarch64 GCC $(aarch64-unknown-linux-gnu-g++ --version | head -1)"
            '';
          };
        }
      );
    };
}
