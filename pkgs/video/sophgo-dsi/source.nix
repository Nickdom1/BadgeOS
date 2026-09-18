# GPL-2.0 vendor HAL pin. The patch adds only the distilled mainline module;
# the vendor VO/VPSS stack is not compiled.
{ pkgs }:
pkgs.applyPatches {
  name = "sophgo-dsi-source";
  src = pkgs.fetchFromGitHub {
    owner = "sophgo";
    repo = "osdrv";
    rev = "aa542c41df94f7bc656cb740f6622a5dca7dc403";
    sparseCheckout = [ "interdrv" ];
    hash = "sha256-wANIMD7INpp3VVI2jE3l9AUFsN7d4KafmfM6iyxiNcc=";
  };
  patches = [ ./0001-sg2000-dsi.patch ];
}
