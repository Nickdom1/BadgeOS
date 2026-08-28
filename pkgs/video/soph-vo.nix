# CVITEK/Sophgo display stack (VO / DSI host / fbdev) kernel modules for the
# SG2000, built out-of-tree against the vendor 5.10 kernel
# (pkgs/kernel/duos-vendor). Same house pattern as pkgs/wifi/aic8800.nix.
#
# Source: github.com/sophgo/osdrv branch sg200x-dev — the open source of the
# display drivers the Debian rig runs (its /mnt/system/ko soph_* modules are
# scpcom's build of this same tree; ours come out named cv181x_* because
# chip dirs are chip/cv181x). Pinned during the issue-#4 source survey, which
# mapped the modules to their sources:
#   cv181x_vo       interdrv/vo       VO / display timing / scaler
#   cv181x_mipi_tx  interdrv/vo       DSI MAC host (the golden-recipe target)
#   cv181x_vpss     interdrv/vpss     scaler HAL + dsi_phy (sclr_* exports)
#   cv181x_fb       interdrv/fb       fbdev over VO (pixel source holder)
#   + deps sys, base (Module.symvers chain: sys -> base -> vpss -> vo, fb)
#
# The interdrv Makefiles are written to be invoked from inside each module dir
# (M=$(PWD), KBUILD_EXTRA_SYMBOLS=$(PWD)/../<dep>/Module.symvers), so the build
# cd's per dir in dependency order inside one writable tree.
{
  pkgs,
  kernel,
}:
let
  inherit (pkgs.stdenv.hostPlatform) linuxArch;
  crossPrefix = pkgs.stdenv.cc.targetPrefix;

  moduleDirs = [
    "sys"
    "base"
    "vpss"
    "vo"
    "fb"
  ];
in
pkgs.stdenv.mkDerivation {
  pname = "soph-vo";
  version = "sg200x-dev-unstable-2026-08-24-${kernel.version}";

  src = pkgs.fetchFromGitHub {
    owner = "sophgo";
    repo = "osdrv";
    rev = "aa542c41df94f7bc656cb740f6622a5dca7dc403";
    sparseCheckout = [ "interdrv" ];
    hash = "sha256-wANIMD7INpp3VVI2jE3l9AUFsN7d4KafmfM6iyxiNcc=";
  };

  nativeBuildInputs = kernel.moduleBuildDependencies;

  # ccflags-y is appended AFTER KCFLAGS in kbuild's flag order, so the vo
  # Makefile's -Werror overrides our -Wno-error; strip the bare -Werror from
  # the module Makefiles instead (GCC 15 promotes several 5.10-era patterns).
  postPatch = ''
    find interdrv -name 'Makefile*' -print0 | xargs -0 sed -i -E 's/-Werror( |$)/\1/g'
  '';

  # 5.10-era vendor code on a modern GCC: -Wno-error in KCFLAGS backstops the
  # -Werror strip above. Same class of escape hatch as the duos-vendor kernel
  # build itself.
  #
  # The interdrv Makefiles include headers via $(srctree)/drivers/... (ion,
  # pinctrl-cvitek, tee) — but nixpkgs' kernel.dev prunes drivers/ entirely.
  # Point KCFLAGS at the same dirs in the unpruned kernel *source* instead.
  buildPhase = ''
    runHook preBuild
    # generated/compile.h only exists after a full in-tree kernel build; the
    # vendor proc code includes it for the build-info strings. Stub the
    # standard macros.
    STUB="$PWD/stub-include"
    mkdir -p "$STUB/generated"
    cat > "$STUB/generated/compile.h" <<'EOF'
    #define UTS_VERSION "#1 NixOS soph-vo"
    #define UTS_MACHINE "aarch64"
    #define LINUX_COMPILE_BY "nixos"
    #define LINUX_COMPILE_HOST "nixos"
    #define LINUX_COMPILER "gcc"
    EOF
    for d in ${pkgs.lib.concatStringsSep " " moduleDirs}; do
      pushd interdrv/$d
      make \
        KERNEL_DIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
        ARCH=${linuxArch} \
        CROSS_COMPILE=${crossPrefix} \
        CVIARCH=CV181X \
        CVIARCH_L=cv181x \
        KCFLAGS="-Wno-error -I$STUB -I${kernel.src}/drivers/staging/android -I${kernel.src}/drivers/pinctrl/cvitek -I${kernel.src}/drivers/tee" \
        all
      popd
    done
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    instdir="$out/lib/modules/${kernel.modDirVersion}/kernel/drivers/soph-vo"
    mkdir -p "$instdir"
    find interdrv -name '*.ko' -exec install -p -m 0644 {} "$instdir/" \;
    for ko in "$instdir"/*.ko; do
      ${pkgs.buildPackages.removeReferencesTo}/bin/remove-references-to -t ${kernel.dev} "$ko"
    done
    runHook postInstall
  '';

  dontStrip = true;

  meta = {
    description = "CVITEK SG2000 display stack (VO/DSI/fb) out-of-tree kernel modules";
    license = pkgs.lib.licenses.gpl2Only;
  };
}
