# dsi-up: minimal DSI-link bring-up tool for the badge (see dsi-up.c header).
# Compiled against the SAME pinned sophgo/osdrv uapi headers the kernel
# modules build from, so struct layouts cannot drift.
{ pkgs }:
let
  osdrvSrc = pkgs.fetchFromGitHub {
    owner = "sophgo";
    repo = "osdrv";
    rev = "aa542c41df94f7bc656cb740f6622a5dca7dc403";
    sparseCheckout = [ "interdrv" ];
    hash = "sha256-wANIMD7INpp3VVI2jE3l9AUFsN7d4KafmfM6iyxiNcc=";
  };
in
pkgs.stdenv.mkDerivation {
  pname = "dsi-up";
  version = "1.0";

  dontUnpack = true;

  buildPhase = ''
    runHook preBuild
    $CC -O2 -Wall \
      -I ${osdrvSrc}/interdrv/include/common/uapi \
      -o dsi-up ${./dsi-up.c}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -D -m 0755 dsi-up $out/bin/dsi-up
    runHook postInstall
  '';

  meta.description = "SG2000 DSI link bring-up for the Nix Badge 2.0 (golden 720p60 config)";
}
