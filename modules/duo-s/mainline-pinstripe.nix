# Selected explicitly by duo-s-riscv-pinstripe-<builder>; ordinary images omit it.
{
  config,
  pkgs,
  lib,
  ...
}:
let
  kernel = config.boot.kernelPackages.kernel;
  driver = import ../../pkgs/video/sophgo-dsi { inherit pkgs kernel; };
in
{
  assertions = [
    {
      assertion = pkgs.stdenv.hostPlatform.system == "riscv64-linux";
      message = "The manual pinstripe image is RISC-V only.";
    }
  ];
  hardware.deviceTree.package = lib.mkOverride 40 (
    import ../../pkgs/firmware/duos-riscv-dtb.nix {
      inherit pkgs kernel;
      pinstripe = true;
    }
  );
  boot.kernelPatches = lib.mkMerge [
    (lib.mkBefore [
      {
        name = "lt8912b-lifecycle";
        patch = ../../pkgs/kernel/patches/lt8912b-lifecycle.patch;
      }
      {
        name = "cv18xx-pll";
        patch = ../../pkgs/kernel/patches/cv18xx-pll.patch;
      }
    ])
    [
      {
        name = "manual-pinstripe-dependencies";
        patch = null;
        structuredExtraConfig = with lib.kernel; {
          DRM = lib.mkForce yes;
          DRM_KMS_HELPER = lib.mkForce yes;
          DRM_GEM_DMA_HELPER = lib.mkForce yes;
          DRM_MIPI_DSI = lib.mkForce yes;
          DRM_LONTIUM_LT8912B = lib.mkForce module;
          DRM_DISPLAY_CONNECTOR = lib.mkForce module;
          I2C_CHARDEV = lib.mkForce module;
          REGULATOR_FIXED_VOLTAGE = lib.mkForce yes;
        };
      }
    ]
  ];
  # A terminal stop fault must not become a timed reboot on this test image.
  systemd.settings.Manager.RuntimeWatchdogSec = lib.mkForce "0";
  systemd.settings.Manager.RebootWatchdogSec = lib.mkForce "0";
  boot.kernelParams = lib.mkAfter [ "panic=0" ];
  boot.extraModulePackages = [ driver ];
  # Probe only: no reset pulse, pattern or display clock programming on boot.
  boot.kernelModules = [
    "sophgo-dsi"
    "i2c-dev"
  ];
  # Reproduce the measured milestone's diagnostic options. These only enable
  # fenced snapshots/trace; display programming still requires manual start.
  boot.extraModprobeConfig = ''
    options sophgo-dsi pinstripe_trace=1 pinstripe_snapshot=1 pinstripe_dphy=1 mac_fire_and_forget=1 vip_bt_clock=1
  '';
  environment.systemPackages = [
    pkgs.i2c-tools
    pkgs.libdrm.bin
    pkgs.dtc
    pkgs.kmod
    (pkgs.writeShellApplication {
      name = "mainline-pinstripe";
      runtimeInputs = [ pkgs.coreutils ];
      text = builtins.readFile ../../tools/display/mainline-pinstripe.sh;
    })
  ];
  system.build.pinstripeDriver = driver;
  environment.etc."mainline-pinstripe-source".text = ''
    Test only; manually run mainline-pinstripe start|status|stop.
    Kernel: ${kernel.version}; module: ${driver.version}
    PCB: d97cfc1d2f50a0447eaf80c7d025e58900b8f5e6
    Record the clean Git commit and artifact hashes before a hardware test.
  '';
}
