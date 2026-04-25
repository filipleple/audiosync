#!/usr/bin/env python3
"""
Generate synthetic SMPTE LTC test audio tracks at varying quality levels.

Output: stereo WAV, 44100 Hz, 25 fps.
  CH1 (L): reference LTC @ 01:00:00:00
  CH2 (R): same LTC + 2-frame offset (= 80 ms), with increasing degradation.

Tracks:
  01_clean.wav               - perfect signal            → VALID,   Q > 0.85
  02_snr20dB.wav             - light noise               → VALID
  03_snr10dB.wav             - moderate noise            → VALID / SUSPECT
  04_snr6dB_dropouts.wav     - SNR 6 dB + dropouts       → SUSPECT
  05_snr3dB_heavy.wav        - SNR 3 dB + heavy dropouts → FAIL / fallback
  06_noise_floor.wav         - undecodable               → FAIL, fallback=YES
  07_gradual_degradation.wav - 60 s ramp clean → noise floor

Usage:
  python3 gen_test_tracks.py
"""

import struct
import math
import random
import wave
import os

SAMPLE_RATE  = 44100
FPS          = 25
DURATION_S   = 15       # seconds per track
CH2_OFFSET_F = 2        # ch2 is 2 frames ahead of ch1  (= 80 ms at 25 fps)

# ---------------------------------------------------------------------------
# LTC encoding helpers
# ---------------------------------------------------------------------------

def encode_ltc_frame(h, m, s, f):
    """Return 80-bit LTC frame as list of ints (0 or 1), bit-0 first."""
    bits = [0] * 80

    fu, ft = f % 10, f // 10
    for i in range(4): bits[i]     = (fu >> i) & 1   # bits 0-3  frame units
    for i in range(2): bits[8 + i] = (ft >> i) & 1   # bits 8-9  frame tens

    su, st = s % 10, s // 10
    for i in range(4): bits[16 + i] = (su >> i) & 1  # bits 16-19 sec units
    for i in range(3): bits[24 + i] = (st >> i) & 1  # bits 24-26 sec tens

    mu, mt = m % 10, m // 10
    for i in range(4): bits[32 + i] = (mu >> i) & 1  # bits 32-35 min units
    for i in range(3): bits[40 + i] = (mt >> i) & 1  # bits 40-42 min tens

    hu, ht = h % 10, h // 10
    for i in range(4): bits[48 + i] = (hu >> i) & 1  # bits 48-51 hr units
    for i in range(2): bits[56 + i] = (ht >> i) & 1  # bits 56-57 hr tens

    # Sync word bits 64-79: 0011111111111101
    for i, b in enumerate([0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1]):
        bits[64 + i] = b

    return bits


