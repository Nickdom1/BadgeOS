# Module-only validation against an explicitly declared, immutable kernel cache.
# No derivation for the installed kernel is requested or rebuilt by this entry.
{ source
, kernelDev ? "/nix/store/r8vyahz0cnsicn0znrjhzj3ajc0j4my6-linux-riscv64-unknown-linux-gnu-7.2.2-dev"
}:
let
  flake = builtins.getFlake source;
  pkgs = flake.nixosConfigurations.duo-s-riscv-pinstripe-x86_64.pkgs;
  build = "${kernelDev}/lib/modules/7.2.2/build";
  expectedConfig = "5abecea7fca10f35109bcb891da704e569f0bea527398b3955f8dfb02ccd07aa";
  expectedSymbols = "eb15d66629333572c58615a304f1a5fbf797654e28c1b0e224dbb8f50379dde8";
  present = builtins.pathExists "${build}/.config" && builtins.pathExists "${build}/Module.symvers";
  matching = pkgs.linux_latest.version == "7.2.2" && present && builtins.hashFile "sha256" "${build}/.config" == expectedConfig
    && builtins.hashFile "sha256" "${build}/Module.symvers" == expectedSymbols
    && builtins.hashFile "sha256" "${kernelDev}/lib/modules/7.2.2/source/include/drm/bridge/lt8912b.h"
      == "d897f4a94ea57fd6ee7c449a49281541dea646cb1f05ab7100c79c091dcd4331";
  kernel = {
    version = "7.2.2";
    modDirVersion = "7.2.2";
    dev = builtins.storePath kernelDev;
    inherit (pkgs.linux_latest) src moduleBuildDependencies;
  };
  bridgeSource = pkgs.buildPackages.runCommand "lt8912b-display-source" {
    nativeBuildInputs = with pkgs.buildPackages; [ gnutar xz patch ];
  } ''
    mkdir source
    tar xf ${kernel.src} --strip-components=1 -C source linux-7.2.2/drivers/gpu/drm/bridge/lontium-lt8912b.c
    patch --batch --fuzz=0 -p1 -d source < ${../kernel/patches/lt8912b-lifecycle.patch}
    mkdir -p "$out/include/drm/bridge"
    cp source/drivers/gpu/drm/bridge/lontium-lt8912b.c "$out/"
    cp source/include/drm/bridge/lt8912b.h "$out/include/drm/bridge/"
    cat > "$out/Makefile" <<'MAKE'
    obj-m += lontium-lt8912b.o
    ccflags-y += -I$(src)/include
    MAKE
  '';
  bridge = pkgs.stdenv.mkDerivation {
    pname = "lt8912b-display";
    version = kernel.version;
    src = bridgeSource;
    nativeBuildInputs = kernel.moduleBuildDependencies;
    makeFlags = [
      "-C" "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
      "M=$(PWD)" "ARCH=${pkgs.stdenv.hostPlatform.linuxArch}"
      "CROSS_COMPILE=${pkgs.stdenv.cc.targetPrefix}"
      "W=1" "KCFLAGS=-Werror" "modules"
    ];
    installPhase = ''
      install -Dm644 lontium-lt8912b.ko "$out/lib/modules/${kernel.modDirVersion}/kernel/drivers/gpu/drm/bridge/lontium-lt8912b.ko"
      ${pkgs.buildPackages.removeReferencesTo}/bin/remove-references-to -t ${kernel.dev} "$out/lib/modules/${kernel.modDirVersion}/kernel/drivers/gpu/drm/bridge/lontium-lt8912b.ko"
    '';
    dontStrip = true;
  };
in
assert pkgs.lib.assertMsg matching "Missing or mismatched configured Linux 7.2.2 cache; refusing an implicit kernel build";
{
  driver = import ../video/sophgo-dsi { inherit pkgs kernel; };
  inherit bridge;
}
