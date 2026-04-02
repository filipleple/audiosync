#!/usr/bin/env python3
"""
Generate a mixed audio + LTC test track from tedtalk_full.mp3.

Structure: 60 seconds, 6 x 10s segments.
  CH1: speech[0..60s]        + LTC @ 01:00:00:00 (fading)
  CH2: speech[80ms..60s+80ms] + LTC @ 01:00:00:02 (fading, same level)

The LTC level drops each segment while speech stays constant.
By the end, LTC is gone and only the audio fallback can maintain sync.

Segment map:
  0-10s   LTC amp 0.60  -> speech amp 0.25  -> LTC clearly dominant   -> VALID
  10-20s  LTC amp 0.35  -> speech amp 0.25  -> LTC still readable      -> VALID/SUSPECT
  20-30s  LTC amp 0.15  -> speech amp 0.25  -> LTC fighting speech     -> SUSPECT/FAIL
  30-40s  LTC amp 0.05  -> speech amp 0.25  -> LTC nearly inaudible    -> FAIL
  40-50s  LTC amp 0.01  -> speech amp 0.25  -> LTC below noise floor   -> FAIL, audio fallback active
  50-60s  LTC amp 0.00  -> speech amp 0.25  -> LTC completely gone     -> FAIL, audio fallback only

Output: test_tracks/08_audio_ltc_fading.wav

Dependencies: ffmpeg binary at workspace/linux/ffmpeg (installed by static-ffmpeg).
              Falls back to system ffmpeg if that path is not present.
"""

import os
import struct
import math
import random
import wave
import subprocess
import sys

SAMPLE_RATE  = 44100
FPS          = 25
DURATION_S   = 60          # seconds to extract from the TED talk
CH2_OFFSET_F = 2           # CH2 is 2 frames ahead of CH1 (= 80 ms at 25 fps)
SPEECH_AMP   = 0.20        # speech level held constant throughout
SNIPPET_START_S = 15       # skip opening silence / applause

FFMPEG = None

