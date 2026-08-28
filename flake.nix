{
  description = "Rebuild the world... or just the Nix Badge. (+ Milk-V Duo S badgeOS)";
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # badgeOS (Milk-V Duo S) is pinned to nixpkgs 26.05, kept separate from the
    # ESP32 badge's unstable nixpkgs so each half builds against what it expects.
    nixpkgs-2605.url = "github:NixOS/nixpkgs/nixos-26.05";
    nixpkgs-esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";
    flake-parts.url = "github:hercules-ci/flake-parts";
    flake-compat.url = "https://flakehub.com/f/edolstra/flake-compat/1.tar.gz";
    flakever.url = "github:numinit/flakever";
  };

  outputs =
    inputs@{
      self,
      flake-parts,
      flakever,
      nixpkgs-esp-dev,
      nixpkgs-2605,
      ...
    }:
    let
      flakeverConfig = flakever.lib.mkFlakever {
        inherit inputs;

        digits = [
          1
          2
          2
        ];
      };

      # ----------------------------------------------------------------------
      # badgeOS: NixOS for the Milk-V Duo S badge (Sophgo SG2000, dual ARM +
      # RISC-V). Merged in from ~/badgeos; kept on its own nixpkgs 26.05 input.
      # ----------------------------------------------------------------------
      duosLib = nixpkgs-2605.lib;

      # core "arm" -> aarch64, "riscv" -> riscv64. One place names the arch.
      hostPlatformOf = core: if core == "arm" then "aarch64-linux" else "riscv64-linux";

      # buildPlatform lets us cross-compile, the reliable bootstrap path
      # (QEMU native build is flaky). x86_64 and aarch64 are dev-host options.
      buildHosts = [
        "x86_64-linux"
        "aarch64-linux"
        "riscv64-linux"
      ];

      mkDuoS =
        { core, buildPlatform }:
        nixpkgs-2605.lib.nixosSystem {
          modules = [
            {
              nixpkgs.hostPlatform = hostPlatformOf core;
              nixpkgs.buildPlatform = buildPlatform;
            }
            ./modules/duo-s/common.nix
            ./modules/duo-s/base.nix
            ./modules/duo-s/wifi.nix
            ./modules/duo-s/leds.nix
            ./modules/duo-s/core-${core}.nix
          ];
        };

      # ARM core on the VENDOR 5.10 kernel — the display bring-up variant
      # (issue #4). Additive: the mainline arm/riscv configs are untouched.
      # Different module list from mkDuoS because base.nix carries
      # mainline-only kernel patches (see core-arm-vendor.nix header).
      mkDuoSVendor =
        { buildPlatform }:
        nixpkgs-2605.lib.nixosSystem {
          modules = [
            {
              nixpkgs.hostPlatform = "aarch64-linux";
              nixpkgs.buildPlatform = buildPlatform;
            }
            ./modules/duo-s/common.nix
            ./modules/duo-s/leds.nix
            ./modules/duo-s/core-arm-vendor.nix
            ./modules/duo-s/badge-hdmi.nix
          ];
        };

      # All sensible (core, buildPlatform) pairs for one core. We keep the
      # native host plus the practical cross hosts, and drop nonsense like
      # building an aarch64 target from a riscv64 box.
      configsFor =
        core:
        let
          hostPlatform = hostPlatformOf core;
          valid = duosLib.filter (b: b == hostPlatform || b != "riscv64-linux") buildHosts;
        in
        duosLib.mapAttrs' (
          buildPlatform: cfg:
          duosLib.nameValuePair (
            "duo-s-${core}"
            + duosLib.optionalString (
              buildPlatform != hostPlatform
            ) "-${duosLib.head (duosLib.splitString "-" buildPlatform)}"
          ) cfg
        ) (duosLib.genAttrs valid (buildPlatform: mkDuoS { inherit core buildPlatform; }));

      # Build the combined dual-system SD image for a given BUILD host,
      # selecting the cross/native nixos variants whose buildPlatform == that host.
      shortArch = p: duosLib.head (duosLib.splitString "-" p);
      duosBuildSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      sdcardFor =
        buildSystem:
        let
          pkgs = nixpkgs-2605.legacyPackages.${buildSystem};
          armSys =
            self.nixosConfigurations.${
              if buildSystem == "aarch64-linux" then "duo-s-arm" else "duo-s-arm-x86_64"
            };
          riscvSys = self.nixosConfigurations."duo-s-riscv-${shortArch buildSystem}";
          fipArmReal = import ./pkgs/firmware/fip.nix { inherit pkgs; };
          fipRiscvReal = import ./pkgs/firmware/fip.nix {
            inherit pkgs;
            core = "riscv";
          };
          mkCombinedRoot = import ./pkgs/sdcard/make-combined-root.nix { inherit pkgs; };
          mkBoot = import ./pkgs/sdcard/make-boot-dir.nix { inherit pkgs; };
          mkImg = import ./pkgs/sdcard/make-sd-image.nix { inherit pkgs; };
          root = mkCombinedRoot {
            systems = [
              armSys
              riscvSys
            ];
            label = "NIXOS_ROOT";
          };
          # Both ARM and RISC-V now use real firmware.
          bootDir = mkBoot {
            inherit armSys riscvSys;
            fipArm = fipArmReal;
            fipRiscv = fipRiscvReal;
            defaultCore = "arm";
          };
        in
        mkImg { inherit bootDir root; };

      # ARM-only variant of the vendor image: both boot trees carry the vendor
      # ARM system (the riscv64 toggle position will not boot — use the Debian
      # card for riscv). Exists so display-bring-up testing does not gate on
      # cross-building the entire riscv userland. Prefer duo-s-sdcard-vendor
      # (the real combined image) when build resources allow.
      sdcardVendorArmOnlyFor =
        buildSystem:
        let
          pkgs = nixpkgs-2605.legacyPackages.${buildSystem};
          armSys = self.nixosConfigurations."duo-s-arm-vendor-x86_64";
          fipArmReal = import ./pkgs/firmware/fip.nix { inherit pkgs; };
          fipRiscvReal = import ./pkgs/firmware/fip.nix {
            inherit pkgs;
            core = "riscv";
          };
          mkCombinedRoot = import ./pkgs/sdcard/make-combined-root.nix { inherit pkgs; };
          mkBoot = import ./pkgs/sdcard/make-boot-dir.nix { inherit pkgs; };
          mkImg = import ./pkgs/sdcard/make-sd-image.nix { inherit pkgs; };
          root = mkCombinedRoot {
            systems = [ armSys ];
            label = "NIXOS_ROOT";
          };
          bootDir = mkBoot {
            inherit armSys;
            riscvSys = armSys;
            fipArm = fipArmReal;
            fipRiscv = fipRiscvReal;
            defaultCore = "arm";
          };
        in
        mkImg { inherit bootDir root; };

      # Same combined image, but the ARM side runs the VENDOR 5.10 kernel with
      # the display stack (issue #4). RISC-V side unchanged mainline. Only
      # defined for x86_64 build hosts (the only vendor variant we generate).
      sdcardVendorFor =
        buildSystem:
        let
          pkgs = nixpkgs-2605.legacyPackages.${buildSystem};
          armSys = self.nixosConfigurations."duo-s-arm-vendor-x86_64";
          riscvSys = self.nixosConfigurations."duo-s-riscv-${shortArch buildSystem}";
          fipArmReal = import ./pkgs/firmware/fip.nix { inherit pkgs; };
          fipRiscvReal = import ./pkgs/firmware/fip.nix {
            inherit pkgs;
            core = "riscv";
          };
          mkCombinedRoot = import ./pkgs/sdcard/make-combined-root.nix { inherit pkgs; };
          mkBoot = import ./pkgs/sdcard/make-boot-dir.nix { inherit pkgs; };
          mkImg = import ./pkgs/sdcard/make-sd-image.nix { inherit pkgs; };
          root = mkCombinedRoot {
            systems = [
              armSys
              riscvSys
            ];
            label = "NIXOS_ROOT";
          };
          bootDir = mkBoot {
            inherit armSys riscvSys;
            fipArm = fipArmReal;
            fipRiscv = fipRiscvReal;
            defaultCore = "arm";
          };
        in
        mkImg { inherit bootDir root; };

      # Native -> duo-s-<core>; cross -> duo-s-<core>-<buildArch>. So:
      #   duo-s-arm, duo-s-arm-x86_64,
      #   duo-s-riscv, duo-s-riscv-x86_64, duo-s-riscv-aarch64
      duosNixosConfigurations = duosLib.mergeAttrsList (
        map configsFor [
          "arm"
          "riscv"
        ]
      );
    in
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [
        inputs.flake-parts.flakeModules.easyOverlay
      ];

      flake = {
        versionTemplate = "2.0-<lastModifiedDate>-<rev>";

        # badgeOS NixOS systems (built against nixpkgs 26.05).
        nixosConfigurations = duosNixosConfigurations // {
          duo-s-arm-vendor-x86_64 = mkDuoSVendor { buildPlatform = "x86_64-linux"; };
        };
      };

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      perSystem =
        {
          config,
          system,
          inputs',
          pkgs,
          final,
          ...
        }:
        {
          _module.args.pkgs = import inputs.nixpkgs {
            inherit system;
            overlays = [
              nixpkgs-esp-dev.overlays.default
              self.overlays.default
            ];
            config = {
              permittedInsecurePackages = [
                "python3.13-ecdsa-0.19.1"
              ];
            };
          };

          overlayAttrs = {
            nixbadge = pkgs.callPackage ./pkgs/idf/nixbadge rec {
              target = "esp32c6";
              esp-idf = pkgs."esp-idf-${target}";
            };
            flakever = flakeverConfig;
          }
          # badgeOS combined dual-core SD image (nixpkgs 26.05), on the hosts
          # that can build it.
          // duosLib.optionalAttrs (duosLib.elem system duosBuildSystems) {
            duo-s-sdcard = sdcardFor system;
          }
          // duosLib.optionalAttrs (system == "x86_64-linux") {
            duo-s-sdcard-vendor = sdcardVendorFor system;
            duo-s-sdcard-vendor-armonly = sdcardVendorArmOnlyFor system;
          };

          packages = {
            default = pkgs.duo-s-sdcard;
            v1 = pkgs.nixbadge-v1;
          }
          // duosLib.optionalAttrs (system == "x86_64-linux") {
            duo-s-sdcard-vendor = pkgs.duo-s-sdcard-vendor;
            duo-s-sdcard-vendor-armonly = pkgs.duo-s-sdcard-vendor-armonly;
          };

          devShells = {
            v1 = pkgs.nixbadge-v1.shell;

            # HDMI bring-up rig (issue #4): laptop-side tooling for driving the
            # badge over ssh and reading the HDMI output back through a USB
            # capture dongle. See tools/hdmi-rig/README.md.
            hdmi-rig = pkgs.mkShell {
              packages = with pkgs; [
                sshpass
                openssh
                ffmpeg
                v4l-utils
                i2c-tools
                (python3.withPackages (ps: with ps; [ numpy pillow ]))
              ];
              shellHook = ''
                export PATH="$PWD/tools/hdmi-rig:$PATH"
                echo "hdmi-rig: badge-run.sh / recipe-run.sh / frame-grab.sh on PATH (tools/hdmi-rig/README.md)"
              '';
            };
          };
        };
    };
}
