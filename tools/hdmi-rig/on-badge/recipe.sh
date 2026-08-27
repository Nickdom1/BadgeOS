#!/bin/sh
# ===== Badge 2 HDMI — rapid-fire recipe cycler =====
# SG2000 / Milk-V Duo S, vendor riscv image. DSI -> LT8912B -> HDMI.
# Lives persistently at /home/debian/badgehdmi/recipe.sh (survives reboot).
# Streams live beside it. Runtime scratch (fifo/logs) stays in /tmp.
#
# Usage: sudo bash /home/debian/badgehdmi/recipe.sh <0|1|2|4|5|6|7|8|9>
#   0 = OFF/blank
#   1 = landscape stream in portrait layer  (mismatch baseline: band-at-top)
#   2 = framebuffer landscape                (fb does NOT composite -> white)
#   3 = DISABLED (sample_vio 50 wedges the SoC — do not run)
#   4 = landscape WHITE baseline             (720p sync anchor; OUT-OF-RANGE w/o producer)
#   5 = PORTRAIT bars, matched 720x1280      (expect FULL-SCREEN color bars)
#   6 = PORTRAIT dynamic, matched 720x1280   (expect full-screen motion)
#   7 = LANDSCAPE bars: 1280x720 producer + landscape disp override (continuous)
#   8 = LANDSCAPE dynamic: 1280x720 motion producer + landscape disp override
#   9 = LANDSCAPE from portrait stream scanned as landscape (repro of the 1st good frame)
#  10 = LANDSCAPE hardware COLORBAR via VO_IOCTL_PATTERN (driver pattern-gen, NO producer)
#  11 = LANDSCAPE hardware pattern, colour-selectable: recipe.sh 11 <patidx 0..9>
set +e
DIR=/home/debian/badgehdmi
cd /mnt/system/usr/bin; export LD_LIBRARY_PATH=/mnt/system/usr/lib
B=2; M=0x48; D=0x49; w(){ i2cset -y $B $1 $2 $3 >/dev/null 2>&1; }
pinmux(){ cvi-pinmux -w IIC2_SCL/IIC2_SCL >/dev/null 2>&1; cvi-pinmux -w IIC2_SDA/IIC2_SDA >/dev/null 2>&1; }

