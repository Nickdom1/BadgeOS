# SPDX-License-Identifier: GPL-2.0-only
"""The snapshot reader must reproduce `xxd` byte for byte and honour the blob ABI."""
import importlib.util
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pinstripe_registers', ROOT / 'pinstripe-registers.py')
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)
NAMES = ['scaler_top', 'disp', 'dsi_mac', 'vip_sys', 'clk_ctrl']


def synthetic(labels):
    """labels: list of (label, ns, error, valid) for the three ring slots."""
    random.seed(1)
    blob = bytearray(224 + 3 * 5 * 4096)
    struct.pack_into('<6I', blob, 0, 0x50444753, 1, 0x1000, 5, 3, len(labels))
    struct.pack_into('<5I', blob, 24, 0x0a080000, 0x0a088000, 0x0a08a000, 0x0a0c8000, 0x03002000)
    for i, name in enumerate(NAMES):
        blob[44 + 12 * i:44 + 12 * i + 12] = name.encode().ljust(12, b'\0')
    pages = {}
    for i, (label, ns, error, valid) in enumerate(labels):
        base = 104 + 40 * i
        blob[base:base + 24] = label.encode().ljust(24, b'\0')
        struct.pack_into('<QiI', blob, base + 24, ns, error, valid)
        for b in range(5):
            if valid & (1 << b):
                data = bytes(random.getrandbits(8) for _ in range(4096))
                blob[224 + (i * 5 + b) * 4096:224 + (i * 5 + b + 1) * 4096] = data
                pages[i, b] = data
    return bytes(blob), pages


def synthetic_v3(dphy_words):
    """A version-3 blob (6 blocks incl. dphy, header 272). dphy_words: {offset: value}.
    One filled slot 'complete' with valid=0x3f."""
    names = NAMES + ['dphy']
    phys = [0x0a080000, 0x0a088000, 0x0a08a000, 0x0a0c8000, 0x03002000, 0x0a0d1000]
    fence = [64, 256, 16, 64, 704, 51]
    blob = bytearray(272 + 3 * 6 * 4096)
    struct.pack_into('<6I', blob, 0, 0x50444753, 3, 0x1000, 6, 3, 1)
    struct.pack_into('<6I', blob, 24, *phys)
    for i, name in enumerate(names):
        blob[48 + 12 * i:48 + 12 * i + 12] = name.encode().ljust(12, b'\0')
    struct.pack_into('<6I', blob, 120, *fence)  # reserved[2] at 144 stays zero
    blob[152:152 + 24] = b'complete'.ljust(24, b'\0')
    struct.pack_into('<QiI', blob, 152 + 24, 999, 0, 0x3f)
    for off, val in dphy_words.items():
        struct.pack_into('<I', blob, 272 + (0 * 6 + 5) * 4096 + off, val)
    return bytes(blob)


class ReaderTest(unittest.TestCase):
    def test_xxd_format_matches_the_real_tool(self):
        xxd = shutil.which('xxd')
        if not xxd:
            self.skipTest('xxd unavailable')
        data = bytes(range(256)) * 16
        real = subprocess.run([xxd], input=data, capture_output=True, check=True).stdout.decode()
        self.assertEqual(reader.xxd(data), real)

    def test_unpack_layout_and_summary(self):
        labels = [('complete', 4000, 0, 0x1f), ('started', 2000, -5, 0x17), ('prepared', 1000, 0, 0x1f)]
        blob, pages = synthetic(labels)
        parsed = reader.parse(blob)
        self.assertEqual(parsed['next'], 3)
        self.assertEqual(parsed['names'], NAMES)
        self.assertEqual([s['label'] for s in parsed['slots']], ['complete', 'started', 'prepared'])
        self.assertEqual(parsed['slots'][1]['error'], -5)
        with tempfile.TemporaryDirectory() as tmp:
            blob_path = Path(tmp) / 'blob.bin'
            blob_path.write_bytes(blob)
            out = Path(tmp) / 'out'
            subprocess.run([sys.executable, str(ROOT / 'pinstripe-registers.py'), str(blob_path), str(out)],
                           check=True, capture_output=True)
            for (i, b), data in pages.items():
                self.assertEqual((out / ('%d-%s' % (i, labels[i][0])) / (NAMES[b] + '.hex')).read_text(),
                                 reader.xxd(data))
            self.assertFalse((out / '1-started' / 'vip_sys.hex').exists())
            summary = (out / 'summary.txt').read_text()
            self.assertIn('vip_sys @0x0a0c8000: not captured', summary)
            self.assertIn('dsi_mac+0x000 MAC_EN', summary)
            # Slots are reported in capture order, oldest first.
            self.assertLess(summary.index('label=prepared'), summary.index('label=complete'))

    def test_v3_dphy_block(self):
        blob = synthetic_v3({0x00: 0x0000002f, 0x64: 0x00002000, 0x6c: 0x00100218, 0x90: 0x10295fad})
        parsed = reader.parse(blob)
        self.assertEqual(parsed['version'], 3)
        self.assertEqual(parsed['names'], NAMES + ['dphy'])
        self.assertEqual(parsed['fence'][5], 51)
        self.assertEqual(parsed['phys'][5], 0x0a0d1000)
        text = reader.summary(parsed['names'], parsed['phys'], parsed['fence'],
                              parsed['slots'][0], parsed['pages'])
        self.assertIn('dphy+0x000 DPHY_EN            0x0000002f', text)
        self.assertIn('dphy+0x064 DPHY_PD            0x00002000', text)
        self.assertIn('dphy+0x06c DPHY_PLL           0x00100218', text)
        self.assertIn('dphy+0x090 DPHY_SET           0x10295fad', text)

    def test_rejects_paths_from_blob_before_writing(self):
        blob, _ = synthetic([('complete', 1, 0, 1)])
        for offset, width, value in ((44, 12, '../../escape'), (44, 12, '/tmp/escape'),
                                     (104, 24, 'x/../../escape')):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as tmp:
                modified = bytearray(blob)
                modified[offset:offset + width] = value.encode().ljust(width, b'\0')
                source, out = Path(tmp) / 'snapshot.bin', Path(tmp) / 'decoded'
                source.write_bytes(modified)
                result = subprocess.run([sys.executable, str(ROOT / 'pinstripe-registers.py'),
                                         str(source), str(out)], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(out.exists())
                self.assertFalse((Path(tmp) / 'escape.hex').exists())

    def test_rejects_foreign_blob(self):
        with self.assertRaises(ValueError):
            reader.parse(b'\0' * 61664)
        blob, _ = synthetic([('x', 1, 0, 0)])
        with self.assertRaises(ValueError):
            reader.parse(blob[:1000])


if __name__ == '__main__':
    unittest.main()
