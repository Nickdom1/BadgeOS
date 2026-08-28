# Vendor / third-party sources — provenance & licensing

Every external tree this flake builds from or borrows knowledge from, with the
exact pin and what we take. House rule: any register sequence, DTS node, or
struct copied from one of these carries a provenance comment at the use site;
this table is the index.

## Build inputs (fetched by derivations, pinned + hashed)

| Repo | Pin | Used by | What we take | License |
|---|---|---|---|---|
| [sophgo/linux_5.10](https://github.com/sophgo/linux_5.10) | `eef0cf7` | `pkgs/kernel/duos-vendor/` | vendor arm64 kernel (in-tree aic8800 wifi, ION, cvitek drivers) | GPL-2.0 |
| [sophgo/osdrv](https://github.com/sophgo/osdrv) branch `sg200x-dev` | `aa542c41` | `pkgs/video/soph-vo.nix` | display interdrv modules (sys/base/vpss/vo/mipi_tx/fb) + uapi headers for the DSI init tool | GPL-2.0 |
| [radxa-pkg/aic8800](https://github.com/radxa-pkg/aic8800) | `bd11969` | `modules/duo-s/wifi.nix`, `core-arm-vendor.nix` | out-of-tree wifi driver (mainline configs); firmware blobs (all configs) | driver GPL-2.0; firmware proprietary AICSemi blobs, redistributed as packaged by radxa |
| [NixOS/nixpkgs](https://github.com/NixOS/nixpkgs) 26.05 + unstable | flake.lock | everything | — | MIT |

## Reference-only (values copied with provenance comments, not build inputs)

| Repo | Pin | What was copied, and to where |
|---|---|---|
| [milkv-duo/duo-buildroot-sdk-v2](https://github.com/milkv-duo/duo-buildroot-sdk-v2) | `6f8962c` | LT9611 720p60 DSI preset values (`cvi_mpi/component/panel/cv181x/dsi_lt9611.h`) → `pkgs/video/dsi-up.c`; full `scaler.c`/`scaler.h` used to decode the DSI MAC registers (session 19, archive `captures/dsi-macdump-s19/`) |
| [milkv-duo/duo-buildroot-sdk](https://github.com/milkv-duo/duo-buildroot-sdk) freertos cv1835 HAL | — | DSI MAC/PHY register names (`scaler_reg.h`, `dsi_phy.c/h`) → the name tables in `tools/hdmi-rig/on-badge/macdump.sh` |
| [scpcom/sophgo-sg200x-debian](https://github.com/scpcom/sophgo-sg200x-debian) | release `v1.9.6` | nothing copied into the flake; its `duos-e_sd` riscv image is the Debian reference rig (`tools/hdmi-rig/`), and its `duos_arm64` defconfig + DTS are archived as the display-config map |
| [torvalds/linux](https://github.com/torvalds/linux) mainline | — | `lontium-lt8912b.c` bridge driver = source of the LT8912B I2C init sequence in `tools/hdmi-rig/on-badge/recipe.sh` (GPL-2.0) |

A local evidence archive (not in this repo) holds frozen copies of the
load-bearing files from each of these at the pins above, so the evidence
chain survives upstream force-pushes.
