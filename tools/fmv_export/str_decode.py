#!/usr/bin/env python3
"""str_decode.py - independent PSX .STR extractor + decoder (clean-room reference).

Clean-room Python re-implementation of the FMV pipeline that the C++ side
(fmv_decode.cpp + vendored mdec.c) implements, so we can tell whether a bad
frame is caused by the C++ decode/backend or by the bitstream itself.

Pipeline:
  CHD (ctypes -> libchdr) -> raw 2352B sectors -> STR demux (XA-ADPCM audio +
  video chunks) -> BS VLC decode (MPEG-1 AC table) -> MDEC dequant/IDCT ->
  YCbCr -> RGB555 -> macroblock tile -> PNG frames + WAV audio.

The MDEC math (dequant rounding, two-pass fixed-point IDCT, YCbCr/555) is a
literal port of mednafen mdec.c so the output is byte-comparable with the
C++ backend.

Usage:
  str_decode.py <disc.chd> <outdir> [--lba N] [--size N] [--frames N]
                [--rowmajor] [--libchdr PATH]

  Defaults: --lba 11491 --size 2621440 (LOGO.STR on the Tomba! 2 disc).
"""

import ctypes
import math
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# libchdr access
# ---------------------------------------------------------------------------

class ChdHeader(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_uint32), ("version", ctypes.c_uint32),
        ("flags", ctypes.c_uint32), ("compression", ctypes.c_uint32 * 4),
        ("hunkbytes", ctypes.c_uint32), ("totalhunks", ctypes.c_uint32),
        ("logicalbytes", ctypes.c_uint64), ("metaoffset", ctypes.c_uint64),
        ("mapoffset", ctypes.c_uint64),
        ("md5", ctypes.c_uint8 * 16), ("parentmd5", ctypes.c_uint8 * 16),
        ("sha1", ctypes.c_uint8 * 20), ("rawsha1", ctypes.c_uint8 * 20),
        ("parentsha1", ctypes.c_uint8 * 20),
        ("unitbytes", ctypes.c_uint32), ("unitcount", ctypes.c_uint64),
        ("hunkcount", ctypes.c_uint32), ("mapentrybytes", ctypes.c_uint32),
        ("rawmap", ctypes.c_void_p),
        ("obsolete_cylinders", ctypes.c_uint32), ("obsolete_sectors", ctypes.c_uint32),
        ("obsolete_heads", ctypes.c_uint32), ("obsolete_hunksize", ctypes.c_uint32),
    ]


class Chd:
    """Reads raw 2352-byte sectors from a CHD via libchdr."""

    def __init__(self, path, libchdr=None):
        if libchdr is None:
            libchdr = os.environ.get("PSXPORT_LIBCHDR",
                                     os.path.join(os.path.dirname(__file__),
                                                  "../../../../scratch/fmv_export/pylibs/libchdr.so"))
        if not os.path.exists(libchdr):
            raise RuntimeError(f"libchdr not found at {libchdr}; build it or set PSXPORT_LIBCHDR")
        self.lib = ctypes.CDLL(libchdr)
        self.lib.chd_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p,
                                      ctypes.POINTER(ctypes.c_void_p)]
        self.lib.chd_open.restype = ctypes.c_int
        self.lib.chd_get_header.argtypes = [ctypes.c_void_p]
        self.lib.chd_get_header.restype = ctypes.POINTER(ChdHeader)
        self.lib.chd_read.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        self.lib.chd_read.restype = ctypes.c_int
        self.lib.chd_close.argtypes = [ctypes.c_void_p]

        self.h = ctypes.c_void_p()
        rc = self.lib.chd_open(path.encode(), 1, None, ctypes.byref(self.h))
        if rc != 0:
            raise RuntimeError(f"chd_open failed: rc={rc}")
        hdr = self.lib.chd_get_header(self.h).contents
        self.hunkbytes = hdr.hunkbytes
        self.totalhunks = hdr.totalhunks
        # Sector stride: some CHDs pack 96 bytes of subchannel after each 2352B
        # CD sector (2448B units); detect which layout this disc uses.
        self.sector_size = 2448 if self.hunkbytes % 2448 == 0 else 2352
        self._cache = {}

    def read_sector(self, sec):
        off = sec * self.sector_size
        out = bytearray()
        got = 0
        while got < 2352:
            hunk = (off + got) // self.hunkbytes
            if hunk not in self._cache:
                buf = ctypes.create_string_buffer(self.hunkbytes)
                rc = self.lib.chd_read(self.h, hunk, buf)
                if rc != 0:
                    raise RuntimeError(f"chd_read hunk {hunk} failed: rc={rc}")
                self._cache[hunk] = buf.raw
            base = (off + got) % self.hunkbytes
            take = min(2352 - got, self.hunkbytes - base)
            out += self._cache[hunk][base:base + take]
            got += take
        return bytes(out)

    def close(self):
        self.lib.chd_close(self.h)


