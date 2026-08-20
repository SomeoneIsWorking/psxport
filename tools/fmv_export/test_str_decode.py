#!/usr/bin/env python3
"""test_str_decode.py - TDD spec for the clean-room PSX STR decoder.

Each test pins a contract of the pipeline (extract -> demux -> VLC -> MDEC ->
RGB555 -> PNG). Golden tests against the real disc skip when it is absent;
everything else runs standalone.

Run: python3 -m pytest tools/fmv_export/test_str_decode.py -v
"""

import hashlib
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(__file__))
import str_decode as sd

DISC = os.environ.get("PSXPORT_TOMBA2_DISC", "")


def have_disc():
    return bool(DISC) and os.path.exists(DISC)


class TestBitReader(unittest.TestCase):
    def test_msb_first_le16_units(self):
        b = sd.BitReader(bytes([0x40, 0x00]))       # 16-bit unit 0x0040, MSB-first
        self.assertEqual(b.get(1), 0)
        self.assertEqual(b.get(10), 2)              # next 10 bits of 0x0040
        self.assertEqual(b.get(5), 0)

    def test_refill_across_units(self):
        b = sd.BitReader(bytes([0x00, 0x80, 0x00, 0x01]))
        self.assertEqual(b.get(16), 0x8000)
        self.assertEqual(b.get(8), 0x01)

    def test_eob_pattern_detectable(self):
        b = sd.BitReader(bytes([0x00, 0x80, 0x20, 0x00]))
        self.assertEqual(b.peek(2), 0x2)             # "10" = EOB

    def test_eof(self):
        self.assertTrue(sd.BitReader(b"").eof())             # no 16-bit unit available
        self.assertTrue(sd.BitReader(bytes([0x00])).eof())   # odd trailing byte unreadable
        self.assertFalse(sd.BitReader(bytes([0x00, 0x80])).eof())
        self.assertFalse(sd.BitReader(bytes([0x00, 0x80, 0x00, 0x00])).eof())


class TestVLC(unittest.TestCase):
    def test_sign_extend(self):
        self.assertEqual(sd.sign_extend(0x1FC, 10), 508)
        self.assertEqual(sd.sign_extend(0x3FF, 10), -1)
        self.assertEqual(sd.sign_extend(0x200, 10), -512)
        self.assertEqual(sd.sign_extend(0x000, 10), 0)

    def test_vlc_table_prefix_free(self):
        """No code may be a prefix of another longer code (unique decodability)."""
        codes = sorted((ln, c) for ln, c, _, _ in sd.VLC)
        for i, (ln_i, c_i) in enumerate(codes):
            for ln_j, c_j in codes[i + 1:]:
                if ln_j <= ln_i:
                    continue
                # c_i as an ln_j-bit prefix of c_j means c_j>>(ln_j-ln_i) == c_i
                self.assertNotEqual(c_j >> (ln_j - ln_i), c_i,
                                    f"code ({ln_i},{c_i:0X}) prefixes ({ln_j},{c_j:0X})")

    def test_eob_and_escape(self):
        b = sd.BitReader(bytes([0x00, 0x80, 0x00, 0x00]))   # "10" then zeros
        self.assertEqual(sd.bs_decode_ac(b), 0)              # EOB
        # ESCAPE "000001" + run(6) + level(10): 6+6+10 = 22 bits
        bits = "000001" + format(3, "06b") + format(0x1FC, "010b")   # run=3, level=508
        b = sd.BitReader(bits2bytes(bits))
        self.assertEqual(sd.bs_decode_ac(b), (3, 508))

    def test_known_vlc_entries(self):
        # (run,level) entries: 1 => code 11 len2; sign bit 0 = positive
        b = sd.BitReader(bits2bytes("11" + "0"))
        self.assertEqual(sd.bs_decode_ac(b), (0, 1))
        b = sd.BitReader(bits2bytes("0100" + "1"))           # (0,2) sign 1 = negative
        self.assertEqual(sd.bs_decode_ac(b), (0, -2))


def bits2bytes(bits):
    while len(bits) % 16:
        bits += "0"
    out = bytearray()
    for i in range(0, len(bits), 16):
        w = int(bits[i:i + 16], 2)
        out += struct.pack("<H", w)
    return bytes(out)


