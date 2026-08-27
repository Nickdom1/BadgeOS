# hdmi-rig — closed-loop test rig for badge HDMI bring-up (issue #4)

Laptop-side tooling for driving the badge's DSI → LT8912B → HDMI path and
*reading the result back by machine* instead of by eyeball:

```
laptop ──ssh──► badge (vendor Debian image) ──DSI──► LT8912B ──HDMI──► capture
   ▲                                                              dongle (UVC)
   └────────────────────── /dev/videoN ◄──────────────────────────────┘
```

Every experiment becomes: apply config → grab a frame → inspect pixels. That
closes the loop for automated register sweeps.

Enter the shell from the repo root: `nix develop .#hdmi-rig`

## Scripts

| script | runs on | purpose |
|---|---|---|
| `badge-run.sh script.sh` | laptop | run any local script on the badge as root (see the stdin trick in its header) |
| `recipe-run.sh N [arg]` | laptop | fire numbered display recipe N on the badge |
| `frame-grab.sh [out.png]` | laptop | grab one settled frame from the capture dongle |
| `on-badge/recipe.sh` | badge | the recipe library: VO/DSI init, LT8912B I2C init (both orientations), pattern generators, stream players |
| `on-badge/macdump.sh` | badge | read-only dump of the CVITEK DSI MAC block via devmem |

Connection env (defaults are the scpcom image's stock login): `BADGE_HOST`
(192.168.1.228), `BADGE_USER` (debian), `BADGE_PASS` (rv).

## The reference rig

* **Badge**: Nix Badge 2.0 (SG2000 / Milk-V Duo S) booting the
  [scpcom sophgo-sg200x-debian](https://github.com/scpcom/sophgo-sg200x-debian)
  `duos-e_sd` v1.9.6 riscv image (vendor 5.10 kernel) from SD. The vendor
  `sample_dsi` / VO stack and kernel modules ship on that image under
  `/mnt/system`. **Never flash the eMMC — CTF prize board. SD boot + insmod only.**
* **Provisioning the badge**: copy `on-badge/` to `/home/debian/badgehdmi/` on
  the badge (`scp`, or `badge-run.sh` a heredoc). Video test streams
  (`badapple_l.264` / `badapple_p.264`, H.264 Annex-B 1280×720 / 720×1280) go
  beside it; not stored in this repo — ask Nick, or any Annex-B .264 of the
  right geometry works. Content with hard scene structure beats test bars: a
  uniform pattern can false-"work" through a stuck pipeline.
* **Capture dongle**: MS2130-class HDMI→USB3 UVC stick ("Hagibis", USB
  `345f:2130`). Quirks that will bite you:
  * It only asserts HPD / locks TMDS **while a USB capture stream is open**,
    and needs ~2–4 s to lock → `frame-grab.sh` records a clip and keeps a late
    frame (`SETTLE` env, default 5 s).
  * Rapid back-to-back grab cycles make it **drop off USB entirely** (needs a
    physical replug). Space grabs ≥20 s apart in any automated loop and treat
    a vanished `/dev/videoN` as "pause for human", not "retry harder".

## Board facts you need before touching anything

Distilled from bring-up; the full story is in `docs/hdmi-bringup.md`.

* **DSI lane order** (badge wiring {D0,D1,CLK,D2,D3}): the ONLY working
  `sample_dsi` lane map is `--laneid=1,2,0,3,4` — proven by exhaustive
  24-permutation sweep against the capture oracle.
* **Pinmux collision**: vendor `sample_dsi` clobbers the IIC2 pinmux; every
  recipe re-asserts `cvi-pinmux -w IIC2_SCL/IIC2_SCL` (+SDA) before touching
  the bridge. On the vendor image the LT8912B answers on i2c bus 2 at
  0x48/0x49/0x4A.
* **MMIO**: register READS via `devmem` are safe (that's how `macdump.sh`
  works). Userspace MMIO **writes wedge the SoC** — do writes from a kernel
  module or not at all. `sample_vio` also wedges it (recipe 3 is disabled for
  a reason).