# ---------------------------------------------------------------------------
# BS / MPEG-1 AC VLC decode (faithful port of fmv_decode.cpp)
# ---------------------------------------------------------------------------

# {len, code, run, level} - canonical MPEG-1 / PSX STR AC VLC table.
VLC = [
    (2, 0x03, 0, 1), (3, 0x03, 1, 1), (4, 0x04, 0, 2), (4, 0x05, 2, 1),
    (5, 0x05, 0, 3), (5, 0x06, 4, 1), (5, 0x07, 3, 1), (6, 0x04, 7, 1),
    (6, 0x05, 6, 1), (6, 0x06, 1, 2), (6, 0x07, 5, 1), (7, 0x04, 2, 2),
    (7, 0x05, 9, 1), (7, 0x06, 0, 4), (7, 0x07, 8, 1), (8, 0x20, 13, 1),
    (8, 0x21, 0, 6), (8, 0x22, 12, 1), (8, 0x23, 11, 1), (8, 0x24, 3, 2),
    (8, 0x25, 1, 3), (8, 0x26, 0, 5), (8, 0x27, 10, 1), (10, 0x08, 16, 1),
    (10, 0x09, 5, 2), (10, 0x0a, 0, 7), (10, 0x0b, 2, 3), (10, 0x0c, 1, 4),
    (10, 0x0d, 15, 1), (10, 0x0e, 14, 1), (10, 0x0f, 4, 2), (12, 0x10, 0, 11),
    (12, 0x11, 8, 2), (12, 0x12, 4, 3), (12, 0x13, 0, 10), (12, 0x14, 2, 4),
    (12, 0x15, 7, 2), (12, 0x16, 21, 1), (12, 0x17, 20, 1), (12, 0x18, 0, 9),
    (12, 0x19, 19, 1), (12, 0x1a, 18, 1), (12, 0x1b, 1, 5), (12, 0x1c, 3, 3),
    (12, 0x1d, 0, 8), (12, 0x1e, 6, 2), (12, 0x1f, 17, 1), (13, 0x10, 10, 2),
    (13, 0x11, 9, 2), (13, 0x12, 5, 3), (13, 0x13, 3, 4), (13, 0x14, 2, 5),
    (13, 0x15, 1, 7), (13, 0x16, 1, 6), (13, 0x17, 0, 15), (13, 0x18, 0, 14),
    (13, 0x19, 0, 13), (13, 0x1a, 0, 12), (13, 0x1b, 26, 1), (13, 0x1c, 25, 1),
    (13, 0x1d, 24, 1), (13, 0x1e, 23, 1), (13, 0x1f, 22, 1), (14, 0x10, 0, 31),
    (14, 0x11, 0, 30), (14, 0x12, 0, 29), (14, 0x13, 0, 28), (14, 0x14, 0, 27),
    (14, 0x15, 0, 26), (14, 0x16, 0, 25), (14, 0x17, 0, 24), (14, 0x18, 0, 23),
    (14, 0x19, 0, 22), (14, 0x1a, 0, 21), (14, 0x1b, 0, 20), (14, 0x1c, 0, 19),
    (14, 0x1d, 0, 18), (14, 0x1e, 0, 17), (14, 0x1f, 0, 16), (15, 0x10, 0, 40),
    (15, 0x11, 0, 39), (15, 0x12, 0, 38), (15, 0x13, 0, 37), (15, 0x14, 0, 36),
    (15, 0x15, 0, 35), (15, 0x16, 0, 34), (15, 0x17, 0, 33), (15, 0x18, 0, 32),
    (15, 0x19, 1, 14), (15, 0x1a, 1, 13), (15, 0x1b, 1, 12), (15, 0x1c, 1, 11),
    (15, 0x1d, 1, 10), (15, 0x1e, 1, 9), (15, 0x1f, 1, 8), (16, 0x10, 1, 18),
    (16, 0x11, 1, 17), (16, 0x12, 1, 16), (16, 0x13, 1, 15), (16, 0x14, 6, 3),
    (16, 0x15, 16, 2), (16, 0x16, 15, 2), (16, 0x17, 14, 2), (16, 0x18, 13, 2),
    (16, 0x19, 12, 2), (16, 0x1a, 11, 2), (16, 0x1b, 31, 1), (16, 0x1c, 30, 1),
    (16, 0x1d, 29, 1), (16, 0x1e, 28, 1), (16, 0x1f, 27, 1),
]
VLC_BY_LEN = {}
for _len, _code, _run, _level in VLC:
    VLC_BY_LEN.setdefault(_len, {})[_code] = (_run, _level)

