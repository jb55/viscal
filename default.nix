{ pkgs ? import <nixpkgs> {} }:
with pkgs;
stdenv.mkDerivation {
  pname = "viscal";
  version = "0.0.1";

  makeFlags = "PREFIX=$(out)";

  src = ./.;
  nativeBuildInputs = [ clang pkg-config gdb ];
  buildInputs = [ gtk3 cairo glib libical ];
}
