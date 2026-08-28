# HDMI display stack for the vendor-kernel ARM variant (issue #4).
#
# Ports the hardware-verified golden recipe (tools/hdmi-rig/on-badge/recipe.sh
# recipe 11, validated on the Debian riscv rig — see docs/hdmi-bringup.md and
# the archive GOLDEN-first-hdmi-out.md) onto NixOS:
#
#   modules (sys->base->vpss->vo/mipi_tx, dep-ordered by modprobe)
#     -> dsi-up            (DSI link, 720p60 RGB888, badge lane map 1,2,0,3,4)
#     -> vopat.py 6        (VO hardware colorbar pattern, no producer needed)
#     -> LT8912B I2C init  (verbatim register sequence from recipe.sh
#                           lt_landscape; origin: mainline lontium-lt8912b.c)
#
# The DTS (pkgs/kernel/duos-vendor/dts/board.dts) deletes the pwm/power-ct
# GPIO claims so vo/mipi_tx can't steal the IIC2 pads, but the pads still
# power on at funcsel 3 (PWR_GPIO), so badge-hdmi-up re-asserts funcsel 0
# before the bridge writes — see the fmux block below (hardware-verified
# 2026-08-28: without it the LT8912B never ACKs).
{
  config,
  pkgs,
  lib,
  ...
}:
let
  kernel = config.boot.kernelPackages.kernel;

  sophVo = import ../../pkgs/video/soph-vo.nix { inherit pkgs kernel; };
  dsiUp = import ../../pkgs/video/dsi-up.nix { inherit pkgs; };

  vopat = ../../tools/hdmi-rig/on-badge/vopat.py;
  vosdk = ../../tools/hdmi-rig/on-badge/vosdk.py;

  # LT8912B bring-up, landscape 1280x720p60. Register values are verbatim from
  # tools/hdmi-rig/on-badge/recipe.sh (lt_common/lt_dds/lt_landscape/lt_latch);
  # main page = 0x48, DDS/rx page = 0x49, on the i2c adapter at 4020000 (IIC2).
  badgeHdmiUp = pkgs.writeShellApplication {
    name = "badge-hdmi-up";
    runtimeInputs = [
      pkgs.kmod
      pkgs.i2c-tools
      pkgs.python3
      dsiUp
    ];
    text = ''
      # dep-ordered by depmod; mipi_tx pulls sys/base/vpss/vo
      modprobe cv181x_mipi_tx
      modprobe cv181x_vo 2>/dev/null || true
      modprobe cvi_fb 2>/dev/null || true

      dsi-up

      python3 ${vopat} 6

      # find the IIC2 bus (i2c adapter at soc address 4020000)
      BUS=""
      for d in /sys/class/i2c-adapter/i2c-*; do
        if grep -q 4020000 "$d/name" 2>/dev/null || [[ "$(readlink -f "$d/device")" == *4020000* ]]; then
          BUS="''${d##*-}"
        fi
      done
      if [[ -z "$BUS" ]]; then
        echo "badge-hdmi-up: no i2c adapter at 4020000 (IIC2) — bridge unreachable" >&2
        exit 1
      fi
      echo "badge-hdmi-up: LT8912B on i2c-$BUS"

      # IIC2 pads (porte0/1) power on at funcsel 3 (PWR_GPIO); the DTS
      # /delete-property/ frees them from vo/mipi_tx but nothing muxes them TO
      # I2C, so the LT8912B never ACKs (i2cset -> "Error: Write failed"). Re-assert
      # IIC2 primary function (funcsel 0) — the NixOS analogue of the vendor
      # `cvi-pinmux -w IIC2_SCL/IIC2_SDA`, which the Debian recipe likewise runs
      # from userspace every boot (the MMIO-write wedge hazard is the display
      # blocks; this pinmux block tolerates it). FMUX regs from the vendor header
      # drivers/pinctrl/cvitek/cv181x_reg_fmux_gpio.h (base 0x03001000):
      # IIC2_SCL +0xb8, IIC2_SDA +0xbc, funcsel = low 3 bits. Hardware-verified
      # 2026-08-28: this alone takes the port from no-signal to the baseline
      # pinstripe. Skipped when the pads are already muxed (e.g. once a future
      # DTS i2c2 pinctrl node does this at boot).
      python3 - <<'PY'
import mmap, os, struct
base = 0x03001000
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, offset=base)
for off in (0xb8, 0xbc):  # IIC2_SCL, IIC2_SDA -> funcsel 0 (IIC2)
    v = struct.unpack('<I', m[off:off + 4])[0]
    if v & 0x7:
        m[off:off + 4] = struct.pack('<I', v & ~0x7)
        print('badge-hdmi-up: fmux +0x%x funcsel %d -> 0 (IIC2)' % (off, v & 0x7))
    else:
        print('badge-hdmi-up: fmux +0x%x already IIC2' % off)
