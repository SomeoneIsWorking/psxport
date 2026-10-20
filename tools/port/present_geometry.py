#!/usr/bin/env python3
"""Is the presented picture the right shape?

Finds the non-black content band of a capture and compares the picture rectangle's aspect with the
expected one. PSX horizontal modes all scan into a 4:3 screen, so 4:3 is expected unless a
widescreen mode is active (`--expect 16:9`).

    uv run --frozen python external/psxport/tools/port/present_geometry.py shot.png [--expect 16:9]
        [--active 512x224 --display 512x240 | --guest-frame vram.png --display 512x240]

Black inside the picture and a letterbox bar look the same, so without the guest's drawn extent the
aspect is the interval [band_w/sink_h, sink_w/band_h]; a straddling interval is refused (rc 3).
`--active`/`--display` (or `--guest-frame`, measured with the same band code) give the exact rect,
assuming the present is a uniform scale of the guest display rect.

Exit codes (max over inputs): 0 OK, 1 stretched or squashed, 2 refused (black, unreadable or
inconsistent --active), 3 ambiguous.
"""
import sys

from PIL import Image, UnidentifiedImageError


class Unreadable(Exception):
    """An input this tool will not guess at."""


def read_image(path):
    """(width, height, flat RGB bytes) of a PNG or PPM capture."""
    try:
        with Image.open(path) as image:
            rgb = image.convert('RGB')
    except (OSError, UnidentifiedImageError) as error:
        raise Unreadable('%s: %s' % (path, error)) from error
    return rgb.width, rgb.height, rgb.tobytes()


def content_band(w, h, px, thresh=0):
    """Bounding box (x, y, w, h) of pixels brighter than `thresh` over every row, or None."""
    tbl = None if thresh == 0 else bytes(0 if v <= thresh else 1 for v in range(256))
    x0, x1, y0, y1 = w, -1, -1, -1
    stride = w * 3
    for y in range(h):
        row = px[y * stride:(y + 1) * stride]
        if tbl is not None:
            row = row.translate(tbl)
        end = len(row.rstrip(b'\x00'))
        if end == 0:
            continue                                   # entirely black row
        lead = len(row) - len(row.lstrip(b'\x00'))
        if y0 < 0:
            y0 = y
        y1 = y
        fx, lx = lead // 3, (end - 1) // 3
        if fx < x0:
            x0 = fx
        if lx > x1:
            x1 = lx
    if y1 < 0:
        return None
    return x0, y0, x1 - x0 + 1, y1 - y0 + 1


def parse_aspect(s):
    if ':' in s:
        a, b = s.split(':', 1)
        return float(a) / float(b)
    return float(s)


def parse_wh(s, what):
    t = s.lower().replace('*', 'x').split('x')
    if len(t) != 2:
        raise Unreadable('%s wants WxH like 512x224, got %r' % (what, s))
    return int(t[0]), int(t[1])


def verdict_word(ratio):
    if ratio > 1:
        return 'STRETCHED %.3fx WIDE' % ratio
    return 'SQUASHED %.3fx (too narrow)' % ratio


