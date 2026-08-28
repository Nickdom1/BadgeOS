#!/bin/sh
# Read-only dump of the CVITEK DSI MAC + DSI PHY blocks via devmem.
# READS are safe on this SoC; userspace WRITES wedge it — never add one here.
#
# Register names come from the vendor cv1835 HAL (scaler_reg.h, dsi_phy.c/h,
# from the milkv-duo/duo-buildroot-sdk freertos cv1835 tree). The HAL
# names stop at MAC+0x24 and PHY+0xC0; a live value at an unnamed offset is
# undocumented hardware state — exactly what the dialect hunt is looking for.
#
# MAC_EN low nibble = enum sclr_dsi_mode {0=IDLE 1=HS(video) 2=ESC 3=SPKT};
# HS_0 = packet header: [7:0] data ID, [23:8] payload (short) / word count.

MAC_BASE=0x0A08A000   # REG_SCL_DSI_BASE (scaler top + 0xA000)
PHY_BASE=0x0A0D1000   # DSI PHY / wrap   (DTS "dphy", len 0x100)

DM=devmem
command -v devmem >/dev/null 2>&1 || DM="busybox devmem"

mac_name() { case "$1" in
  0)  echo MAC_EN;;   4)  echo HS_0;;     8)  echo HS_1;;
  12) echo ESC;;      16) echo ESC_TX0;;  20) echo ESC_TX1;;
  24) echo ESC_TX2;;  28) echo ESC_TX3;;  32) echo ESC_RX0;;
  36) echo ESC_RX1;;  *)  echo "-";;
esac; }

phy_name() { case "$1" in
  0)   echo PHY_EN;;        4)   echo CLK_CFG1;;    8)   echo CLK_CFG2;;
  12)  echo ESC_INIT;;      16)  echo ESC_WAKE;;    20)  echo HS_CFG1;;
  24)  echo HS_CFG2;;       28)  echo CAL_CFG;;     32)  echo CAL_NUM;;
  36)  echo CLK_STATE;;     40)  echo DATA0_STATE;; 44)  echo DATA12_STATE;;
  48)  echo DATA3_STATE;;   56)  echo HS_OV;;       60)  echo HS_SW1;;
  64)  echo HS_SW2;;        68)  echo DATA_OV;;     76)  echo LPTX_LPRX_OV;;
  100) echo PD;;            108) echo TXPLL;;       140) echo REG_8C;;
  144) echo REG_SET;;       156) echo LANE_SEL;;    160) echo LANE_PN_SWAP;;
  180) echo LVDS_EN;;       192) echo EXT_GPIO;;    *)   echo "-";;
esac; }

dump_block() { # $1=label $2=base $3=len $4=name-fn
  echo "== $1 @ $2 =="
  off=0
  while [ "$off" -lt "$3" ]; do
    a=$(printf '0x%08X' $(($2 + off)))
    v=$($DM "$a" 32 2>/dev/null) || v="READ-FAIL"
    printf '  +0x%02X  %-14s %s\n' "$off" "$($4 $off)" "$v"
    off=$((off + 4))
  done
}

dump_block "DSI MAC" $MAC_BASE 256 mac_name
dump_block "DSI PHY" $PHY_BASE 256 phy_name

en=$($DM $MAC_BASE 32 2>/dev/null)
hs0=$($DM 0x0A08A004 32 2>/dev/null)
if [ -n "$en" ] && [ -n "$hs0" ]; then
  case $((en & 0x0f)) in
    0) m=IDLE;; 1) m="HS(video)";; 2) m=ESC;; 3) m=SPKT;; *) m="?";;
  esac
  printf 'decode: MAC_EN=%s mode=%s ; HS_0=%s DI=0x%02X WC=%d\n' \
    "$en" "$m" "$hs0" $((hs0 & 0xff)) $(((hs0 >> 8) & 0xffff))
fi
