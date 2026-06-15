#!/usr/bin/env python3
"""audio_differ — inspect / compare the port's SPU output WAVs against the oracle.

The port dumps its mixed 44.1 kHz stereo output with PSXPORT_WAV=path (spu_audio.c,
works headless under PSXPORT_NOAUDIO). This script is the listening/measuring end:

  compare.py stats  OURS.wav                 — duration, RMS/peak per channel, silence %,
                                                first non-silent time (catches "all zeros").
  compare.py diff   OURS.wav  ORACLE.wav      — align by cross-correlation, then report
                                                length/RMS/peak deltas + best-offset similarity.

Why cross-correlation align: our HLE port and the full-emulation oracle run at different
timings/states, so the two recordings are NOT sample-aligned (same trap as GPU frame
numbers). We slide one against the other to find the lag of peak correlation, then compare
the overlapped region. (For a sample-EXACT check use the SPU-input replay path instead —
ours == Beetle's spu.c, so identical inputs give identical output by construction.)

Pure stdlib (wave/array/math); no numpy required.
"""
import sys, wave, array, math


def load(path):
    w = wave.open(path, 'rb')
    if w.getsampwidth() != 2:
        raise SystemExit("%s: only 16-bit PCM supported" % path)
    n, ch, sr = w.getnframes(), w.getnchannels(), w.getframerate()
    a = array.array('h'); a.frombytes(w.readframes(n)); w.close()
    if ch == 2:
        L, R = a[0::2], a[1::2]
    else:
        L = a; R = a
    return L, R, sr


def rms(ch):
    if not ch:
        return 0.0
    return math.sqrt(sum(x * x for x in ch) / len(ch))


def peak(ch):
    return max((abs(x) for x in ch), default=0)


def mono(L, R):
    return array.array('i', (((L[i] + R[i]) >> 1) for i in range(len(L))))


def first_nonsilent(ch, sr, thresh=64):
    for i, v in enumerate(ch):
        if abs(v) > thresh:
            return i / sr
    return None


def stats(path):
    L, R, sr = load(path)
    dur = len(L) / sr
    print("%s" % path)
    print("  %.2f s  %d Hz  stereo  (%d frames)" % (dur, sr, len(L)))
    print("  RMS   L=%.1f  R=%.1f" % (rms(L), rms(R)))
    print("  peak  L=%d  R=%d" % (peak(L), peak(R)))
    m = mono(L, R)
    nz = sum(1 for v in m if abs(v) > 64)
    fns = first_nonsilent(m, sr)
    print("  non-silent: %.2f%% of frames; first audible at %s" %
          (100.0 * nz / max(1, len(m)), ("%.2f s" % fns) if fns is not None else "NEVER (silent)"))
    if peak(L) == 0 and peak(R) == 0:
        print("  *** WARNING: output is COMPLETELY SILENT (all zeros) ***")


def xcorr_best_lag(a, b, max_lag):
    """Coarse normalized cross-correlation; returns (lag, score) at the downsampled grid.
    Positive lag = b lags a (b starts `lag` samples later). O(max_lag * N/step)."""
    n = min(len(a), len(b))
    step = max(1, n // 20000)          # subsample for speed
    a2 = a[:n:step]; b2 = b[:n:step]
    lag_step = max(1, max_lag // step)
    best = (0, -1e30)
    na = math.sqrt(sum(x * x for x in a2)) or 1.0
    for lag in range(-lag_step, lag_step + 1):
        s = c = 0.0
        for i in range(len(a2)):
            j = i + lag
            if 0 <= j < len(b2):
                s += a2[i] * b2[j]; c += 1
        if c:
            nb = math.sqrt(sum(b2[i + lag] ** 2 for i in range(len(a2)) if 0 <= i + lag < len(b2))) or 1.0
            score = s / (na * nb)
            if score > best[1]:
                best = (lag * step, score)
    return best


def diff(p_ours, p_oracle):
    Lo, Ro, sro = load(p_ours)
    Lr, Rr, srr = load(p_oracle)
    if sro != srr:
        print("rate mismatch: %d vs %d Hz" % (sro, srr))
    print("ours  : ", end=""); stats(p_ours)
    print("oracle: ", end=""); stats(p_oracle)
    mo, mr = mono(Lo, Ro), mono(Lr, Rr)
    if peak(Lo) == peak(Ro) == 0:
        print("\nours is silent — nothing to align; the gap IS the whole signal.")
        print("oracle RMS (mono) = %.1f  -> ours is missing ~100%% of the audio." % rms(mr))
        return
    lag, score = xcorr_best_lag(mo, mr, max_lag=sro)   # search +-1 s
    print("\nbest alignment: oracle lags ours by %d samples (%.3f s), corr=%.3f" %
          (lag, lag / sro, score))
    print("RMS ratio ours/oracle = %.3f (1.0 = same loudness)" % (rms(mo) / (rms(mr) or 1)))


def main(argv):
    if len(argv) >= 3 and argv[1] == "stats":
        stats(argv[2])
    elif len(argv) >= 4 and argv[1] == "diff":
        diff(argv[2], argv[3])
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main(sys.argv)
