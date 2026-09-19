#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Unpack the sophgo-dsi register-snapshot blob into Morgan's dbg-locked layout.

On the badge (root), after a supervised activation:
    cat /sys/kernel/debug/sophgo-dsi/pinstripe-registers > pinstripe-registers.bin
Then here:
    python3 tools/display/pinstripe-registers.py pinstripe-registers.bin OUT_DIR [--golden DBG_LOCKED]
writes OUT_DIR/<n>-<label>/{scaler_top,disp,dsi_mac,vip_sys,clk_ctrl}.hex in
the exact `xxd` default format Morgan's `dd | xxd` produced (offsets restart at
0 for every page), plus summary.txt with the named registers from his script
comments, so `diff -r OUT_DIR/2-complete /tmp/dbg-locked` is meaningful.

Blob version 2 fences every page: only `fence[block]` words were read and
the rest of the page is zero, NOT hardware state. summary.txt states each fence,
and `--golden DIR` prints a word-level comparison against the golden pages that
is restricted to the fenced range (OUT_DIR/golden-diff.txt), naming registers
from the tables below. Version 1 blobs (unfenced) are still accepted.

The blob layout is fixed by sophgo-dump.h. Nothing here touches hardware; the
driver captures only while it owns the clocks.
"""
import json
import re
import struct
import sys
from pathlib import Path

MAGIC = 0x50444753
NAMED = {
    'scaler_top': {0x00: 'TOP_CFG0', 0x04: 'TOP_CFG1', 0x08: 'TOP_AXI', 0x10: 'TOP_SHD',
                   0x30: 'TOP_INTR_MASK', 0x4c: 'PG', 0x70: 'VO_MUX'},
    'disp': {0x00: 'DISP_CFG', 0x04: 'DISP_TOTAL', 0x08: 'DISP_VSYNC', 0x0c: 'DISP_VFDE',
             0x10: 'DISP_VMDE', 0x14: 'DISP_HSYNC', 0x18: 'DISP_HFDE', 0x1c: 'DISP_HMDE',
             0x30: 'DISP_FIFO_THR',
             0x5c: 'OUT_CSC_C00_01', 0x60: 'OUT_CSC_C02_10', 0x64: 'OUT_CSC_C11_12',
             0x68: 'OUT_CSC_C20_21', 0x78: 'IN_CSC_C00_01', 0x7c: 'IN_CSC_C02_10',
             0x80: 'IN_CSC_C11_12', 0x84: 'IN_CSC_C20_21',
             0x94: 'PAT_CFG', 0x98: 'PAT_COLOR0', 0x9c: 'PAT_COLOR1', 0xac: 'BW_FAIL',
             0xc0: 'DISP_CACHE',
             0x304: 'TGEN_LITE_SIZE', 0x308: 'TGEN_LITE_VS', 0x314: 'TGEN_LITE_HS'},
    'dsi_mac': {0x00: 'MAC_EN', 0x04: 'HS_0', 0x08: 'HS_1', 0x0c: 'ESC',
                0x20: 'ESC_RX0', 0x24: 'ESC_RX1'},
    'vip_sys': {0x00: 'RESETS', 0x04: 'RESERVE', 0x14: 'CLK_LP', 0x18: 'CLK_CTRL0',
                0x1c: 'CLK_CTRL1', 0x70: 'AXI_RT_FAB_PRI_OW', 0xc0: 'RESETS1'},
    'clk_ctrl': {0x00: 'CLK_EN0', 0x04: 'CLK_EN1', 0x08: 'CLK_EN2', 0x0c: 'CLK_EN3', 0x10: 'CLK_EN4',
                 0xc4: 'DIV_DSI_ESC', 0xc8: 'VIP_DIV_c8', 0xd0: 'VIP_DIV_d0', 0xd8: 'VIP_DIV_d8',
                 0xe0: 'DIV_DISP_SRC_VIP', 0x110: 'VIP_DIV_110',
                 0x140: 'VIP_DIV_140', 0x144: 'VIP_DIV_144', 0x840: 'DISP_DSI_PATH_840',
                 0x900: 'PLL_900', 0x904: 'PLL_904', 0x908: 'PLL_908', 0x90c: 'PLL_90c',
                 0x910: 'PLL_910', 0x914: 'PLL_914', 0x918: 'PLL_918', 0x91c: 'PLL_91c'},
    # the D-PHY block (blob version 3, opt-in pinstripe_dphy), fenced
    # to 51 words (0x00-0xc8). sophgo-dphy.h. DPHY_EN's low bits are the lane enable/state
    # (vendor dphy_get_dsi_lane_status); DPHY_PLL/SET are the PLL config
    # readback (expect 0x218 in PLL[0x6c] low 0x7ff, loop 0x100000, SET
    # 0x10295fad for 74.25 MHz / 4 lanes / RGB888).
    'dphy': {0x00: 'DPHY_EN', 0x14: 'DPHY_HS', 0x64: 'DPHY_PD', 0x6c: 'DPHY_PLL', 0x8c: 'DPHY_UPDATE',
             0x90: 'DPHY_SET', 0x9c: 'DPHY_MAP', 0xa0: 'DPHY_PN'},
}


def xxd(page):
    """Reproduce `xxd` default output for a bytes object starting at offset 0."""
    lines = []
    for offset in range(0, len(page), 16):
        chunk = page[offset:offset + 16]
        groups = [chunk[i:i + 2].hex() for i in range(0, len(chunk), 2)]
        hexpart = ' '.join(groups)
        text = ''.join(chr(b) if 0x20 <= b < 0x7f else '.' for b in chunk)
        lines.append('%08x: %-39s  %s' % (offset, hexpart, text))
    return '\n'.join(lines) + '\n'


def unxxd(text):
    """Parse an `xxd` default-format page back into bytes (golden dbg-locked files)."""
    out = bytearray()
    for line in text.splitlines():
        if ':' not in line:
            continue
        body = line.split(':', 1)[1][:40]
        out += bytes.fromhex(body.replace(' ', ''))
    return bytes(out)


def parse(blob):
    if len(blob) < 24:
        raise ValueError('snapshot header truncated')
    magic, version, page, blocks, slots, next_slot = struct.unpack_from('<6I', blob, 0)
    if magic != MAGIC:
        raise ValueError('not a sophgo-dsi snapshot blob (magic %#x)' % magic)
    if version not in (1, 2, 3) or page != 0x1000 or slots != 3:
        raise ValueError('unsupported blob layout: version %d page %#x blocks %d slots %d'
                         % (version, page, blocks, slots))
    if (version == 3 and blocks != 6) or (version in (1, 2) and blocks != 5):
        raise ValueError('version %d expects %d blocks, got %d'
                         % (version, 6 if version == 3 else 5, blocks))
    expected_size = {1: 224, 2: 256, 3: 272}[version] + slots * blocks * page
    if len(blob) != expected_size:
        raise ValueError('snapshot size does not match its layout')
    words = page // 4
    phys = struct.unpack_from('<%dI' % blocks, blob, 24)
    name_base = 24 + 4 * blocks  # phys[blocks] ends here (v1/v2: 44, v3: 48)
    names = [blob[name_base + 12 * i:name_base + 12 * i + 12].split(b'\0', 1)[0].decode()
             for i in range(blocks)]
    expected_names = ['scaler_top', 'disp', 'dsi_mac', 'vip_sys', 'clk_ctrl']
    if names != expected_names + (['dphy'] if version == 3 else []):
        raise ValueError('unexpected snapshot block names')
    if version == 1:
        fence = [words] * blocks
        header = 224
        slot_base = 104
    else:
        fence_off = name_base + 12 * blocks  # v2: 104, v3: 120
        fence = list(struct.unpack_from('<%dI' % blocks, blob, fence_off))
        reserved = 3 if version == 2 else 2  # words of padding before the slot array
        slot_base = fence_off + 4 * blocks + 4 * reserved  # v2: 136, v3: 152
        header = slot_base + 40 * slots  # v2: 256, v3: 272
    if any(f > words for f in fence):
        raise ValueError('blob fence exceeds the page: %r' % fence)
    slot_list = []
    for i in range(slots):
        base = slot_base + 40 * i
        label = blob[base:base + 24].split(b'\0', 1)[0].decode(errors='replace')
        if not re.fullmatch(r'[A-Za-z0-9_-]*', label):
            raise ValueError('invalid snapshot slot label')
        ns, error, valid = struct.unpack_from('<QiI', blob, base + 24)
        slot_list.append({'index': i, 'label': label, 'ns': ns, 'error': error, 'valid': valid})
    pages = {}
    for s in range(slots):
        for b in range(blocks):
            start = header + (s * blocks + b) * page
            pages[s, b] = blob[start:start + page]
    return {'version': version, 'next': next_slot, 'phys': phys, 'names': names, 'fence': fence,
            'slots': slot_list, 'pages': pages}


def summary(names, phys, fence, slot, pages):
    out = ['slot %d label=%s ns=%d error=%d valid=%#x' % (slot['index'], slot['label'], slot['ns'],
                                                          slot['error'], slot['valid'])]
    for b, name in enumerate(names):
        if not slot['valid'] & (1 << b):
            out.append('  %s @%#010x: not captured' % (name, phys[b]))
            continue
        out.append('  %s @%#010x: %d words read (0x000-%#05x); the rest of the page is unread, not zero'
                   % (name, phys[b], fence[b], fence[b] * 4 - 4))
        page = pages[slot['index'], b]
        for offset, reg in sorted(NAMED.get(name, {}).items()):
            if offset >= fence[b] * 4:
                out.append('  %s+%#05x %-18s unread (beyond fence)' % (name, offset, reg))
                continue
            value = struct.unpack_from('<I', page, offset)[0]
            out.append('  %s+%#05x %-18s %#010x' % (name, offset, reg, value))
    return '\n'.join(out) + '\n'


def golden_diff(parsed, golden_dir):
    """Word-level comparison of every captured slot against the golden pages, fenced."""
    lines = []
    golden = {}
    for name in parsed['names']:
        path = golden_dir / (name + '.hex')
        golden[name] = unxxd(path.read_text()) if path.exists() else None
    for s in sorted(parsed['slots'], key=lambda x: x['ns']):
        if not s['label']:
            continue
        lines.append('== slot %d %s' % (s['index'], s['label']))
        for b, name in enumerate(parsed['names']):
            if not s['valid'] & (1 << b):
                lines.append('  %s: not captured' % name)
                continue
            if golden[name] is None:
                lines.append('  %s: no golden page' % name)
                continue
            page = parsed['pages'][s['index'], b]
            differ = []
            for w in range(parsed['fence'][b]):
                mine = struct.unpack_from('<I', page, 4 * w)[0]
                theirs = struct.unpack_from('<I', golden[name], 4 * w)[0]
                if mine != theirs:
                    differ.append((4 * w, mine, theirs))
            lines.append('  %s: %d of %d fenced words differ' % (name, len(differ), parsed['fence'][b]))
            for offset, mine, theirs in differ:
                reg = NAMED.get(name, {}).get(offset, '')
                lines.append('    +%#05x %-18s mainline %#010x golden %#010x xor %#010x'
                             % (offset, reg, mine, theirs, mine ^ theirs))
    return '\n'.join(lines) + '\n'


def main(argv):
    golden = None
    if '--golden' in argv:
        i = argv.index('--golden')
        golden = Path(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]
    if len(argv) != 3:
        sys.exit(__doc__)
    blob = Path(argv[1]).read_bytes()
    out = Path(argv[2])
    parsed = parse(blob)
    out.mkdir(parents=True, exist_ok=True)
    order = sorted(parsed['slots'], key=lambda s: s['ns'])
    lines = ['blob version %d, bytes %d, captures so far %d, physical bases %s' % (
        parsed['version'], len(blob), parsed['next'],
        ' '.join('%s=%#010x' % (n, p) for n, p in zip(parsed['names'], parsed['phys'])))]
    lines.append('fence (words read per block): %s' % ' '.join(
        '%s=%d' % (n, f) for n, f in zip(parsed['names'], parsed['fence'])))
    for s in order:
        if not s['label']:
            continue
        folder = out / ('%d-%s' % (s['index'], s['label']))
        folder.mkdir(exist_ok=True)
        for b, name in enumerate(parsed['names']):
            if s['valid'] & (1 << b):
                (folder / (name + '.hex')).write_text(xxd(parsed['pages'][s['index'], b]))
        lines.append(summary(parsed['names'], parsed['phys'], parsed['fence'], s, parsed['pages']))
    (out / 'summary.txt').write_text('\n'.join(lines))
    (out / 'slots.json').write_text(json.dumps({'version': parsed['version'], 'next': parsed['next'],
                                                'fence': parsed['fence'], 'slots': parsed['slots'],
                                                'names': parsed['names'], 'phys': parsed['phys']},
                                               indent=2) + '\n')
    print('\n'.join(lines))
    if golden is not None:
        text = golden_diff(parsed, golden)
        (out / 'golden-diff.txt').write_text(text)
        print(text)


if __name__ == '__main__':
    try:
        main(sys.argv)
    except (ValueError, OSError) as error:
        sys.exit(str(error))