PY

      M=0x48 D=0x49
      w() { i2cset -y "$BUS" "$1" "$2" "$3"; }

      # lt_common
      for kv in 08:ff 09:ff 0a:ff 0b:7c 0c:ff 42:04 31:b1 32:b1 33:0e 37:00 38:22 60:82 39:45 3a:00 3b:00 44:31 55:44 57:01 5a:02 3e:d6 3f:d4 41:3c b2:00; do
        w $M "0x''${kv%:*}" "0x''${kv#*:}"
      done
      w $D 0x13 0x00
      for kv in 12:04 14:00 15:00 1a:03 1b:03; do w $D "0x''${kv%:*}" "0x''${kv#*:}"; done

      # lt_landscape timing (1280x720@60)
      for kv in 10:01 11:08 18:28 19:05 1c:00 1d:05 2f:0c 34:72 35:06 36:ee 37:02 38:14 39:00 3a:05 3b:00 3c:dc 3d:00 3e:6e 3f:00; do
        w $D "0x''${kv%:*}" "0x''${kv#*:}"
      done

      # lt_latch: hdmi on + dds + mipi-rx logic reset (must fire AFTER source streams)
      w $M 0xab 0x03
      w $M 0xb2 0x01
      for kv in 4e:ff 4f:56 50:69 51:80 1f:5e 20:01 21:2c 22:01 23:fa 24:00 25:c8 26:00 27:5e 28:01 29:2c 2a:01 2b:fa 2c:00 2d:c8 2e:00 42:64 43:00 44:04 45:00 46:59 47:00 48:f2 49:06 4a:00 4b:72 4c:45 4d:00 52:08 53:00 54:b2 55:00 56:e4 57:0d 58:00 59:e4 5a:8a 5b:00 5c:34 1e:4f 51:00; do
        w $D "0x''${kv%:*}" "0x''${kv#*:}"
      done
      w $M 0x03 0x7f
      sleep 0.1
      w $M 0x03 0xff

      sleep 0.3
      echo -n "badge-hdmi-up: HPD 0xc1 = "; i2cget -y "$BUS" $M 0xc1
      echo "badge-hdmi-up: done (expect colorbar-fed pinstripe on the sink — Debian parity)"
    '';
  };
in
{
  boot.extraModulePackages = [ sophVo ];

  environment.systemPackages = [
    badgeHdmiUp
    dsiUp
    pkgs.python3
  ];

  # Bring the display up at boot so the demo needs no ssh. Failure must never
  # block or fail the boot: the badge is still useful without HDMI. No udev
  # ordering needed: every device node the script touches appears from its own
  # modprobes (devtmpfs) or is always present (/dev/mem, i2c).
  systemd.services.badge-hdmi = {
    description = "Badge HDMI bring-up (vendor display stack, issue #4)";
    wantedBy = [ "multi-user.target" ];
    serviceConfig = {
      Type = "oneshot";
      RemainAfterExit = true;
      TimeoutStartSec = 90;
      ExecStart = lib.getExe badgeHdmiUp;
    };
  };
}
