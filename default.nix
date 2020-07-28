{ pkgs ? import <nixpkgs> {} }:
with pkgs;
stdenv.mkDerivation {
  name = "viscal";
  nativeBuildInputs = [ clang pkgconfig ];
  buildInputs = [ gtk3 cairo glib libical ];
}