def tc_advance(h, m, s, f, n, fps):
    """Advance timecode (h,m,s,f) by n frames."""
    total = h * 3600 * fps + m * 60 * fps + s * fps + f + n
    f2 = total % fps;  total //= fps
    s2 = total % 60;   total //= 60
    m2 = total % 60;   h2 = (total // 60) % 24
    return h2, m2, s2, f2


def generate_ltc(n_frames, start_tc, fps=FPS, sr=SAMPLE_RATE):
    """
    Generate pure LTC audio (Biphase Mark Code) as a list of float32.
    Values are in {-1.0, +1.0}.  Length = n_frames * sr // fps samples.

    The fractional half-pulse (sr/fps/160) is distributed across the stream
    using a running integer accumulator to avoid per-frame drift.
    """
    half_pulse = sr / fps / 160.0
    samples    = []
    polarity   = 1.0
    t          = 0.0
    tc         = start_tc

    for _ in range(n_frames):
        bits = encode_ltc_frame(*tc)
        for bit in bits:
            # Mandatory cell-start transition
            polarity = -polarity
            t_prev   = t;  t += half_pulse
            samples.extend([polarity] * (int(t) - int(t_prev)))

            # Optional mid-cell transition for '1' bit
            if bit == 1:
                polarity = -polarity
            t_prev = t;  t += half_pulse
            samples.extend([polarity] * (int(t) - int(t_prev)))

        tc = tc_advance(*tc, 1, fps)

    return samples


# ---------------------------------------------------------------------------
# Degradation helpers
# ---------------------------------------------------------------------------

def add_noise(samples, snr_db):
    """Add white Gaussian noise at the requested SNR in dB."""
    sig_rms   = math.sqrt(sum(x * x for x in samples) / len(samples)) or 1.0
    noise_rms = sig_rms / (10 ** (snr_db / 20.0))
    return [s + random.gauss(0.0, noise_rms) for s in samples]


def add_dropouts(samples, sr, rate_per_sec, avg_dur_ms):
    """
    Randomly zero-out bursts (exponential duration, Poisson arrivals).
    rate_per_sec : expected number of dropout events per second
    avg_dur_ms   : mean dropout length in ms
    """
    result          = list(samples)
    avg_dur_smp     = max(1, int(avg_dur_ms / 1000.0 * sr))
    prob_per_sample = rate_per_sec / sr
    i = 0
    while i < len(result):
        if random.random() < prob_per_sample:
            dur = max(1, int(random.expovariate(1.0 / avg_dur_smp)))
            dur = min(dur, avg_dur_smp * 6)
            for j in range(i, min(i + dur, len(result))):
                result[j] = 0.0
            i += dur
        else:
            i += 1
    return result


def scale(samples, amp):
    return [s * amp for s in samples]


def clip_and_quantise(samples):
    out = bytearray()
    for s in samples:
        v = max(-1.0, min(1.0, s))
        out += struct.pack('<h', int(v * 32767))
    return out


# ---------------------------------------------------------------------------
# WAV writer
# ---------------------------------------------------------------------------

def write_wav(path, ch_l, ch_r, sr=SAMPLE_RATE):
    assert len(ch_l) == len(ch_r), f"Channel length mismatch: {len(ch_l)} vs {len(ch_r)}"
    with wave.open(path, 'w') as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(sr)
        # interleave L/R
        data = bytearray()
        for l, r in zip(ch_l, ch_r):
            lv = max(-1.0, min(1.0, l));  rv = max(-1.0, min(1.0, r))
            data += struct.pack('<hh', int(lv * 32767), int(rv * 32767))
        wf.writeframes(bytes(data))
    dur = len(ch_l) / sr
    size_kb = os.path.getsize(path) // 1024
    print(f"  {os.path.basename(path):40s}  {dur:5.1f}s  {size_kb:6d} KB")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    random.seed(42)

    sr       = SAMPLE_RATE
    fps      = FPS
    dur      = DURATION_S
    n_frames = dur * fps

    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_tracks')
    os.makedirs(out_dir, exist_ok=True)

    tc1 = (1, 0, 0, 0)
    tc2 = tc_advance(*tc1, CH2_OFFSET_F, fps)

    print(f"Sample rate : {sr} Hz")
    print(f"FPS         : {fps}")
    print(f"Duration    : {dur}s per track ({n_frames} frames)")
    print(f"TC1 start   : {tc1[0]:02d}:{tc1[1]:02d}:{tc1[2]:02d}:{tc1[3]:02d}")
    print(f"TC2 start   : {tc2[0]:02d}:{tc2[1]:02d}:{tc2[2]:02d}:{tc2[3]:02d}  "
          f"(+{CH2_OFFSET_F} frames = {CH2_OFFSET_F*1000//fps} ms)")
    print(f"\nGenerating base LTC streams ({n_frames} frames each)...")

    base1 = generate_ltc(n_frames, tc1, fps, sr)
    base2 = generate_ltc(n_frames, tc2, fps, sr)
    print(f"  CH1: {len(base1)} samples | CH2: {len(base2)} samples\n")

    print(f"{'Filename':40s}  {'Dur':>5}  {'Size':>7}    Notes")
    print("-" * 72)

    # ------------------------------------------------------------------ 01 --
    # Clean signal - should lock immediately, VALID throughout
    c1 = scale(base1, 0.70)
    c2 = scale(base2, 0.70)
    write_wav(os.path.join(out_dir, '01_clean.wav'), c1, c2, sr)

    # ------------------------------------------------------------------ 02 --
    # SNR 20 dB - very light noise, barely perceptible
    c1 = add_noise(scale(base1, 0.70), 20)
    c2 = add_noise(scale(base2, 0.70), 20)
    write_wav(os.path.join(out_dir, '02_snr20dB.wav'), c1, c2, sr)

    # ------------------------------------------------------------------ 03 --
    # SNR 10 dB - audible noise, decoder should still hold VALID or border
    c1 = add_noise(scale(base1, 0.70), 10)
    c2 = add_noise(scale(base2, 0.70), 10)
    write_wav(os.path.join(out_dir, '03_snr10dB.wav'), c1, c2, sr)

    # ------------------------------------------------------------------ 04 --
    # SNR 6 dB + 0.5 dropouts/sec (~30 ms each) - SUSPECT expected
    c1 = add_noise(scale(base1, 0.50),  6)
    c2 = add_noise(scale(base2, 0.50),  6)
    c1 = add_dropouts(c1, sr, rate_per_sec=0.5, avg_dur_ms=30)
    c2 = add_dropouts(c2, sr, rate_per_sec=0.5, avg_dur_ms=30)
    write_wav(os.path.join(out_dir, '04_snr6dB_dropouts.wav'), c1, c2, sr)

    # ------------------------------------------------------------------ 05 --
    # SNR 3 dB + 2 dropouts/sec (~50 ms each) - FAIL on most windows
    c1 = add_noise(scale(base1, 0.30),  3)
    c2 = add_noise(scale(base2, 0.30),  3)
    c1 = add_dropouts(c1, sr, rate_per_sec=2.0, avg_dur_ms=50)
    c2 = add_dropouts(c2, sr, rate_per_sec=2.0, avg_dur_ms=50)
    write_wav(os.path.join(out_dir, '05_snr3dB_heavy.wav'), c1, c2, sr)

    # ------------------------------------------------------------------ 06 --
    # At noise floor: 0 dB SNR + constant bursts - undecodable, fallback=YES
    c1 = add_noise(scale(base1, 0.08),  0)
    c2 = add_noise(scale(base2, 0.08),  0)
    c1 = add_dropouts(c1, sr, rate_per_sec=5.0, avg_dur_ms=80)
    c2 = add_dropouts(c2, sr, rate_per_sec=5.0, avg_dur_ms=80)
    write_wav(os.path.join(out_dir, '06_noise_floor.wav'), c1, c2, sr)

    # ------------------------------------------------------------------ 07 --
    # Gradual ramp: 60 s with 6 × 10s sections, clean → noise floor
    print()
    print("Generating 60s gradual degradation track...")
    ramp_frames = 60 * fps
    r1 = generate_ltc(ramp_frames, tc1, fps, sr)
    r2 = generate_ltc(ramp_frames, tc2, fps, sr)

    seg        = len(r1) // 6    # 10 s = 441,000 samples = 250 frames exactly
    ramp_c1    = []
    ramp_c2    = []

    # (amplitude, snr_db or None, dropout_rate/s, dropout_avg_ms)
    segments = [
        (0.70, None, 0.0,  0),   # 00-10s  clean
        (0.70,   20, 0.0,  0),   # 10-20s  SNR 20 dB
        (0.60,   10, 0.0,  0),   # 20-30s  SNR 10 dB
        (0.50,    6, 0.5, 30),   # 30-40s  SNR 6 dB + dropouts
        (0.30,    3, 2.0, 50),   # 40-50s  SNR 3 dB + heavy dropouts
        (0.08,    0, 5.0, 80),   # 50-60s  noise floor - fallback
    ]
    labels = [
        "clean",
        "SNR 20 dB",
        "SNR 10 dB",
        "SNR 6 dB + dropouts",
        "SNR 3 dB + heavy dropouts",
        "noise floor (fallback)",
    ]

    for i, ((amp, snr, dr, dd), lbl) in enumerate(zip(segments, labels)):
        s1 = list(r1[i * seg : (i + 1) * seg])
        s2 = list(r2[i * seg : (i + 1) * seg])
        s1 = scale(s1, amp);  s2 = scale(s2, amp)
        if snr is not None:
            s1 = add_noise(s1, snr);  s2 = add_noise(s2, snr)
        if dr > 0:
            s1 = add_dropouts(s1, sr, dr, dd)
            s2 = add_dropouts(s2, sr, dr, dd)
        ramp_c1.extend(s1);  ramp_c2.extend(s2)
        print(f"  {i*10:2d}s-{(i+1)*10:2d}s  {lbl}")

    write_wav(os.path.join(out_dir, '07_gradual_degradation.wav'), ramp_c1, ramp_c2, sr)

    print(f"\nAll files written to: {out_dir}/")
    print()
    print("Expected Q_LTC per track (rough guide):")
    print("  01_clean              VALID    Q ≈ 0.90+")
    print("  02_snr20dB            VALID    Q ≈ 0.85+")
    print("  03_snr10dB            VALID    Q ≈ 0.70–0.85")
    print("  04_snr6dB_dropouts    SUSPECT  Q ≈ 0.50–0.70")
    print("  05_snr3dB_heavy       FAIL     Q ≈ 0.20–0.50")
    print("  06_noise_floor        FAIL     Q ≈ 0.00–0.20  fallback=YES")
    print("  07_gradual_degradation - all of the above in one 60s file")


if __name__ == '__main__':
    main()
