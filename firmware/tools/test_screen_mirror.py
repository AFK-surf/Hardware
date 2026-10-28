# SPDX-License-Identifier: Apache-2.0
"""screen_mirror.py decoding against frames coded the way ui/screen_mirror.c codes them."""

import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(__file__))
try:
    import screen_mirror
except ImportError:  # pyserial missing: the decoder does not need it
    import types
    sys.modules["serial"] = types.ModuleType("serial")
    import screen_mirror


def code(w, h, levels):
    """The firmware's run-length coding of 4-bit levels."""
    out = bytearray(struct.pack("<HH", w, h))
    i = 0
    while i < len(levels):
        run = 1
        while i + run < len(levels) and run < 65535 and levels[i + run] == levels[i]:
            run += 1
        if run <= 15:
            out.append(levels[i] << 4 | (run - 1))
        else:
            out.append(levels[i] << 4 | 15)
            out += struct.pack("<H", run)
        i += run
    return bytes(out)


class Decode(unittest.TestCase):
    def test_round_trip(self):
        w, h = 466, 466
        levels = [0] * (w * h)
        for i in range(1000, 1200):
            levels[i] = 15
        levels[5000:5003] = [3, 8, 8]
        levels[-20:] = [7] * 20
        data = code(w, h, levels)
        dw, dh, pixels = screen_mirror.decode(data)
        self.assertEqual((dw, dh), (w, h))
        self.assertEqual(list(pixels), [lv * 17 for lv in levels])

    def test_long_runs_and_size_check(self):
        data = code(300, 300, [2] * 90000)
        self.assertEqual(len(data), 4 + 3 * 2)  # 65535 + 24465
        self.assertEqual(screen_mirror.decode(data)[2], bytes([34]) * 90000)
        with self.assertRaises(ValueError):
            screen_mirror.decode(data[:-3])

    def test_hash_matches_fnv1a(self):
        self.assertEqual(screen_mirror.fnv1a(b""), 0x811C9DC5)
        self.assertEqual(screen_mirror.fnv1a(b"a"), 0xE40C292C)

    def test_png(self):
        png = screen_mirror.png(4, 4, bytes(range(16)))
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        idat = png[png.index(b"IDAT") + 4:png.index(b"IEND") - 8]
        self.assertEqual(len(zlib.decompress(idat)), 4 * 5)


if __name__ == "__main__":
    unittest.main()