def find_ffmpeg():
    global FFMPEG
    candidates = [
        os.path.join(os.path.dirname(os.path.abspath(__file__)), 'linux', 'ffmpeg'),
        'ffmpeg',
    ]
    for c in candidates:
        try:
            r = subprocess.run([c, '-version'], capture_output=True, timeout=5)
            if r.returncode == 0:
                FFMPEG = c
                return
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass
    print("ERROR: ffmpeg not found. Install via: pip3 install static-ffmpeg", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# LTC helpers (same as gen_test_tracks.py)
# ---------------------------------------------------------------------------

def encode_ltc_frame(h, m, s, f):
    bits = [0] * 80
    fu, ft = f % 10, f // 10
    for i in range(4): bits[i]     = (fu >> i) & 1
    for i in range(2): bits[8 + i] = (ft >> i) & 1
    su, st = s % 10, s // 10
    for i in range(4): bits[16 + i] = (su >> i) & 1
    for i in range(3): bits[24 + i] = (st >> i) & 1
    mu, mt = m % 10, m // 10
    for i in range(4): bits[32 + i] = (mu >> i) & 1
    for i in range(3): bits[40 + i] = (mt >> i) & 1
    hu, ht = h % 10, h // 10
    for i in range(4): bits[48 + i] = (hu >> i) & 1
    for i in range(2): bits[56 + i] = (ht >> i) & 1
    for i, b in enumerate([0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1]):
        bits[64 + i] = b
    return bits


def tc_advance(h, m, s, f, n, fps):
    total = h * 3600 * fps + m * 60 * fps + s * fps + f + n
    f2 = total % fps;  total //= fps
    s2 = total % 60;   total //= 60
    m2 = total % 60;   h2 = (total // 60) % 24
    return h2, m2, s2, f2


def generate_ltc(n_frames, start_tc, fps=FPS, sr=SAMPLE_RATE):
    half_pulse = sr / fps / 160.0
    samples    = []
    polarity   = 1.0
    t          = 0.0
    tc         = start_tc
    for _ in range(n_frames):
        bits = encode_ltc_frame(*tc)
        for bit in bits:
            polarity = -polarity
            t_prev   = t;  t += half_pulse
            samples.extend([polarity] * (int(t) - int(t_prev)))
            if bit == 1:
                polarity = -polarity
            t_prev = t;  t += half_pulse
            samples.extend([polarity] * (int(t) - int(t_prev)))
        tc = tc_advance(*tc, 1, fps)
    return samples


# ---------------------------------------------------------------------------
# Audio loading via ffmpeg
# ---------------------------------------------------------------------------

def load_audio_mono_f32(path, start_s, duration_s, sr=SAMPLE_RATE):
    """Decode audio to mono float32 via ffmpeg. Returns list of floats."""
    raw_path = '/tmp/_gen_audio_tmp.raw'
    cmd = [
        FFMPEG,
        '-y',
        '-ss', str(start_s),
        '-t',  str(duration_s),
        '-i',  path,
        '-ar', str(sr),
        '-ac', '1',
        '-f',  'f32le',
        raw_path,
    ]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        print("ffmpeg error:", r.stderr.decode()[-500:], file=sys.stderr)
        sys.exit(1)
    data = open(raw_path, 'rb').read()
    samples = list(struct.unpack(f'{len(data)//4}f', data))
    return samples


def normalise(samples, target_peak=1.0):
    """Scale so the peak is target_peak."""
    peak = max(abs(s) for s in samples) or 1.0
    scale = target_peak / peak
    return [s * scale for s in samples], scale


# ---------------------------------------------------------------------------
# WAV writer (same as gen_test_tracks.py)
# ---------------------------------------------------------------------------

def write_wav(path, ch_l, ch_r, sr=SAMPLE_RATE):
    n = min(len(ch_l), len(ch_r))
    with wave.open(path, 'w') as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(sr)
        data = bytearray()
        for i in range(n):
            lv = max(-1.0, min(1.0, ch_l[i]))
            rv = max(-1.0, min(1.0, ch_r[i]))
            data += struct.pack('<hh', int(lv * 32767), int(rv * 32767))
        wf.writeframes(bytes(data))
    size_kb = os.path.getsize(path) // 1024
    print(f"  {os.path.basename(path):45s}  {n/sr:.1f}s  {size_kb:6d} KB")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    random.seed(42)

    find_ffmpeg()
    print(f"ffmpeg: {FFMPEG}")

    mp3_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            'test_tracks', 'tedtalk_full.mp3')
    if not os.path.exists(mp3_path):
        print(f"ERROR: {mp3_path} not found", file=sys.stderr)
        sys.exit(1)

    sr        = SAMPLE_RATE
    fps       = FPS
    offset_f  = CH2_OFFSET_F
    offset_ms = offset_f * 1000 // fps    # 80 ms
    offset_s  = offset_ms / 1000.0

    print(f"\nLoading speech audio from {os.path.basename(mp3_path)} ...")
    print(f"  snippet start : {SNIPPET_START_S}s")
    print(f"  snippet length: {DURATION_S}s")

    # Load enough audio for both channels:
    # CH1 needs DURATION_S, CH2 needs DURATION_S starting offset_s later.
    # So load DURATION_S + offset_s total, then slice.
    raw = load_audio_mono_f32(mp3_path, SNIPPET_START_S,
                               DURATION_S + offset_s + 0.5, sr)

    # Normalise to 1.0 peak, then scale to SPEECH_AMP
    raw, _ = normalise(raw)
    raw = [s * SPEECH_AMP for s in raw]

    offset_smp = int(round(offset_s * sr))   # = 3528 samples = 80 ms
    n_main     = DURATION_S * sr              # samples for one full channel

    speech1 = raw[:n_main]
    speech2 = raw[offset_smp : offset_smp + n_main]

    print(f"  CH1: samples[0..{n_main}]  = {n_main/sr:.1f}s")
    print(f"  CH2: samples[{offset_smp}..{offset_smp+n_main}]"
          f"  = {n_main/sr:.1f}s  (offset +{offset_ms}ms)")

    # Generate full-length LTC for both channels
    n_frames = DURATION_S * fps
    tc1      = (1, 0, 0, 0)
    tc2      = tc_advance(*tc1, offset_f, fps)

    print(f"\nGenerating LTC ({n_frames} frames @ {fps} fps) ...")
    ltc1 = generate_ltc(n_frames, tc1, fps, sr)
    ltc2 = generate_ltc(n_frames, tc2, fps, sr)
    # Trim to min length (fractional half-pulse accumulation can produce
    # one fewer sample than n_main due to float64 rounding of 11.025)
    n_main = min(n_main, len(ltc1), len(ltc2), len(speech1), len(speech2))
    ltc1 = ltc1[:n_main]
    ltc2 = ltc2[:n_main]
    speech1 = speech1[:n_main]
    speech2 = speech2[:n_main]
    print(f"  TC1: {tc1[0]:02d}:{tc1[1]:02d}:{tc1[2]:02d}:{tc1[3]:02d}")
    print(f"  TC2: {tc2[0]:02d}:{tc2[1]:02d}:{tc2[2]:02d}:{tc2[3]:02d}"
          f"  (+{offset_f} frames = +{offset_ms}ms)")

    # Segment map: (ltc_amp, label, expected_state)
    # Speech amplitude is fixed at SPEECH_AMP throughout.
    #
    # Key design principle: avoid the "danger zone" where LTC is close to speech
    # level (the decoder fails but the fallback hasn't converged yet, so nothing
    # works).  Instead keep LTC dominant for the first half, then cut it sharply
    # so the audio fallback has a clean transition to work with.
    #
    # Speech at 0.20 peak.  LTC needs to be >= ~3x speech (>= ~0.60) to reliably
    # dominate the adaptive threshold.  Below ~2x (0.40) transitions start being
    # cancelled by speech peaks and the decoder degrades rapidly.
    segments = [
        (0.70, "LTC 0.70 / speech 0.20  - LTC dominant, clean lock",    "VALID"),
        (0.70, "LTC 0.70 / speech 0.20  - LTC dominant, continued",     "VALID"),
        (0.70, "LTC 0.70 / speech 0.20  - LTC dominant, audio fallback accumulating", "VALID"),
        (0.00, "LTC off  / speech 0.20  - sharp cut, audio fallback taking over",     "FAIL -> AUD"),
        (0.00, "LTC off  / speech 0.20  - audio fallback active",        "FAIL, src=AUD"),
        (0.00, "LTC off  / speech 0.20  - audio fallback holding",       "FAIL, src=AUD"),
    ]

    seg_smp = n_main // len(segments)   # samples per segment

    ch1 = []
    ch2 = []

    print(f"\nMixing segments ({seg_smp/sr:.1f}s each) ...")
    print(f"  {'Start':>5}  {'LTC amp':>8}  {'State'}")
    print("  " + "-" * 55)

    for i, (ltc_amp, label, state) in enumerate(segments):
        a  = i * seg_smp
        b  = (i + 1) * seg_smp

        s1 = [speech1[j] + ltc1[j] * ltc_amp for j in range(a, b)]
        s2 = [speech2[j] + ltc2[j] * ltc_amp for j in range(a, b)]

        ch1.extend(s1)
        ch2.extend(s2)
        print(f"  {i*10:4d}s  amp={ltc_amp:.2f}    {label}")

    out_dir  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_tracks')
    out_path = os.path.join(out_dir, '08_audio_ltc_fading.wav')

    print(f"\nWriting output ...")
    write_wav(out_path, ch1, ch2, sr)
    print(f"\nDone: {out_path}")
    print()
    print("Expected plugin behaviour:")
    print(f"   0-10s  VALID    LTC dominant, fast lock,        Fallback=no   src=LTC")
    print(f"  10-20s  VALID    LTC dominant, audio accumulating, Fallback=no  src=LTC")
    print(f"  20-30s  VALID    LTC dominant, fallback ready,   Fallback=no   src=LTC")
    print(f"  30-40s  FAIL     LTC cut, fallback convergence,  Fallback=YES  src=NONE->AUD")
    print(f"  40-50s  FAIL     audio fallback active,          Fallback=YES  src=AUD")
    print(f"  50-60s  FAIL     audio fallback holding,         Fallback=YES  src=AUD")
    print()
    print(f"  Audio fallback target: +{offset_ms}ms (should match LTC value from first 30s)")


if __name__ == '__main__':
    main()