ZIGZAG = [
    0x00, 0x08, 0x01, 0x02, 0x09, 0x10, 0x18, 0x11,
    0x0a, 0x03, 0x04, 0x0b, 0x12, 0x19, 0x20, 0x28,
    0x21, 0x1a, 0x13, 0x0c, 0x05, 0x06, 0x0d, 0x14,
    0x1b, 0x22, 0x29, 0x30, 0x38, 0x31, 0x2a, 0x23,
    0x1c, 0x15, 0x0e, 0x07, 0x0f, 0x16, 0x1d, 0x24,
    0x2b, 0x32, 0x39, 0x3a, 0x33, 0x2c, 0x25, 0x1e,
    0x17, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x3c, 0x35,
    0x2e, 0x27, 0x2f, 0x36, 0x3d, 0x3e, 0x37, 0x3f,
]

QUANT_DEFAULT = [
    2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
]
QMATRIX = [  # raster[zigzag[scan]] so QMatrix[scan] is the per-frequency weight
    [QUANT_DEFAULT[ZIGZAG[s]] for s in range(64)],
    [QUANT_DEFAULT[ZIGZAG[s]] for s in range(64)],
]


class BitReader:
    def __init__(self, data):
        self.data = data          # bytes after the 8-byte BS header
        self.size = len(data)
        self.bytepos = 0
        self.bitbuf = 0
        self.bitcnt = 0

    def refill(self):
        while self.bitcnt <= 16 and self.bytepos + 1 < self.size:
            w = self.data[self.bytepos] | (self.data[self.bytepos + 1] << 8)
            self.bytepos += 2
            self.bitbuf |= w << (16 - self.bitcnt)
            self.bitcnt += 16

    def peek(self, n):
        self.refill()
        if n == 0:
            return 0
        return (self.bitbuf >> (32 - n)) & ((1 << n) - 1)

    def skip(self, n):
        self.bitbuf <<= n
        self.bitcnt -= n

    def get(self, n):
        v = self.peek(n)
        self.skip(n)
        return v

    def eof(self):
        return self.bitcnt <= 0 and self.bytepos + 1 >= self.size


def bs_decode_ac(b):
    """Returns 0=EOB, 1=(run,level), -1=bad code."""
    if b.peek(2) == 0x2:
        b.skip(2)
        return 0
    if b.peek(6) == 0x1:                     # ESCAPE "000001"
        b.skip(6)
        run = b.get(6)
        lvl = b.get(10)
        if lvl & 0x200:
            lvl -= 0x400
        return run, lvl
    for n in range(1, 17):
        v = b.peek(n)
        ent = VLC_BY_LEN.get(n, {}).get(v)
        if ent is not None:
            b.skip(n)
            s = b.get(1)
            run, level = ent
            return run, (-level if s else level)
    return -1