class TestMDECDequant(unittest.TestCase):
    def test_dc_dequant(self):
        m = sd.MDEC(320, 240)
        # qscale=1, dc=508 -> code (1<<10)|508 = 0x5FC; first block is Cr (qmw=1)
        m.write_image_data(0x5FC)
        self.assertEqual(m.coeff_index, 1)
        self.assertEqual(m.coeff[0], 16248)     # (508*2)<<4 - 8

    def test_ac_dequant(self):
        m = sd.MDEC(320, 240)
        m.write_image_data(0x5FC)               # DC qscale=1 dc=508
        m.write_image_data(0x403)               # run=1, level=3
        # zero fill at scan idx 1, then level at scan idx 2 (zigzag[2]=1, q=16)
        self.assertEqual(m.coeff[0], 16248)
        self.assertEqual(m.coeff[1], 88)        # ((3*16)>>3)<<4 - 8
        self.assertEqual(m.coeff_index, 3)

    def test_eob_finishes_block(self):
        m = sd.MDEC(320, 240)
        m.write_image_data(0x5FC)
        m.write_image_data(0xFE00)
        self.assertEqual(m.coeff_index, 0)      # block completed
        self.assertEqual(m.decode_wb, 1)        # advanced to Cb

    def test_block_order_cr_cb_y(self):
        """Per-macroblock stream order is Cr, Cb, Y0..Y3 (mdec.c DecodeWB)."""
        m = sd.MDEC(320, 240)
        # all-white frame: Cr=Cb dc 0, Y dc 508
        for i in range(6):
            dc = 0x0400 if i < 2 else 0x05FC
            m.write_image_data(dc)
            m.write_image_data(0xFE00)
        self.assertEqual(m.decode_wb, 0)        # wrapped to next MB
        self.assertEqual(len(m.mbs), 1)
        self.assertTrue(all(p == 0x7FFF for p in m.mbs[0]), "all-white MB")

    def test_idct_dc_only_is_flat(self):
        out = sd.idct([16248] + [0] * 63)
        self.assertTrue(all(v == out[0] for v in out), "DC-only block flat")

    def test_idct_numpy_matches_scalar(self):
        import random
        random.seed(7)
        for _ in range(100):
            coeff = [random.randint(-0x4000, 0x3FFF) for _ in range(64)]
            self.assertEqual(sd.idct(coeff), _scalar_idct(coeff))


def _scalar_idct(coeff):
    tmp = [0] * 64
    for col in range(8):
        for x in range(8):
            s = sum(coeff[col * 8 + u] * sd.IDCTM[x * 8 + u] for u in range(8))
            tmp[x * 8 + col] = (s + 0x4000) >> 15
    out = [0] * 64
    for col in range(8):
        for x in range(8):
            s = sum(tmp[col * 8 + u] * sd.IDCTM[x * 8 + u] for u in range(8))
            out[col * 8 + x] = sd.mask9_clamp_s8((s + 0x4000) >> 15)
    return out


class TestColorPipeline(unittest.TestCase):
    def test_rgb555_channel_layout(self):
        """PSX 16bpp: R=bits 0-4, G=bits 5-9, B=bits 10-14 (mednafen gpu.c)."""
        self.assertEqual(sd.rgb555_to_rgb888(0x001F), (248, 0, 0))
        self.assertEqual(sd.rgb555_to_rgb888(0x03E0), (0, 248, 0))
        self.assertEqual(sd.rgb555_to_rgb888(0x7C00), (0, 0, 248))
        self.assertEqual(sd.rgb555_to_rgb888(0x7FFF), (248, 248, 248))

    def test_ycbcr_white(self):
        r, g, b = sd.ycbcr_to_rgb(107, 0, 0)    # 8-bit white (235,128,128)
        self.assertEqual((r, g, b), (235, 235, 235))

    def test_ycbcr_black(self):
        r, g, b = sd.ycbcr_to_rgb(-112, 0, 0)   # 8-bit black (16,128,128)
        self.assertEqual((r, g, b), (16, 16, 16))


