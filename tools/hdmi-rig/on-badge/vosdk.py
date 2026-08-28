#!/usr/bin/env python3
# Drive the vendor VO driver's SDK-CTRL ioctl directly (safe, driver-mediated — NOT /dev/mem).
# Lets us call CVI_VO SDK ops the vendor sample forgot/refused to, e.g. force a hidden
# channel to SHOW, clear a stray channel rotation, or (later) re-time the device.
#
#   vosdk.py show            -> VO_SDK_SHOW_CHN(layer0,chn0)     (composite the channel)
#   vosdk.py rot <0|90|180|270> -> VO_SDK_SET_CHNROTATION
#   vosdk.py hide            -> VO_SDK_HIDE_CHN
#
# ioctl: VO_IOC_S_CTRL = _IOWR('o',0x21, vo_ext_control[24]) = 0xC0186F21
#   vo_ext_control { u32 id; u32 sdk_id; u32 size; u32 reserved; union{...; void*ptr;} @16 }
#   id = VO_IOCTL_SDK_CTRL(30); sdk_id = VO_SDK_* op; ptr -> cfg struct (copy_from_user'd).
import ctypes, fcntl, os, struct, sys

VO_IOC_S_CTRL     = 0xC0186F21
VO_IOCTL_SDK_CTRL = 30
# VO_SDK_CTRL enum (from vo_uapi.h)
SHOW_CHN = 12
HIDE_CHN = 15
SET_CHNROTATION = 26
# ROTATION_E
ROT = {"0": 0, "90": 1, "180": 2, "270": 3}

fd = os.open('/dev/cvi-vo', os.O_RDWR)

def s_ctrl(sdk_id, payload):
    buf = ctypes.create_string_buffer(bytes(payload) + b'\0' * 16, 32)  # over-allocate
    ptr = ctypes.addressof(buf)
    ext = struct.pack('<IIIIQ', VO_IOCTL_SDK_CTRL, sdk_id, len(payload), 0, ptr)
    fcntl.ioctl(fd, VO_IOC_S_CTRL, ext)

VO_IOCTL_ONLINE = 7  # -> sclr_ctrl_set_disp_src(value): routes disp source (disp_from_sc)

op = sys.argv[1] if len(sys.argv) > 1 else 'show'
if op == 'online':
    v = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    ext = struct.pack('<IIIIq', VO_IOCTL_ONLINE, 0, 0, 0, v)  # id, sdk_id, size, resv, value
    fcntl.ioctl(fd, VO_IOC_S_CTRL, ext)
    print(f'vosdk: ONLINE(disp_src={v}) OK')
elif op == 'show':
    s_ctrl(SHOW_CHN, struct.pack('<BB', 0, 0))            # vo_chn_cfg
    print('vosdk: SHOW_CHN(0,0) OK')
elif op == 'hide':
    s_ctrl(HIDE_CHN, struct.pack('<BB', 0, 0))
    print('vosdk: HIDE_CHN(0,0) OK')
elif op == 'rot':
    r = ROT[sys.argv[2]]
    s_ctrl(SET_CHNROTATION, struct.pack('<BBxxI', 0, 0, r))  # vo_chn_rotation_cfg
    print(f'vosdk: SET_CHNROTATION(0,0,{sys.argv[2]}) OK')
else:
    print('usage: vosdk.py show|hide|rot <0|90|180|270>|online [src]'); sys.exit(2)
