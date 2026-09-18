# Compiled selected graph, no kernel/image/FIP build.
{ pkgs, kernel }:
let
  dtb = import ./duos-riscv-dtb.nix {
    inherit pkgs kernel;
    pinstripe = true;
  };
in
pkgs.runCommand "duos-riscv-pinstripe-test" { nativeBuildInputs = [ pkgs.dtc ]; } ''
  dt=${dtb}/sophgo/sg2000-milkv-duo-s.dtb
  controller=/soc/i2c@4020000
  pins=/soc/pinctrl@3001000/iic2-cfg
  test "$(fdtget -t s "$dt" /aliases i2c2)" = "$controller"
  test "$(fdtget -t s "$dt" "$controller" status)" = okay
  test "$(fdtget -t u "$dt" "$controller" clock-frequency)" = 100000
  test "$(fdtget -t x "$dt" "$controller" pinctrl-0)" = "$(fdtget -t x "$dt" "$pins" phandle)"
  test "$(fdtget -t x "$dt" "$pins/iic2-scl-pins" pinmux)" = ff000a0c
  test "$(fdtget -t x "$dt" "$pins/iic2-sda-pins" pinmux)" = ff000a0d
  for pad in iic2-scl-pins iic2-sda-pins; do
    test "$(fdtget -t u "$dt" "$pins/$pad" power-source)" = 1800
    ! fdtget -p "$dt" "$pins/$pad" | grep -E '^(bias-|drive-strength)'
  done
  bridge=/soc/i2c@4020000/bridge@48
  host=/soc/dsi@a080000
  for node in "$bridge" "$host" /soc/syscon@a0c8000 /hdmi-connector; do
    test "$(fdtget -t s "$dt" "$node" status)" = okay
  done
  clk=$(fdtget -t u "$dt" /soc/clock-controller@3002000 phandle)
  test "$(fdtget -t u "$dt" /soc/syscon@a0c8000 clocks)" = "$clk 128"
  test "$(fdtget -t s "$dt" "$host" clock-names)" = 'sc_top clk_disp clk_dsi dsi_esc disppll disp_src dsi_src esc_parent cfg_reg'
  test "$(fdtget -t u "$dt" "$host" clocks)" = "$clk 114 $clk 121 $clk 122 $clk 98 $clk 5 $clk 161 $clk 162 $clk 97 $clk 128"
  test "$(fdtget -t u "$dt" "$host" sophgo,pad-roles)" = '1 2 0 3 4'
  test "$(fdtget -t u "$dt" "$host" sophgo,pn-swap-mask)" = 0
  test "$(fdtget -t u "$dt" "$host" sophgo,clock-phase)" = 1
  test "$(fdtget -t u "$dt" "$host/port/endpoint" data-lanes)" = '0 1 2 3'
  test "$(fdtget -t u "$dt" "$bridge/ports/port@0/endpoint" data-lanes)" = '0 1 2 3'
  fdtget -p "$dt" "$host" | grep -qx sophgo,manual-pinstripe
  ! fdtget -p "$dt" "$host" | grep -qx sophgo,desk-only
  ! fdtget -p "$dt" "$bridge" | grep -qx reset-gpios
  fdtget -p "$dt" "$bridge" | grep -qx lontium,rc-reset
  rail=$(fdtget -t x "$dt" /regulator-lt8912-1v8 phandle)
  test "$(fdtget -t u "$dt" /regulator-lt8912-1v8 regulator-min-microvolt)" = 1800000
  fdtget -p "$dt" /regulator-lt8912-1v8 | grep -qx regulator-always-on
  for supply in vdd vccmipirx vccsysclk vcclvdstx vcchdmitx vcclvdspll vcchdmipll; do
    test "$(fdtget -t x "$dt" "$bridge" "$supply-supply")" = "$rail"
  done
  test "$(fdtget -t x "$dt" /hdmi-connector ddc-i2c-bus)" = "$(fdtget -t x "$dt" /soc/i2c@4020000 phandle)"
  test "$(fdtget -t x "$dt" "$host/port/endpoint" remote-endpoint)" = "$(fdtget -t x "$dt" "$bridge/ports/port@0/endpoint" phandle)"
  test "$(fdtget -t x "$dt" "$bridge/ports/port@0/endpoint" remote-endpoint)" = "$(fdtget -t x "$dt" "$host/port/endpoint" phandle)"
  test "$(fdtget -t x "$dt" "$bridge/ports/port@1/endpoint" remote-endpoint)" = "$(fdtget -t x "$dt" /hdmi-connector/port/endpoint phandle)"
  test "$(fdtget -t x "$dt" /hdmi-connector/port/endpoint remote-endpoint)" = "$(fdtget -t x "$dt" "$bridge/ports/port@1/endpoint" phandle)"
  mkdir -p "$out"
  dtc -I dtb -O dts "$dt" > "$out/compiled.dts" 2> "$out/dtc-warnings.txt"
  echo 'PASS: opt-in compiled pinstripe graph, IIC2 J13/J14 at 1.8 V/100 kHz, real RC reset and fixed board rails, shared DDC and reciprocal links' > "$out/result.txt"
''
