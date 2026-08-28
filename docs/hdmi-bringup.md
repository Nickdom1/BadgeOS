# Badge 2 HDMI bring-up — state of the world

Issue: [#4](https://github.com/NixVegas/BadgeOS/issues/4) · Path under test:
SG2000 VO/DSI (CVITEK MAC, custom IP — not DesignWare) → 4-lane MIPI-DSI →
LT8912B bridge → HDMI connector · Test method: closed-loop capture rig
(`tools/hdmi-rig/`) — badge output is read back through an HDMI-USB dongle, so
every claim below is machine-verified from captured pixels or register reads,
not eyeballs.

Reference stack: scpcom Debian riscv image (vendor 5.10 kernel) from SD,
vendor `sample_dsi` + VO stack, LT8912B driven by I2C from the recipes in
`tools/hdmi-rig/on-badge/`. Strategy is vendor-first: make the vendor stack
show real pixels, then distill the delta into mainline (a `mipi_dsi_host`
derived from vendor `vo_mipi_tx.c`/`dsi_phy.c` + the existing mainline
`lontium-lt8912b` bridge). Mainline has no SG2000 DSI-host driver, so there is
nothing to "just enable" — the vendor path is the source of truth either way.
(Same shape as the wifi fix: vendor kernel as oracle → two distilled mainline
patches.)

## Proven working (with the proof)

| Claim | Evidence |
|---|---|
| I2C2 → LT8912B control bus | i2cdetect ACKs 0x48/0x49/0x4A; chip ID reads 0x12/0xB2 (mainline DTB, hardware-verified — the M1 DTS commit) |
| 720p60 timing reaches the bridge | LT8912B RX counts vtotal = 750 exactly; stable |
| DSI lane order correct | exhaustive 24-permutation sweep; only `--laneid=1,2,0,3,4` yields vtotal 750 |
| SoC display engine + DMA | vendor disp dump shows real decoded frames (Bad Apple) in the framebuffer being scanned out |
| Host emits correct video packets | DSI MAC register dump (`macdump.sh`): MAC in HS-video mode, word-count 3840 = 1280×3 = exact RGB888 |
| LT8912B HDMI-TX half + cable + sink | bridge's internal test-pattern generator (`0x49:0x70=0xc0`) → clean full-screen white through the dongle |

## The wall

With everything above true, the screen shows a **content-invariant 1-px green/
white pinstripe**: red, green, and blue test frames capture byte-identical.
The bridge locks sync but its video FIFO never receives pixel payload — the
video long packets are emitted by the host and never accepted by the bridge's
DSI unpacker. Both endpoints check out; the handoff fails.

## NixOS vendor-ARM parity (2026-08-28)

The whole bring-up now reproduces on the flake's `duo-s-arm-vendor` image (vendor
5.10 arm64 kernel + display stack + `badge-hdmi.service`) — no Debian rig needed.
First boot on hardware exposed the port's one gap: the service reached
DSI-link-up and the VO colorbar, then failed on the *first* LT8912B I2C write —
the bridge never ACKed on IIC2. Root cause: the IIC2 pads (`porte0/1`) power on
at their default mux `funcsel = 3` (PWR_GPIO); the DTS `/delete-property/` frees
them from `vo`/`mipi_tx` but nothing muxes them *to* I2C. Writing `funcsel = 0`
(IIC2 primary — the same mux the M1 mainline commit uses; FMUX regs
`0x030010b8`/`0x030010bc`, base `0x03001000`) makes the bridge appear (chip id
`0x12`/`0xb2`), and the full recipe then runs clean and lands on the **same
starvation pinstripe, pixel-class-identical to the Debian-rig baseline capture**
(mean abs pixel diff 2.3 — capture noise only). So the SG2000 → DSI → LT8912B path
is proven end-to-end on NixOS and hits the identical wall as Debian; Front B
(below) is now the critical path on NixOS too. Fix: `badge-hdmi-up` re-asserts
the IIC2 pinmux before the bridge writes (the NixOS analogue of the vendor
`cvi-pinmux -w IIC2_SCL/IIC2_SDA`).

## Leading theory: DSI dialect mismatch

The mainline LT8912B driver requests `MIPI_DSI_MODE_VIDEO | LPM |
VIDEO_NO_HFP | NO_EOT_PACKET` (sync-**event**, no EoT). The vendor CVITEK MAC,
per its HAL source, appears to emit the opposite dialect (sync-pulse + HSE +
EoTp, LT9611-style) and exposes no mode knob in the registers we've mapped.
No LT8912B driver anywhere (mainline, Espressif, BSP forks) exposes a
bridge-side tolerance bit for this.

Two honest caveats: the MAC's emitted dialect is **inferred from source, not
yet measured**; and the MAC does drive a real DSI panel in vendor products, so
*some* sink accepts its dialect.

## Next moves (in cost order)

1. **Register-doc hunt**: the LT8912B register programming guide (public
   datasheet is pinout-only). Anyone with the doc or a Lontium FAE contact
   short-circuits everything.
2. **Measure the dialect**: extend `macdump.sh` to the full MAC block and map
   it against the vendor HAL headers; find sync-mode/EoTp bits if they exist.
   Writes go through a small kernel module (userspace MMIO writes wedge the
   SoC), buildable against the nixified vendor kernel.
3. **Closed-loop register sweep**: corpus-diff every LT8912B driver variant's
   0x49-page init, then drive curated candidate configs through the capture
   rig automatically (the pinstripe is byte-stable, so "anything else" is
   machine-detectable).
4. **Port the working stack into this flake**: vendor VO/DSI interdrv modules
   packaged like `pkgs/wifi/aic8800.nix`, recipes as a NixOS module — so the
   whole rig is `nix build`-able and the eventual fix lands as one patch file.

## Constraints

* **The badge is a CTF prize — preserve it.** SD boot and insmod are fine;
  never flash the eMMC.
* Userspace MMIO writes to the display blocks (and `sample_vio`) wedge the
  SoC; reads are safe everywhere. The `0x03001000` pinmux block is the proven
  exception: the vendor's own `cvi-pinmux` and our `badge-hdmi-up` write it
  from userspace on every boot.
* Capture dongle quirks are documented in `tools/hdmi-rig/README.md`.