def bs_decode_frame(payload, width, height):
    """VLC-decode a frame payload -> list of 16-bit MDEC codes + stats dict."""
    bs_q = payload[4] | (payload[5] << 8)
    qscale = bs_q & 0x3F
    if qscale == 0:
        qscale = 1
    nwords_hdr = payload[0] | (payload[1] << 8)
    br = BitReader(payload[8:])

    w = width
    h = height
    mbx = (w + 15) // 16
    mby = (h + 15) // 16
    nblocks = mbx * mby * 6

    codes = []
    dcs = []
    ac_count = 0
    for blk in range(nblocks):
        if br.eof():
            break
        dc = br.get(10)
        if dc & 0x200:
            dc -= 0x400
        dcs.append(dc)
        codes.append(((qscale & 0x3F) << 10) | (dc & 0x3FF))
        while True:
            r = bs_decode_ac(br)
            if r == 0:
                codes.append(0xFE00)
                break
            if r == -1:
                return codes, {
                    "nwords_hdr": nwords_hdr, "qscale": qscale, "blocks": blk,
                    "dcs": dcs, "ac_count": ac_count, "aborted": True,
                }
            run, level = r
            ac_count += 1
            codes.append(((run & 0x3F) << 10) | (level & 0x3FF))
    return codes, {
        "nwords_hdr": nwords_hdr, "qscale": qscale, "blocks": len(dcs),
        "dcs": dcs, "ac_count": ac_count, "aborted": False,
    }


# ---------------------------------------------------------------------------
# MDEC decode - literal port of mednafen mdec.c
# ---------------------------------------------------------------------------

def sign_extend(v, bits):
    v &= (1 << bits) - 1
    if v & (1 << (bits - 1)):
        v -= 1 << bits
    return v


def mask9_clamp_s8(v):
    v = sign_extend(v, 9)
    return max(-128, min(127, v))


# IDCT matrix: uploaded round(basis * 2^15) for basis c(u)*cos((2x+1)u*pi/16),
# stored transposed and >> 3 (mdec.c cmd-3 upload path).
IDCTM = [0] * 64
for u in range(8):
    for x in range(8):
        cu = (1.0 / math.sqrt(2.0)) if u == 0 else 1.0
        v = cu * math.cos((2 * x + 1) * u * math.pi / 16.0) * 32768.0
        iv = int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))
        iv = max(-32768, min(32767, iv))
        uploaded = iv
        IDCTM[x * 8 + u] = uploaded >> 3


def idct(coeff):
    """Scalar port of mednafen's two-pass fixed-point IDCT (mdec.c)."""
    tmp = [0] * 64
    for col in range(8):
        for x in range(8):
            s = 0
            for u in range(8):
                s += coeff[col * 8 + u] * IDCTM[x * 8 + u]
            tmp[x * 8 + col] = (s + 0x4000) >> 15
    out = [0] * 64
    for col in range(8):
        for x in range(8):
            s = 0
            for u in range(8):
                s += tmp[col * 8 + u] * IDCTM[x * 8 + u]
            out[col * 8 + x] = mask9_clamp_s8((s + 0x4000) >> 15)
    return out


try:
    import numpy as np
    _IDCTM_NP = np.array(IDCTM, dtype=np.int64).reshape(8, 8)

    def idct(coeff):    # vectorized equivalent of the scalar idct()
        c = np.asarray(coeff, dtype=np.int64).reshape(8, 8)
        tmp = _IDCTM_NP @ c.T          # tmp[x][col] = sum_u M[x][u] * c[col][u]
        tmp = (tmp + 0x4000) >> 15
        out = (_IDCTM_NP @ tmp.T).T    # out[col][x] = sum_u M[x][u] * tmp[col][u]
        out = (out + 0x4000) >> 15
        out = out & 0x1FF               # sign-extend from 9 bits (mask first)
        out = np.where(out & 0x100 != 0, out - 0x200, out)
        out = np.clip(out, -128, 127)
        return out.astype(np.int64).flatten().tolist()
except ImportError:
    pass


