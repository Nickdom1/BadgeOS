# ARM core, VENDOR 5.10 kernel variant — the display bring-up image (issue #4).
#
# The mainline ARM config (core-arm.nix + base.nix's aarch64 branch) applies
# mainline-source kernel patches (cv18xx-vsw, mmc-no-async-irq) that cannot
# apply to the vendor tree, so this variant deliberately does NOT import
# base.nix; the few arch-agnostic bits it needs are inlined below.
#
# Kernel: pkgs/kernel/duos-vendor — the sophgo linux_5.10 arm64 tree nixified
# for the wifi bring-up (it carries the aic8800 wifi stack IN-tree,
# CONFIG_AIC8800_WLAN_SUPPORT=m, firmware path already pointed at
# /run/current-system/firmware by nixos-extra.config). Its staged DTS chain
# (pkgs/kernel/duos-vendor/dts) retains the full display node set: &vo,
# &mipi_tx, vpss, cvitek-ion + ion_reserved. The display interdrv modules and
# the LT8912B recipe service live in badge-hdmi.nix, not here.
{ pkgs, ... }:
let
  vendorKernel = pkgs.callPackage ../../pkgs/kernel/duos-vendor { };

  # Same pinned aic8800 source as modules/duo-s/wifi.nix — reused here for the
  # FIRMWARE payload only (the vendor kernel's driver is in-tree; building the
  # out-of-tree radxa driver against 5.10 is neither needed nor possible).
  aicSrc = pkgs.fetchFromGitHub {
    owner = "radxa-pkg";
    repo = "aic8800";
    rev = "bd11969265809a0fc948f1107c8256bbb2c1aa60";
    hash = "sha256-7M02L7G/cadlUWF3YG5CAumtmV7RmEbRHWqZQTddRUI=";
  };
  firmware = import ../../pkgs/wifi/aic8800-firmware.nix {
    inherit pkgs;
    src = aicSrc;
  };
in
{
  boot.kernelPackages = pkgs.linuxPackagesFor vendorKernel;

  # Inlined from base.nix's shared section (see header for why not imported).
  boot.kernelParams = [
    "console=ttyS0,115200"
    "earlycon"
  ];
  boot.loader.grub.enable = false;
  boot.loader.generic-extlinux-compatible.enable = true;

  # NixOS's default initrd module list is generic-PC storage (ahci etc.) that
  # the vendor kernel does not build; everything this board boots from is
  # built-in (MMC_SDHCI_CVI/MMC_BLOCK/EXT4/VFAT all =y), so drop the defaults.
  boot.initrd.includeDefaultModules = false;

  # Various NixOS modules still request generic-PC modules (tpm-crb, ...) the
  # slim vendor config never builds; the standard embedded-board escape is to
  # let the module closure skip what the kernel doesn't have.
  nixpkgs.overlays = [
    (final: prev: {
      makeModulesClosure = x: prev.makeModulesClosure (x // { allowMissing = true; });
    })
  ];

  # The vendor kernel builds its own board DTB (postPatch in
  # pkgs/kernel/duos-vendor/default.nix registers it); use it as-is.
  hardware.deviceTree.enable = true;
  hardware.deviceTree.name = "cvitek/sg2000_milkv_duos_glibc_arm64_sd.dtb";

  # In-tree aic8800: firmware package + module autoload. The board switch must
  # be physically set to ARM to boot this core.
  hardware.firmware = [ firmware ];
  boot.kernelModules = [
    "aic8800_bsp"
    "aic8800_fdrv"
  ];
}
