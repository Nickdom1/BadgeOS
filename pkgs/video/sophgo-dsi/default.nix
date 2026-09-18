# Module-only kbuild with target ARCH/CROSS_COMPILE, kernel dependencies,
# pinned vendor source + patch series, and no kernel.dev runtime reference.
{ pkgs, kernel }:
pkgs.stdenv.mkDerivation {
  pname = "sophgo-dsi";
  version = "aa542c41-${kernel.version}";
  src = import ./source.nix { inherit pkgs; };
  nativeBuildInputs = kernel.moduleBuildDependencies;
  makeFlags = [
    "-C"
    "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "M=$(PWD)/interdrv/sophgo-dsi"
    "ARCH=${pkgs.stdenv.hostPlatform.linuxArch}"
    "CROSS_COMPILE=${pkgs.stdenv.cc.targetPrefix}"
    "CONFIG_DRM_SOPHGO_DSI=m"
    "W=1"
    "KCFLAGS=-Werror"
    "modules"
  ];
  preBuild = ''
    test -s ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build/Module.symvers
  '';
  installPhase = ''
    runHook preInstall
    instdir="$out/lib/modules/${kernel.modDirVersion}/kernel/drivers/gpu/drm/sophgo"
    install -Dm644 interdrv/sophgo-dsi/sophgo-dsi.ko "$instdir/sophgo-dsi.ko"
    ${pkgs.buildPackages.removeReferencesTo}/bin/remove-references-to \
      -t ${kernel.dev} "$instdir/sophgo-dsi.ko"
    runHook postInstall
  '';
  dontStrip = true;
  meta = {
    description = "SG2000 DRM and manual golden-pattern MIPI DSI host";
    license = pkgs.lib.licenses.gpl2Only;
  };
}