def ycbcr_to_rgb(y, cb, cr):
    r = mask9_clamp_s8(y + ((359 * cr + 0x80) >> 8))
    g = mask9_clamp_s8(y + ((((-88 * cb) & ~0x1F) + ((-183 * cr) & ~0x07) + 0x80) >> 8))
    b = mask9_clamp_s8(y + ((454 * cb + 0x80) >> 8))
    # mdec.c stores via uint8_t: (int ^ 0x80) wraps mod 256.
    return (r ^ 0x80) & 0xFF, (g ^ 0x80) & 0xFF, (b ^ 0x80) & 0xFF


def rgb_to_rgb555(r, g, b):
    r = (r + 4) >> 3
    g = (g + 4) >> 3
    b = (b + 4) >> 3
    r = min(r, 0x1F); g = min(g, 0x1F); b = min(b, 0x1F)
    return r | (g << 5) | (b << 10)


class MDEC:
    """Decodes the MDEC code stream into 16x16 macroblock rasters (in stream order)."""

    def __init__(self, width, height):
        self.w = width
        self.h = height
        self.mbx = (width + 15) // 16
        self.mby = (height + 15) // 16
        self.coeff = [0] * 64
        self.coeff_index = 0
        self.qscale = 0
        self.decode_wb = 0
        self.block_y = [[0] * 8 for _ in range(8)]
        self.block_cb = [[0] * 8 for _ in range(8)]
        self.block_cr = [[0] * 8 for _ in range(8)]
        self.cur_mb = [0] * 256
        self.mbs = []          # list of 16x16 pixel grids (int rgb555)

    def write_image_data(self, v):
        qmw = 1 if self.decode_wb < 2 else 0
        ci_index = self.coeff_index
        if ci_index == 0:
            if v == 0xFE00:
                return False
            self.qscale = v >> 10
            ci = sign_extend(v & 0x3FF, 10)
            q = QMATRIX[qmw][0]
            if q != 0:
                tmp = (ci * q) << 4
                tmp += 8 if ci < 0 else (-8 if ci else 0)
            else:
                tmp = (ci * 2) << 4
            tmp = max(-0x4000, min(0x3FFF, tmp))
            self.coeff[ZIGZAG[0]] = tmp
            self.coeff_index = 1
        else:
            if v == 0xFE00:
                while self.coeff_index < 64:
                    self.coeff[ZIGZAG[self.coeff_index]] = 0
                    self.coeff_index += 1
            else:
                rlcount = v >> 10
                i = 0
                while i < rlcount and self.coeff_index < 64:
                    self.coeff[ZIGZAG[self.coeff_index]] = 0
                    self.coeff_index += 1
                    i += 1
                if self.coeff_index < 64:
                    q = self.qscale * QMATRIX[qmw][self.coeff_index]
                    ci = sign_extend(v & 0x3FF, 10)
                    if q != 0:
                        tmp = ((ci * q) >> 3) << 4
                        tmp += 8 if ci < 0 else (-8 if ci else 0)
                    else:
                        tmp = (ci * 2) << 4
                    tmp = max(-0x4000, min(0x3FFF, tmp))
                    self.coeff[ZIGZAG[self.coeff_index]] = tmp
                    self.coeff_index += 1

        if self.coeff_index == 64:
            self.coeff_index = 0
            db = self.decode_wb
            out = idct(self.coeff)              # 64 int8 values, raster row-major
            rows = [out[i * 8:(i + 1) * 8] for i in range(8)]
            if db == 0:
                self.block_cr = rows
            elif db == 1:
                self.block_cb = rows
            else:
                self.block_y = rows
            if db >= 2:
                self.encode_image((db + 4) % 6)
            self.decode_wb += 1
            if self.decode_wb == 6:
                self.decode_wb = 0
        return True

    def encode_image(self, ybn):
        for y in range(8):
            by = self.block_y[y]
            cb_row = (y >> 1) | ((ybn & 2) << 1)
            cb_col = (ybn & 1) << 2
            cr_row = cb_row
            for x in range(8):
                yv = by[x]
                cbv = self.block_cb[cb_row][cb_col + (x >> 1)]
                crv = self.block_cr[cr_row][cb_col + (x >> 1)]
                r, g, b = ycbcr_to_rgb(yv, cbv, crv)
                self.cur_mb[(ybn >> 1) * 128 + (ybn & 1) * 8 + y * 16 + x] = rgb_to_rgb555(r, g, b)
        if ybn == 3:
            self.mbs.append(self.cur_mb)
            self.cur_mb = [0] * 256

    def decode(self, codes):
        for v in codes:
            self.write_image_data(v)
        return self.mbs