def report(path, w, h, px, expect, expect_s, tol, active, display, out=print):
    """Returns the exit code for this one file and prints the whole report. `active`/`display` are
    (w, h) in GUEST pixels or None."""
    band = content_band(w, h, px)
    out('%s' % path)
    out('  sink        %dx%d' % (w, h))
    if band is None:
        out('  content     NONE — every pixel is black (scanned all %d rows x %d cols)' % (h, w))
        out('  verdict     REFUSED: entirely black, no measurable geometry. This is not a pass and '
            'not a fail.')
        return 2
    bx, by, bw, bh = band
    out('  content     x %d..%d (%d wide), y %d..%d (%d tall)'
        % (bx, bx + bw - 1, bw, by, by + bh - 1, bh))
    margins = []
    if by > 0 or by + bh < h:
        margins.append('top/bottom %d+%d px' % (by, h - (by + bh)))
    if bx > 0 or bx + bw < w:
        margins.append('left/right %d+%d px' % (bx, w - (bx + bw)))
    out('  margins     %s' % (', '.join(margins) if margins else 'none (band fills the sink)'))

    lo_ok, hi_ok = expect * (1.0 - tol), expect * (1.0 + tol)

    if active and display:
        aw, ah = active
        dw, dh = display
        if aw <= 0 or ah <= 0 or dw <= 0 or dh <= 0:
            out('  verdict     REFUSED: --active/--display must be positive.')
            return 2
        if aw > dw or ah > dh:
            out('  verdict     REFUSED: drawn extent %dx%d does not fit the display rect %dx%d — '
                'the guest cannot draw outside its own display rect, so one of these is wrong.'
                % (aw, ah, dw, dh))
            return 2
        pw = bw * (dw / float(aw))
        ph = bh * (dh / float(ah))
        out('  guest       drawn %dx%d of display rect %dx%d  -> band is %.1f%% x %.1f%% of the '
            'picture' % (aw, ah, dw, dh, 100.0 * aw / dw, 100.0 * ah / dh))
        out('  picture     %.1f x %.1f  (band scaled up by the undrawn guest margin)' % (pw, ph))
        if pw > w + 1.5 or ph > h + 1.5:
            out('  verdict     REFUSED: that picture rect does not fit inside the %dx%d sink. The '
                '--active/--display you gave is inconsistent with this frame; refusing to report '
                'an impossible rectangle.' % (w, h))
            return 2
        got = pw / ph
        out('  aspect      %.3f:1   expected %s = %.3f:1' % (got, expect_s, expect))
        if lo_ok <= got <= hi_ok:
            out('  verdict     OK')
            return 0
        out('  verdict     %s' % verdict_word(got / expect))
        return 1

    # band <= picture <= sink bounds the aspect.
    lo = bw / float(h)          # widest possible picture height, narrowest possible width
    hi = w / float(bh)          # narrowest possible picture height, widest possible width
    out('  aspect      %.3f .. %.3f : 1   expected %s = %.3f:1'
        % (lo, hi, expect_s, expect))
    if lo > hi_ok or hi < lo_ok:
        got = lo if lo > hi_ok else hi
        why = ('' if not margins else
               '  (the ENTIRE range is off, so this holds however the black margins are apportioned '
               'between picture and letterbox)')
        out('  verdict     %s%s' % (verdict_word(got / expect), why))
        return 1
    if lo >= lo_ok and hi <= hi_ok:
        out('  verdict     OK')
        return 0
    naive = bw / float(bh)
    out('  verdict     REFUSED: AMBIGUOUS. The band has black margins, and black INSIDE the picture')
    out('              (the game drew it) is indistinguishable from black OUTSIDE it (a letterbox')
    out('              bar). The true aspect is anywhere in the range above, and that range spans')
    out('              the expected %.3f, so both "right" and "wrong" are consistent with these'
        % expect)
    out('              pixels. Band-only arithmetic says %.3f:1 = %.3fx, which is not trustworthy.'
        % (naive, naive / expect))
    out('              To get a verdict, tell it the frame\'s own drawn extent:')
    out('                --active <drawn WxH> --display <guest display WxH>')
    out('                --guest-frame <guest framebuffer dump> --display <guest display WxH>')
    return 3


def main():
    expect, expect_s, tol = 4.0 / 3.0, '4:3', 0.02
    active = display = guest_frame = None
    args, rest = [], list(sys.argv[1:])
    while rest:
        a = rest.pop(0)
        if a in ('--expect', '--tol', '--active', '--display', '--guest-frame'):
            if not rest:
                print('%s needs a value' % a); return 2
            v = rest.pop(0)
            if a == '--expect':
                expect_s, expect = v, parse_aspect(v)
            elif a == '--tol':
                tol = float(v)
            elif a == '--active':
                active = parse_wh(v, '--active')
            elif a == '--display':
                display = parse_wh(v, '--display')
            else:
                guest_frame = v
        elif a.startswith('--'):
            print('unknown option %s' % a); return 2
        else:
            args.append(a)
    if not args:
        print(__doc__)
        return 2

    if guest_frame:
        gw, gh, gpx = read_image(guest_frame)
        gb = content_band(gw, gh, gpx)
        if gb is None:
            print('%s: the guest frame is ENTIRELY BLACK, so it has no drawn extent to correct '
                  'with. REFUSING.' % guest_frame)
            return 2
        active = (gb[2], gb[3])
        if display is None:
            display = (gw, gh)
        print('guest frame  %s: %dx%d, drawn extent %dx%d at (%d,%d)  -> --active %dx%d'
              % (guest_frame, gw, gh, gb[2], gb[3], gb[0], gb[1], gb[2], gb[3]))
    if (active is None) != (display is None):
        print('--active and --display go together: --active is the DRAWN bbox and --display is the '
              'guest DISPLAY RECT, both in guest pixels. One without the other says nothing.')
        return 2

    rc = 0
    for path in args:
        w, h, px = read_image(path)
        rc = max(rc, report(path, w, h, px, expect, expect_s, tol, active, display))
    return rc


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Unreadable as e:
        print('REFUSED: %s' % e)
        sys.exit(2)