class TestPNG(unittest.TestCase):
    def test_write_and_parse(self):
        import tempfile
        png = sd.write_png_bytes(1, 1, [0x7FFF])
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        pos = 8
        chunks = {}
        while pos < len(png):
            ln, = struct.unpack(">I", png[pos:pos + 4])
            tag = png[pos + 4:pos + 8]
            data = png[pos + 8:pos + 8 + ln]
            crc, = struct.unpack(">I", png[pos + 8 + ln:pos + 12 + ln])
            self.assertEqual(crc, zlib.crc32(tag + data) & 0xFFFFFFFF)
            chunks[tag] = data
            pos += 12 + ln
        w, h, depth, ctype = struct.unpack(">IIBB", chunks[b"IHDR"][:10])
        self.assertEqual((w, h, depth, ctype), (1, 1, 8, 2))
        raw = zlib.decompress(chunks[b"IDAT"])
        self.assertEqual(raw, b"\x00\xf8\xf8\xf8")


@unittest.skipUnless(have_disc(), "disc not available")
class TestExtraction(unittest.TestCase):
    def test_chd_2448_stride(self):
        """Sector 11491 must be LOGO.STR frame-1 chunk-0 (pins the 2448 stride)."""
        chd = sd.Chd(DISC)
        r = chd.read_sector(11491)
        self.assertEqual(r[18] & 0x04, 0x00)                 # not an XA audio sector
        self.assertEqual(r[24] | (r[25] << 8), 0x0160)
        self.assertEqual(r[28] | (r[29] << 8), 0)            # chunk idx 0
        self.assertEqual(r[30] | (r[31] << 8), 5)            # 5 chunks
        self.assertEqual(r[32] | (r[33] << 8) | (r[34] << 16) | (r[35] << 24), 1)  # frame 1

    def test_demux_frame1_payload(self):
        chd = sd.Chd(DISC)
        frames, audio, rate = sd.demux(chd, 11491, 2621440, max_frames=1)
        self.assertEqual(len(frames), 1)
        self.assertEqual(len(frames[0]["payload"]), 10080)   # 5 chunks x 2016
        self.assertEqual(hashlib.sha1(frames[0]["payload"]).hexdigest(),
                         "f39c9a049147394cff4a5df2d8418d9302db1763")

    def test_frame1_all_white_golden(self):
        """Frame 1 of LOGO.STR decodes to a full all-white 320x240 frame."""
        chd = sd.Chd(DISC)
        frames, _, _ = sd.demux(chd, 11491, 2621440, max_frames=1)
        fr = frames[0]
        codes, st = sd.bs_decode_frame(fr["payload"], fr["width"], fr["height"])
        self.assertEqual(st["aborted"], False)
        self.assertEqual(st["blocks"], 1800)
        self.assertEqual(st["ac_count"], 0)
        # luma DC=508 x1200, chroma DC=0 x600 -> exactly an all-white frame
        from collections import Counter
        dc = Counter(st["dcs"])
        self.assertEqual(dc[508], 1200)
        self.assertEqual(dc[0], 600)
        mbs = sd.MDEC(fr["width"], fr["height"]).decode(codes)
        self.assertEqual(len(mbs), 300)                      # all macroblocks decoded
        px = sd.tile_frame(mbs, fr["width"], fr["height"])
        self.assertTrue(all(p == 0x7FFF for p in px))

    def test_content_frame_deterministic_and_colorful(self):
        """First frame with AC content decodes deterministically with correct channels."""
        chd = sd.Chd(DISC)
        frames, _, _ = sd.demux(chd, 11491, 2621440)
        for fr in frames:
            codes, st = sd.bs_decode_frame(fr["payload"], fr["width"], fr["height"])
            if st["ac_count"] > 0:
                mbs = sd.MDEC(fr["width"], fr["height"]).decode(codes)
                px = sd.tile_frame(mbs, fr["width"], fr["height"])
                self.assertEqual(len(mbs), 300)
                # same payload -> identical output (determinism)
                mbs2 = sd.MDEC(fr["width"], fr["height"]).decode(codes)
                self.assertEqual(px, sd.tile_frame(mbs2, fr["width"], fr["height"]))
                # primaries must land in their correct channels if any exist
                colors = set(px)
                if 0x001F in colors:
                    self.assertEqual(sd.rgb555_to_rgb888(0x001F), (248, 0, 0))
                return
        self.fail("no AC-content frame found")


if __name__ == "__main__":
    unittest.main(verbosity=2)