# ---------------------------------------------------------------------------
# Frame assembly (tile 16x16 macroblocks into the frame) + PNG/WAV output
# ---------------------------------------------------------------------------

def tile_frame(mbs, width, height, rowmajor=False):
    """mbs: list of 16x16 rgb555 grids in stream order."""
    mbx = (width + 15) // 16
    mby = (height + 15) // 16
    pixels = [0] * (width * height)
    for k, mb in enumerate(mbs):
        if rowmajor:
            by, bx = divmod(k, mbx)
        else:
            bx = k // mby
            by = k % mby
        if bx >= mbx or by >= mby:
            continue
        for yy in range(16):
            fy = by * 16 + yy
            if fy >= height:
                break
            for xx in range(16):
                fx = bx * 16 + xx
                if fx >= width:
                    break
                pixels[fy * width + fx] = mb[yy * 16 + xx]
    return pixels


def rgb555_to_rgb888(p):
    """PSX 16bpp layout: R=bits 0-4, G=bits 5-9, B=bits 10-14 (mednafen gpu.c)."""
    return (((p >> 0) & 31) << 3,
            ((p >> 5) & 31) << 3,
            ((p >> 10) & 31) << 3)


def write_png_bytes(width, height, pixels):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        c += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return c

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        base = y * width
        for x in range(width):
            raw += bytes(rgb555_to_rgb888(pixels[base + x]))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))


def write_png(path, width, height, pixels):
    with open(path, "wb") as f:
        f.write(write_png_bytes(width, height, pixels))


def write_wav(path, samples, rate):
    n = len(samples)
    data = struct.pack("<%dh" % n, *samples)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(data)))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(data)))
        f.write(data)


# ---------------------------------------------------------------------------
# XA-ADPCM audio (port of xa_decode_sector in fmv_decode.cpp)
# ---------------------------------------------------------------------------

XA_W = [  # 16-entry standard CD-XA filter table
    (0, 0), (60, 0), (115, -52), (98, -55), (122, -60), (55, -35), (-61, 31), (22, 0),
    (50, -10), (-27, -9), (103, -57), (122, -59), (40, 20), (72, -34), (52, -37), (25, -10),
]


def xa_decode_unit(indata, out, shift, filter_idx):
    w0, w1 = XA_W[filter_idx]
    for i in range(28):
        s = ((indata[i] << 8) & 0xFFFF)
        if s & 0x8000:
            s -= 0x10000
        s >>= shift
        s += (out[i - 1] * w0 + out[i - 2] * w1) >> 6
        s = max(-32768, min(32767, s))
        out[i] = s


def xa_decode_sector(raw, hist):
    """Returns (list of (L,R) sample pairs, sample rate)."""
    coding = raw[19]
    ishift = 0 if (coding & 0x10) else 1
    stereo = coding & 0x01
    units = 4 << ishift
    rate = 18900 if (coding & 0x04) else 37800
    ch = [[0] * (4032 + 8) for _ in range(2)]
    cp = [0, 0]
    for group in range(18):
        sg = raw[24 + group * 128: 24 + group * 128 + 128]
        for unit in range(units):
            param = sg[(unit & 3) | ((unit & 4) << 1)]
            pcopy = sg[4 | (unit & 3) | ((unit & 4) << 1)]
            ib = []
            for i in range(28):
                t = sg[16 + i * 4 + (unit >> ishift)]
                if ishift:
                    t = (t << (0 if (unit & 1) else 4)) & 0xF0
                ib.append(t)
            ocn = 1 if (unit & 1 and stereo) else 0
            ob = [0] * 30
            ob[0] = hist[ocn][0]
            ob[1] = hist[ocn][1]
            xa_decode_unit(ib, ob[2:], param & 0x0F, param >> 4)
            hist[ocn][0] = ob[28]
            hist[ocn][1] = ob[29]
            if param != pcopy:
                ob = [0] * 30
            if stereo:
                for s in range(28):
                    ch[ocn][cp[ocn]] = ob[2 + s]
                    cp[ocn] += 1
            else:
                for s in range(28):
                    ch[0][cp[0]] = ob[2 + s]
                    ch[1][cp[1]] = ob[2 + s]
                    cp[0] += 1
                    cp[1] += 1
    n = cp[0]
    return [(ch[0][i], ch[1][i]) for i in range(n)], rate


