{ pkgs ? import <nixpkgs> {} }:
with pkgs;
stdenv.mkDerivation {
  pname = "viscal";
  version = "0.0.1";

  makeFlags = "PREFIX=$(out)";

  src = ./.;
  nativeBuildInputs = [ clang pkgconfig ];
  buildInputs = [ gtk3 cairo glib libical ];
}
