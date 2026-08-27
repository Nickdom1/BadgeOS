# BadgeOS — assistant notes

NixOS for the Nix Badge 2.0 (Sophgo SG2000 / Milk-V Duo S, dual-arch
arm64+riscv64). Flake-parts layout: `modules/duo-s/` NixOS modules,
`pkgs/firmware/` FSBL/U-Boot/DTS, `pkgs/kernel/duos-vendor/` nixified vendor
5.10 kernel, `pkgs/wifi/aic8800.nix` = the house pattern for packaging
out-of-tree vendor kernel modules (patch series + provenance comments +
experiment toggles).

House style: commits are single-purpose with imperative subjects; register
sequences and DTS nodes carry provenance comments citing the exact vendor
file/function they came from; DTS doubles as wiring documentation
(`gpio-line-names`). AI-assisted commits carry an attribution trailer.

Hard constraints:
- Badges are CTF prizes: **never flash eMMC**; SD boot + insmod only.
- On-badge userspace MMIO writes wedge the SoC (reads are safe).

Active work: HDMI bring-up (issue #4) — state and evidence in
`docs/hdmi-bringup.md`, test rig in `tools/hdmi-rig/`.