# ---------------------------------------------------------------------------
# STR demux (faithful port of the fmv_export.cpp loop)
# ---------------------------------------------------------------------------

def find_str_files(chd):
    """Walk the ISO9660 tree and return [(path, iso_lba, size)] for *.STR files.

    iso_lba is the file's start sector relative to the ISO data track (raw sector
    number, 1:1 on the discs we use).
    """
    import struct
    results = []
    pvd = None
    for sec in range(16, 400):
        r = chd.read_sector(sec)
        if r[25:30] == b"CD001":
            pvd = sec
            break
    if pvd is None:
        raise RuntimeError("no ISO9660 PVD found")
    root = chd.read_sector(pvd)[24 + 156:]
    root_ext, = struct.unpack("<I", root[2:6])
    root_size, = struct.unpack("<I", root[10:14])

    visited = set()

    def walk(lba, size, prefix, path):
        key = (lba, size)
        if key in visited:
            return
        visited.add(key)
        nsect = max(1, (size + 2047) // 2048)
        for s in range(lba, lba + nsect):
            d = chd.read_sector(s)
            off = 24
            while off < 24 + 2048:
                ln = d[off]
                if ln == 0:
                    off = 24 + (((off - 24) // 2048) + 1) * 2048
                    continue
                if off + 34 > 24 + 2048:
                    break
                namelen = d[off + 32]
                raw = d[off + 33:off + 33 + namelen]
                name = raw.rstrip(b";1").rstrip(b".").decode("latin1")
                flags = d[off + 25]
                fsize, = struct.unpack("<I", d[off + 10:off + 14])
                fext, = struct.unpack("<I", d[off + 2:off + 6])
                if flags & 2:
                    walk(fext, fsize, prefix, path + name + "/")
                elif name.lower().endswith(".str"):
                    results.append((prefix + name, fext, fsize))
                off += ln

    walk(root_ext, root_size, "", "")
    return results


# ---------------------------------------------------------------------------
# STR demux (faithful port of the fmv_export.cpp loop)
# ---------------------------------------------------------------------------

def demux(chd, lba, size, max_frames=0):
    nsectors = (size + 2048 - 1) // 2048
    frames = []
    audio = []
    xa_freq = 37800
    hist = [[0, 0], [0, 0]]
    media_frames = 0
    cur_frame = -1
    paylen = 0
    payload = bytearray()
    expected_chunks = 0
    got_chunks = 0
    fwidth = 320
    fheight = 240

    for sec in range(nsectors):
        raw = chd.read_sector(lba + sec)
        submode = raw[18]
        if submode & 0x04:                      # XA-ADPCM audio sector
            pairs, freq = xa_decode_sector(raw, hist)
            if media_frames == 0:
                xa_freq = freq
            audio.extend(pairs)
            media_frames += len(pairs)
            continue

        sbuf = raw[24:24 + 2048]
        magic = sbuf[0] | (sbuf[1] << 8)
        if magic != 0x0160:
            continue
        chunk_idx = sbuf[4] | (sbuf[5] << 8)
        nchunks = sbuf[6] | (sbuf[7] << 8)
        framenum = sbuf[8] | (sbuf[9] << 8) | (sbuf[10] << 16) | (sbuf[11] << 24)
        w = sbuf[16] | (sbuf[17] << 8)
        h = sbuf[18] | (sbuf[19] << 8)

        if chunk_idx == 0:
            cur_frame = framenum
            paylen = 0
            payload = bytearray()
            expected_chunks = nchunks
            got_chunks = 0
            fwidth = w if w else 320
            fheight = h if h else 240

        if cur_frame != framenum:
            continue
        payload += sbuf[32:32 + 2016]
        paylen += 2016
        got_chunks += 1

        if expected_chunks > 0 and got_chunks >= expected_chunks:
            frames.append({
                "framenum": framenum, "width": fwidth, "height": fheight,
                "chunks": expected_chunks, "payload": bytes(payload),
                "media_frames": media_frames,
            })
            cur_frame = -1
            if max_frames and len(frames) >= max_frames:
                break
    return frames, audio, xa_freq


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    chd_path = argv[1]
    outdir = argv[2]
    lba = 11491
    size = 2621440
    str_name = None
    max_frames = 0
    rowmajor = False
    libchdr = None
    i = 3
    while i < len(argv):
        a = argv[i]
        if a == "--lba" and i + 1 < len(argv):
            lba = int(argv[i + 1]); i += 2
        elif a == "--size" and i + 1 < len(argv):
            size = int(argv[i + 1]); i += 2
        elif a == "--str" and i + 1 < len(argv):
            str_name = argv[i + 1]; i += 2
        elif a == "--frames" and i + 1 < len(argv):
            max_frames = int(argv[i + 1]); i += 2
        elif a == "--rowmajor":
            rowmajor = True; i += 1
        elif a == "--libchdr" and i + 1 < len(argv):
            libchdr = argv[i + 1]; i += 2
        else:
            i += 1

    os.makedirs(os.path.join(outdir, "frames"), exist_ok=True)
    chd = Chd(chd_path, libchdr)
    if str_name:
        for p, flba, fsize in find_str_files(chd):
            if p.upper() == str_name.upper() or os.path.basename(p).upper() == str_name.upper():
                print(f"found {p} at LBA {flba} size {fsize}")
                lba, size = flba, fsize
                break
        else:
            raise SystemExit(f"STR file '{str_name}' not found on disc")
    frames, audio, xa_freq = demux(chd, lba, size, max_frames)
    chd.close()

    print(f"demuxed {len(frames)} video frames, {len(audio)} audio sample-pairs "
          f"@ {xa_freq} Hz")

    # sanity: identical demux payload to the C exporter? hash frame 1 payload
    if frames:
        import hashlib
        print("frame1 payload sha1:", hashlib.sha1(frames[0]["payload"]).hexdigest())

    stats_path = os.path.join(outdir, "py_stats.txt")
    with open(stats_path, "w") as st:
        for fi, fr in enumerate(frames):
            codes, st_ = bs_decode_frame(fr["payload"], fr["width"], fr["height"])
            dc_hist = {}
            for d in st_["dcs"]:
                dc_hist[d] = dc_hist.get(d, 0) + 1
            dcs_str = " ".join(f"{k}:{v}" for k, v in
                               sorted(dc_hist.items(), key=lambda kv: -kv[1])[:8])
            print(f"frame {fi + 1}: str#{fr['framenum']} {fr['width']}x{fr['height']} "
                  f"chunks={fr['chunks']} payload={len(fr['payload'])}B "
                  f"hdr_nwords={st_['nwords_hdr']} blocks={st_['blocks']}/1800 "
                  f"ac={st_['ac_count']} aborted={st_['aborted']} topDC: {dcs_str}")
            st.write(f"frame {fi + 1}: hdr_nwords={st_['nwords_hdr']} "
                     f"blocks={st_['blocks']} ac={st_['ac_count']} dcs={dc_hist}\n")
            if st_["aborted"]:
                continue

            mdec = MDEC(fr["width"], fr["height"])
            mbs = mdec.decode(codes)
            if len(mbs) != 300:
                print(f"  !! decoded {len(mbs)}/300 macroblocks")
            pixels = tile_frame(mbs, fr["width"], fr["height"], rowmajor)
            write_png(os.path.join(outdir, "frames", f"py_{fi + 1:05d}.png"),
                      fr["width"], fr["height"], pixels)

    write_wav(os.path.join(outdir, "py_audio.wav"), [s for pair in audio for s in pair],
              xa_freq)
    print("stats:", stats_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
