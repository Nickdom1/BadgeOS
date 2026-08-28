#!/usr/bin/env python3
# Badge 2 HDMI — trigger the VO scaler test-pattern via the driver (safe, no /dev/mem).
# Usage: vopat.py [pat]   patterns[] index in vo.c:
#   0=OFF 1=SNOW 2=AUTO 3=RED 4=GREEN 5=BLUE 6=COLORBAR 7=Hgrad 8=Vgrad 9=USR(black)
# Driver: /dev/cvi-vo  VO_IOC_S_CTRL = _IOWR('o',0x21,struct vo_ext_control[24])
#         struct vo_ext_control { u32 id, sdk_id, size, reserved; union{s32 value; s64; ptr;} }
#         id = VO_IOCTL_PATTERN = 4 ; value = pattern index
import fcntl, struct, os, sys
pat = int(sys.argv[1]) if len(sys.argv) > 1 else 6
VO_IOC_S_CTRL = 0xC0186F21
fd = os.open('/dev/cvi-vo', os.O_RDWR)
buf = struct.pack('<IIIIq', 4, 0, 0, 0, pat)   # id=4(PATTERN), value(s64-slot)=pat
try:
    fcntl.ioctl(fd, VO_IOC_S_CTRL, buf)
    print("vopat: pattern=%d -> OK" % pat)
    rc = 0
except OSError as e:
    print("vopat: pattern=%d -> ioctl FAILED: %s" % (pat, e))
    rc = 1
finally:
    os.close(fd)
sys.exit(rc)