lt_common(){ # base regs shared by both orientations
  for kv in 08:ff 09:ff 0a:ff 0b:7c 0c:ff 42:04 31:b1 32:b1 33:0e 37:00 38:22 60:82 39:45 3a:00 3b:00 44:31 55:44 57:01 5a:02 3e:d6 3f:d4 41:3c b2:00; do w $M 0x${kv%:*} 0x${kv#*:}; done
  w $D 0x13 0x00
  for kv in 12:04 14:00 15:00 1a:03 1b:03; do w $D 0x${kv%:*} 0x${kv#*:}; done
}
lt_dds(){ for kv in 4e:ff 4f:56 50:69 51:80 1f:5e 20:01 21:2c 22:01 23:fa 24:00 25:c8 26:00 27:5e 28:01 29:2c 2a:01 2b:fa 2c:00 2d:c8 2e:00 42:64 43:00 44:04 45:00 46:59 47:00 48:f2 49:06 4a:00 4b:72 4c:45 4d:00 52:08 53:00 54:b2 55:00 56:e4 57:0d 58:00 59:e4 5a:8a 5b:00 5c:34 1e:4f 51:00; do w $D 0x${kv%:*} 0x${kv#*:}; done; }
lt_latch(){ w $M 0xab 0x03; w $M 0xb2 0x01; lt_dds; w $M 0x03 0x7f; sleep 0.02; w $M 0x03 0xff; }

lt_landscape(){ pinmux; lt_common   # 1280x720@60
  for kv in 10:01 11:08 18:28 19:05 1c:00 1d:05 2f:0c 34:72 35:06 36:ee 37:02 38:14 39:00 3a:05 3b:00 3c:dc 3d:00 3e:6e 3f:00; do w $D 0x${kv%:*} 0x${kv#*:}; done
  lt_latch; sleep 0.3; echo -n "HPD 0xc1: "; i2cget -y $B $M 0xc1; }

lt_portrait(){ pinmux; lt_common    # 720x1280@60
  for kv in 10:01 11:08 18:40 19:10 1c:d0 1d:02 2f:0c 34:b4 35:03 36:1a 37:05 38:04 39:00 3a:06 3b:00 3c:24 3d:00 3e:80 3f:00; do w $D 0x${kv%:*} 0x${kv#*:}; done
  lt_latch; sleep 0.3; echo -n "HPD 0xc1: "; i2cget -y $B $M 0xc1; }

dsi_up(){ lsmod | grep -q soph_mipi_tx || insmod /mnt/system/ko/soph_mipi_tx.ko
  # LANEID overridable for data-lane de-interleave debugging (default = clock@phys2, D0-3@phys0,1,3,4).
  timeout 8 ./sample_dsi --laneid=${LANEID:-1,2,0,3,4} ${PNSWAP:+--pnswap=$PNSWAP} --panel=LT9611_1280x720_60HZ >/tmp/dsi.log 2>&1; }

start_bars(){ # $1 = stream file; vdecvo producer, held open, CLEAN-stoppable
  STREAM="${1:-$DIR/bars720.264}"
  rm -f /tmp/vfifo; mkfifo /tmp/vfifo
  ( sleep 100000 > /tmp/vfifo ) &          # holder keeps stdin open
  setsid ./sample_vdecvo 3 "$STREAM" "$STREAM" < /tmp/vfifo > /tmp/vd.log 2>&1 &
  sleep 6; }

reset(){ echo "[reset]"
  # register oracle: /proc/dispdump shows the disp block's WORKING bank (hw truth)
  # vs SHADOW bank (sw belief). Read-only. $DIR/oracle.sh = summary + ADDR motion.
  lsmod | grep -q dispdump || insmod $DIR/dispdump.ko 2>/dev/null
  # turn OFF the scaler pattern generator (recipe 10/11) — else its color bars paint
  # OVER any producer's layer and you see bars instead of video.
  python3 $DIR/vopat.py 0 >/dev/null 2>&1
  # clean producer exit -> frees VB; timeout-bounded so a readerless FIFO can't hang us.
  # Both sample_vdecvo and badge_landscape_vo2 read stdin (getchar) from the FIFO — a newline
  # makes them exit cleanly (unbind + VB destroy), avoiding the kill-9 VB-leak dead-end.
  if { pgrep -f sample_vdecvo >/dev/null 2>&1 || pgrep -f badge_landscape_vo2 >/dev/null 2>&1; } && [ -p /tmp/vfifo ]; then
    timeout 3 sh -c 'echo x > /tmp/vfifo' 2>/dev/null; sleep 2
  fi
  rm -f /tmp/vfifo 2>/dev/null            # never leave a stale FIFO node behind
  pkill -9 -f 'sleep 100000' 2>/dev/null
  pkill -9 -f sample_vdecvo 2>/dev/null
  pkill -9 -f badge_landscape_vo2 2>/dev/null
  pkill -f FBHOLD 2>/dev/null; systemctl stop fbhold 2>/dev/null; systemctl reset-failed fbhold 2>/dev/null
  sleep 1; }

case "$1" in
 0) reset; w $M 0xab 0x00; echo "RECIPE 0: OFF/blank (producers stopped)";;
 1) reset; echo "RECIPE 1: landscape stream in portrait layer (mismatch baseline)."
    dsi_up; start_bars $DIR/bars720.264; lt_portrait
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    grep -iE 'Fail|abnormal|NG' /tmp/vd.log | head -1;;
 2) reset; echo "RECIPE 2: FRAMEBUFFER landscape (fb does not composite -> expect white)."
    dsi_up
    lsmod | grep -q soph_fb || insmod /mnt/system/ko/soph_fb.ko mode_option=1280x720-32@60 vxres=1280 vyres=720
    sleep 0.3
    systemd-run --unit=fbhold --collect sh -c 'exec 3<>/dev/fb0; sleep infinity' >/dev/null 2>&1
    sleep 0.4
    python3 - <<'PY' 2>/dev/null
import os
W,H=1280,720
bars=[(255,255,255),(0,255,255),(255,255,0),(0,255,0),(255,0,255),(0,0,255),(255,0,0),(0,0,0)]
row=bytearray()
for x in range(W):
    b,g,r=bars[x*len(bars)//W]; row+=bytes((b,g,r,255))
buf=bytes(row)*H
fd=os.open('/dev/fb0',os.O_RDWR); os.write(fd,buf); os.close(fd)
PY
    lt_landscape
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p';;
 3) echo "RECIPE 3: DISABLED — sample_vio 50 wedges the SoC (needs power-cycle). Skipping.";;
 4) reset; echo "RECIPE 4: landscape WHITE baseline (clean 720p sync)."
    dsi_up; lt_landscape
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p';;
 5) reset; echo "RECIPE 5: PORTRAIT bars, MATCHED 720x1280 stream. Expect FULL-SCREEN color bars."
    dsi_up; start_bars $DIR/bars_p.264; lt_portrait
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    grep -iE 'Fail|abnormal|NG' /tmp/vd.log | head -1;;
 6) reset; echo "RECIPE 6: PORTRAIT dynamic (moving), MATCHED 720x1280. Expect full-screen motion."
    dsi_up; start_bars $DIR/dyn_p.264; lt_portrait
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    grep -iE 'Fail|abnormal|NG' /tmp/vd.log | head -1;;
 7) reset; echo "RECIPE 7: LANDSCAPE bars. 1280x720 producer, then re-assert landscape disp. Expect FULL-SCREEN landscape bars."
    # producer loads 1280x720 content (also flips disp to portrait via enIntfSync=21);
    # second dsi_up re-asserts landscape 1280x720 disp timing while producer keeps feeding VB.
    dsi_up; start_bars $DIR/bars720.264; dsi_up; lt_landscape
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    grep -iE 'Fail|abnormal|NG' /tmp/vd.log | head -1;;
 8) reset; echo "RECIPE 8: LANDSCAPE dynamic (moving). 1280x720 motion producer + landscape disp override."
    dsi_up; start_bars $DIR/dyn720.264; dsi_up; lt_landscape
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    grep -iE 'Fail|abnormal|NG' /tmp/vd.log | head -1;;
 9) reset; echo "RECIPE 9: LANDSCAPE from PORTRAIT stream (repro of first good frame). Expect vertical columns, landscape."
    dsi_up; start_bars $DIR/bars_p.264; dsi_up; lt_landscape
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    grep -iE 'Fail|abnormal|NG' /tmp/vd.log | head -1;;
 10) reset; echo "RECIPE 10: LANDSCAPE hardware COLORBAR (VO_IOCTL_PATTERN, no producer)."
    dsi_up                                   # landscape 1280x720 disp timing (tgen on)
    python3 $DIR/vopat.py 6                   # scaler color-bar pattern -> fills active region
    lt_landscape                             # LT8912B landscape locks to it
    echo -n "VO_DISP: "; cat /proc/cvitek/vo_disp 2>/dev/null | grep -iE 'bw fail|hde|vde' | tr '\n' ' '; echo;;
 11) reset; PAT="${2:-6}"; echo "RECIPE 11: LANDSCAPE hardware pattern idx=$PAT (0=off 3=R 4=G 5=B 6=bars 7=Hgrad 8=Vgrad)."
    dsi_up
    python3 $DIR/vopat.py "$PAT"
    lt_landscape
    echo -n "VO_DISP: "; cat /proc/cvitek/vo_disp 2>/dev/null | grep -iE 'bw fail|hde|vde' | tr '\n' ' '; echo;;
 12) reset; VID="${2:-$DIR/badapple_p.264}"; echo "RECIPE 12: PORTRAIT real VIDEO FILE -> HDMI ($VID). Bad Apple!!"
    dsi_up; start_bars "$VID"; lt_portrait
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    grep -iE 'Fail|abnormal|NG' /tmp/vd.log | head -1;;
 13) reset; VID="${2:-$DIR/badapple_l.264}"; echo "RECIPE 13: LANDSCAPE real VIDEO via VPSS rotate (case 1) ($VID)."
    # case 1 = VDEC -> VPSS -> VO. VPSS can rotate/scale. Still fights enIntfSync=21;
    # experimental landscape-video attempt. Producer held open, clean-stoppable.
    rm -f /tmp/vfifo; mkfifo /tmp/vfifo; ( sleep 100000 > /tmp/vfifo ) &
    dsi_up
    setsid ./sample_vdecvo 1 "$VID" "$VID" < /tmp/vfifo > /tmp/vd.log 2>&1 &
    sleep 6; dsi_up; lt_landscape
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    grep -iE 'Fail|abnormal|NG|enIntfSync' /tmp/vd.log | head -2;;
 14) reset; VID="${2:-$DIR/dyn_p.264}"; echo "RECIPE 14: PORTRAIT video via VDEC->VPSS->VO (case 1). Tests disp_from_sc/online routing ($VID)."
    rm -f /tmp/vfifo; mkfifo /tmp/vfifo; ( sleep 100000 > /tmp/vfifo ) &
    dsi_up
    setsid ./sample_vdecvo 1 "$VID" "$VID" < /tmp/vfifo > /tmp/vd.log 2>&1 &
    sleep 6; lt_portrait
    python3 $DIR/vosdk.py show >/dev/null 2>&1
    echo -n "disp_from_sc: "; cat /proc/cvitek/vo_disp 2>/dev/null | grep -o 'disp_from_sc([0-9])'
    grep -iE 'Fail|abnormal|NG|not support' /tmp/vd.log | head -2;;
 15) reset; VID="${2:-$DIR/badapple_l.264}"; echo "RECIPE 15: LANDSCAPE video via CUSTOM VO player (VDEC->VPSS->VO, 720P60 landscape, ROT_0) ($VID)."
    # badge_landscape_vo = cvi_mpi sample_vdecvo case 0, patched: enIntfSync=720P60, VO 1280x720, ROTATION_0.
    # Native landscape producer (NOT sample_vdecvo's hardcoded portrait). Held open via FIFO, clean-stoppable.
    rm -f /tmp/vfifo; mkfifo /tmp/vfifo; ( sleep 100000 > /tmp/vfifo ) &
    dsi_up
    LD_LIBRARY_PATH=/mnt/system/usr/lib setsid $DIR/badge_landscape_vo2 "$VID" 1280x720 < /tmp/vfifo > /tmp/blv.log 2>&1 &
    sleep 7; lt_landscape
    echo -n "VO: "; cat /proc/cvitek/vo 2>/dev/null | sed -n '4p'
    echo "--- player log ---"; tail -20 /tmp/blv.log
    echo "--- disp bw / set_rect ---"; dmesg | grep -iE 'set_rect|bw fail|disp bw|waitq|out of range' | tail -6;;
 16) reset; export LANEID="${2:-1,2,0,3,4}"; PAT="${3:-3}"; export PNSWAP="$4"
    echo "RECIPE 16: DATA-LANE de-interleave test. laneid=$LANEID pnswap=${PNSWAP:-none} pattern=$PAT (3=RED)."
    echo "  Expect: SOLID colour if lane order correct; fine vertical stripes if wrong."
    dsi_up
    python3 $DIR/vopat.py "$PAT"
    lt_landscape
    echo "--- DSI TX state ---"; grep -A6 'DSI TX' /proc/dispdump 2>/dev/null;;
 *) echo "usage: recipe.sh <0|1|2|4|5|6|7|8|9|10|11|12|13|14|15> | 16 <laneid> [pat] [pnswap]  (3 is disabled)";;
esac
echo "[done recipe $1]"
