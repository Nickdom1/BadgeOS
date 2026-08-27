#!/bin/sh
# Read-only dump of the CVITEK DSI MAC block via /dev/mem (READS are safe; only writes wedge).
# DSI MAC base = 0x0A08A000. Want the packet-header regs at +0x04 (HS_0) / +0x08 (HS_1) →
# data-type / virtual-channel / word-count of the video packets on the wire.
BASE=0x0A08A000
if command -v devmem >/dev/null 2>&1; then
  echo "using devmem"
  off=0
  while [ $off -lt 64 ]; do
    a=$(printf '0x%08X' $((BASE + off)))
    v=$(devmem $a 32 2>/dev/null)
    printf "  +0x%02x  %s = %s\n" $off $a "$v"
    off=$((off + 4))
  done
else
  echo "no devmem; trying busybox devmem"
  busybox devmem $BASE 32 2>&1
fi
