# A swaybar status command with one tappable block per entry.
{
  lib,
  writeShellApplication,
  sway,
}:
writeShellApplication {
  name = "badge-touch-bar";
  runtimeInputs = [ sway ];
  text = builtins.readFile ./badge-touch-bar.sh;
  meta.description = "Touch launcher for swaybar";
}
