# HDMI output

The badge's HDMI port is driven by the SG2000's MIPI DSI host through the
on-board LT8912B DSI-to-HDMI bridge, on both cores. `modules/duo-s/hdmi.nix`
loads the host driver (`pkgs/video/sophgo-dsi`); the bridge is the kernel's own
`lontium-lt8912b` with the patches in `modules/duo-s/base.nix`, and the device
tree graph is in `pkgs/firmware/dts/sg2000-milkv-duo-s-common.dtsi`.

The output is a fixed 1280x720 at 60 Hz in RGB888 over two DSI lanes, from
boot. Programs get a DRM device with the connector `HDMI-A-1` and its fbdev,
`/dev/fb1` (`sophgo-dsidrmfb`). EDID is not read, and hot-plug goes to the
bridge, not to the SoC.

The Linux consoles, with the login on tty1, are on HDMI the right way up
(`fbcon=map:1,rotate:0`); serial stays `/dev/console`. fbcon takes the console
over when the HDMI framebuffer registers, so the Sharp panel shows no console
while HDMI is enabled.

`nixbadge.hdmi.enable = false` in `modules/duo-s/common.nix` does not load the
driver and leaves the consoles on the Sharp panel.
