# Writeup Notes — AudioSync VST3 Plugin

Structured reference for the 4–8 page SDC writeup.  Every claim here is
verified against the source code as of April 2026.

---

## 1. Problem Statement

Multi-camera 360° video rigs record audio on separate devices (field recorders,
boom mics, camera built-ins). Each device has an independent clock; even if
they start simultaneously, sample-clock drift accumulates over a shoot.
SMPTE LTC (Linear Time Code) is a standard solution: a continuous audio
timecode signal is laid down on one channel of every recorder.  In post-
production the LTC signals are decoded, the timecode offsets are compared, and
compensating delays are applied.

**The problem with existing workflows:**

- LTC is decoded by separate utilities outside the DAW.
- Delay compensation requires manual per-track editing.
- LTC quality is not monitored; a corrupted signal causes silent errors.
- There is no automatic fallback when LTC degrades.

---

## 2. Solution Overview

A single VST3 plugin binary (C++17, JUCE framework) that runs inside the DAW
in two roles:

| Role | Where | What it does |
|------|--------|--------------|
| **Master** | Reference LTC track | Decodes LTC, extracts novelty curve, writes to shared memory. Audio passes through unchanged. |
| **Slave** (×1–8) | Each track to delay | Reads master SM, decodes own LTC, computes delay, applies it. |

The master-slave pair communicates via **OS named shared memory** — no audio
routing between DAW tracks, works with any host that supports VST3.

Key differentiators vs. doing this manually:
- Frame-accurate delay applied in real time.
- Automatic fallback to audio correlation (NCC) when LTC degrades.
- 7-metric LTC quality score with visual diagnostics.
- Up to 8 independent slave tracks per group.

---

## 3. System Architecture

### 3.1 IPC — Named Shared Memory

```
OS named shared memory  "/AUTOSYNC2_<groupName>"
                                    │
                   ┌────────────────┴────────────────┐
           MasterSlot (≈8 KB)                 SlaveSlot × 8 (≈112 bytes each)
           ─────────────────                  ─────────────────────────────────
           tc_ref_ms                          tc_self_ms
           ref_decode_sample                  dt_ltc_ms, dt_aud_ms, conf_aud
           Q_ref, ltc_state, locked           active_delay_ms, fusion_src
           nov_ref[2000] (log-novelty ring)    connected, holding
           nov_writePos, nov_framesFilled
           nov_anchor_sample
```

Lock-free synchronization via **seqcount pattern**:
- Writer increments `writeSeq` to odd before write, to even after.
- Reader retries if `writeSeq` is odd or changes between two reads.
- No mutex → no priority inversion on DAW real-time thread.

### 3.2 Processing Pipeline (Slave, per sample)

```
Input samples
      │
      ├─► handleTimecode(ltcSample, chnl1_in)   ← LTC decoder
      │        │
      │        ├─► BMC demodulation → bit buffer
      │        ├─► Sync word match + BCD decode
      │        ├─► BCD range validation (reject impossible SMPTE values)
      │        ├─► Temporal-coherence gate (LOCK_N=10 / UNLOCK_M=20)
      │        └─► Quality window accumulation → Q_LTC (every 0.5 s)
      │
      ├─► pushAudioAnalysisSample(ltcSample, sceneSample)
      │        │
      │        ├─► Bandpass HPF 100 Hz + LPF 3500 Hz
      │        ├─► Log-energy novelty → novelty1[ring]
      │        ├─► Stereo-LTC guard: suppress novelty2 proportional to Q_LTC
      │        ├─► Noise-floor follower + activity gate
      │        └─► every 200 ms: estimateAudioFallbackOffset()
      │                                  └─► fuseLtcAndAudioFallback()
      │
      ├─► every 100 ms: read MasterSlot from SM
      │        ├─► sample-accurate d_ms = (A_slave − A_master)/sr × 1000
      │        │                        − (TC_self_ms − TC_ref_ms)
      │        ├─► hysteresis / bothLocked gate before committing d_ms
      │        └─► linearise master novelty ring → masterNoveltyRef
      │
      └─► delay engine: apply |targetMs| to both channels
```

---

## 4. LTC Decoder

### 4.1 BMC Demodulation

SMPTE LTC uses **Bi-Phase Mark Code**: a `1` bit has a transition mid-cell; a
`0` bit has no mid-cell transition.  The decoder detects zero-crossings
against an adaptive envelope threshold (exponential follower, τ ≈ 100 ms) and
counts inter-transition intervals against the expected pulse size:

```
pulsesize = sampleRate / fps / 160
```

A 10 ms DC-blocking IIR removes offset before threshold comparison.

### 4.2 Sync Word and BCD Extraction

The 80-bit frame consists of 64 data bits (timecode in BCD) + 16-bit sync
word `0x3FFD` at bits 64–79.  After each bit commit, the circular 80-bit
buffer is checked for the sync pattern.  On match:
- BCD digits are extracted for frames, seconds, minutes, hours.
- Range checked: `frms < fps`, `scnds < 60`, `mnts < 60`, `hrs < 24`.
  Out-of-range frames are silently rejected and `rejected_frames_count` is
  incremented without updating decoder state.

### 4.3 Temporal-Coherence Gate

To guard against speech transients that happen to satisfy both the sync-word
and BCD range checks:

- **Unlocked:** accepts any BCD-valid frame, counts consecutive +1..+3 forward
  steps toward lock.
- **Locked** (after `LOCK_N = 10` clean steps): requires each new frame to be
  within `[−1.5, +3.5]` frame-durations of the last accepted frame; otherwise
  rejects and counts toward unlock.
- **Unlock** (after `UNLOCK_M = 20` consecutive rejects): drops to unlocked
  for re-acquisition.

The `locked` flag is shared to SM so the slave's `d_ms` update is gated on
both decoders being locked (`bothLocked` check).

---

## 5. LTC Quality Score (Q_LTC)

Seven sub-metrics computed every `W_SIZE = 22050` samples (~0.5 s):

| Metric | Weight | What it measures |
|--------|--------|-----------------|
| `q_lock_ratio` | 0.25 | Decoded frames / expected frames in window |
| `q_continuity_score` | 0.20 | Fraction of consecutive frame pairs with +1 step |
| `q_pulse_consistency` | 0.15 | Deviation of measured BMC pulse width from ideal |
| `q_transition_reliability` | 0.15 | Fraction of transitions with valid inter-transition interval |
| `q_signal_strength` | 0.10 | Mean envelope level relative to noise floor constant |
| `q_sync_word_rate` | 0.10 | Sync word hits / expected frames |
| `q_fps_plausibility` | 0.05 | Empirically estimated fps vs 25/30 |

```
Q_LTC = 0.25·lock + 0.20·cont + 0.15·pulse + 0.15·trans + 0.10·sig + 0.10·sync + 0.05·fps
```

States: **VALID** (Q > 0.8), **SUSPECT** (0.5 < Q ≤ 0.8), **FAIL** (Q ≤ 0.5).

---

## 6. Sample-Accurate Delay Measurement

When both decoders are locked and master SM data is fresh:

```
d_ms = (slave_decode_sample − master_decode_sample) / sr × 1000
       − (TC_self_ms − TC_ref_ms)
```

`slave_decode_sample` and `master_decode_sample` are the absolute DAW playhead
positions at which each decoder committed its last frame.  Subtracting the
timecode difference cancels LTC frame-boundary quantisation (±1 frame = ±40 ms
at 25 fps), giving sub-millisecond accuracy regardless of update tick phase.

A **hysteresis gate** prevents single corrupted frames from derailing the delay
engine: a `d_ms` change > 500 ms (or > 50 ms when Q_LTC < 0.65) must be
confirmed by `D_MS_JUMP_CONFIRMS = 3` consecutive consistent readings.

---

## 7. Audio Fallback — NCC on Log-Energy Novelty

### 7.1 Motivation

When LTC Q < 0.5 (FAIL) or the master is stale, the LTC-derived offset is
unavailable. The audio fallback maintains an independent lag estimate from
shared acoustic content present in both tracks.

### 7.2 Log-Energy Novelty Function

Per hop (10 ms):

```
E[n]   = (1/H) Σ x_bp[k]²         (bandpass-filtered, 100–3500 Hz)
nov[n] = max(0, log(E[n]+ε) − log(E[n-1]+ε))
```

Log-flux is chosen over linear flux because it is **gain-invariant**: a
constant recording level difference A between master and slave cancels in the
log difference, making the NCC peak position independent of mic gain.

LTC carrier (constant power, near-zero novelty) does not contribute peaks.
Speech onsets produce sparse, aperiodic spikes that dominate the NCC.

### 7.3 Normalised Cross-Correlation

```
NCC(τ) = Σ (nov1[k] − μ₁)(nov2[k+τ] − μ₂) / (|valid pairs| · σ₁ · σ₂)
```

Two modes:
- **Wide** (cold start, no anchor): ±2 s search, 4 s window (400 hops).
- **Anchored** (LTC anchor < 30 s old): ±300 ms search, 1.2 s window (120 hops); ~22× cheaper.

**Sub-hop parabolic interpolation** on the NCC peak sharpens accuracy from
±10 ms (one hop) to ±2 ms:

```
subOffset = −0.5 × (NCC(τ*+1) − NCC(τ*−1)) / (NCC(τ*+1) − 2·NCC(τ*) + NCC(τ*−1))
```

**Distance-gated runner-up** (MIN_PEAK_DIST = 5 hops): the second-best peak
is only considered at lags ≥ 5 hops from the best, so prominence is not
artificially deflated by the peak's own side lobes.

### 7.4 Confidence Scoring

```
prominence = (r₁ − max(0, r₂)) / (r₁ + ε)      [how isolated the peak is]
stability  = min(1, stableCount / stabDivisor)    [consistency across cycles]

conf_AUD = 0.6 × prominence + 0.4 × stability
```

`stabDivisor` = 2 (anchored) or 3 (wide); anchored mode converges in 400 ms,
wide in 600 ms.  `valid = true` when `stableCount ≥ stabThresh AND conf > 0.3`.

### 7.5 Alpha-Beta Tracker

Raw NCC estimates are smoothed by an α-β tracker (α=0.20, β=0.02) before
reaching the delay engine.  Velocity is clamped at 0.10 ms/s to reject
acoustic TDOA variation from moving sound sources.

---

## 8. Fusion Policy

```
ltcOk = masterValid && (chnl1_in.ltc_state == VALID) && !drift_suspected

if ltcOk:
    source = LTC;  delay = d_ms

else if !inTransitionHold && audFallback.valid && conf > confThresh:
    source = AudioFallback;  delay = ab.estMs

else:
    source = None;  delay = hold (last committed activeDelayMs)
```

`confThresh` = 0.25 (anchored) / 0.40 (wide).

**Transition hold (2.5 s):** after LTC drops to FAIL, the novelty ring is
purged and NCC is suppressed for 2.5 s to allow the LTC carrier tail to clear
from the buffer.

**Hold-on-abstain:** when both sources fail, the delay engine is not reset to
zero — it holds the last committed value, shown as `src=NONE` in the UI.

---

## 9. Delay Engine

Each slave channel has:
- `const_buf` (500,000-sample historical ring): allows CH1 to serve past audio
  immediately when delay is first activated (no silence glitch).
- `delay_buf` (FIFO, length = delay_size): normal playback delay path.

**Rebuild trigger:** if `|targetMs − activeDelayMs| > 2 frames`, the FIFO is
cleared and recommitted at the new target.  The 2-frame threshold absorbs NCC
hop-resolution jitter (±10 ms) while catching genuine offset corrections.

---

## 10. MIDI Output (Slave Only)

On every `processBlock` call, three MIDI messages on channel 1:

| Message | Content |
|---------|---------|
| CC#6 (MSB) | Upper 7 bits of 14-bit value |
| CC#38 (LSB) | Lower 7 bits of 14-bit value |
| Pitch wheel | Same 14-bit value |

```
value = (|d_ms| + slider_offset) / 10000 × 16383   (full-scale = 10 s)
```

Note: `value` is derived from raw `d_ms`, not the alpha-beta smoothed estimate.

---

## 11. Implementation Notes

### Build
- CMake 3.22+ with `-DJUCE_PATH=...`; Linux requires `-lrt` for `shm_open`.
- Single binary: mode (Master / Slave) set per-instance in the UI.
- State persisted via JUCE XML (`getStateInformation` / `setStateInformation`).

### Audio Thread Safety
- No allocation, no system calls, no locks after `prepareToPlay()`.
- SM access via seqcount helpers (`seqcount_write_begin/end`, `seqcount_read_begin/end`).
- GUI reads diagnostic copies (`aud_*`, `ch1_*`) written once per ~0.1 s in the audio thread.

### Platforms
- Linux/macOS: `shm_open + mmap` under `/dev/shm/AUTOSYNC2_<group>`.
- Windows: `CreateFileMappingA + MapViewOfFile`, name `Local\AUTOSYNC2_<group>`.
- Cross-platform `SharedGroupMemory` class (~120 lines) in `Source/SharedGroupMemory.h`.

---

## 12. Test Infrastructure

| Script | Output | Purpose |
|--------|--------|---------|
| `gen_test_tracks.py` | `test_tracks/01_clean.wav` … `07_gradual_degradation.wav` | LTC tracks at varying SNR (25 fps, 44100 Hz, stereo, CH2 = CH1 + 80 ms) |
| `gen_audio_ltc_track.py` | `test_tracks/08_audio_ltc_fading.wav` | LTC dominant for 30 s then sharp cut; validates LTC→AUD handover |

Track `08` expected behaviour:
- 0–30 s: `src=LTC`, `conf_AUD` builds up in background.
- ~30.8 s: `src=AUD`, `applied ≈ 80 ms` (same as LTC phase).

---

## 13. Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Log-energy novelty (not linear) | Gain-invariant NCC: mic level differences cancel in log subtraction |
| Named shared memory (not audio routing) | DAW-independent; works across processes; tiny 8 KB struct — no audio buffer transfer |
| seqcount (not mutex) | Audio thread must not block; mutex risks priority inversion under real-time scheduling |
| Hold-on-abstain | Avoids rebuild transient and audible silence when both sources temporarily fail |
| Temporal-coherence gate | Prevents speech-derived BCD-valid collisions from corrupting `d_ms` during LTC fades |
| bothLocked gate on d_ms | A single garbage frame during re-acquisition cannot commit a spurious large offset |
| Sub-hop parabolic interpolation | Improves lag accuracy from ±10 ms to ±2 ms at no extra NCC cost |
| purgeNoveltyRing on FAIL edge | Ensures post-fade NCC window contains scene-only novelty, not LTC carrier tail |
| Anchor-coast during ring refill | Avoids 20-second blind spot after purge while ring refills |

---

## 14. Figures / Diagrams to Include in Writeup

1. **System block diagram** — Master + 2 Slave instances, SM IPC arrows, DAW tracks.
2. **Processing pipeline flowchart** — per-sample path from ADC to delay FIFO.
3. **LTC frame structure** — 80-bit frame layout (BCD fields + sync word) annotated diagram.
4. **Q_LTC pipeline** — 7 metrics → weighted sum → state thresholds.
5. **NCC illustration** — novelty curves on master and slave timelines, correlation peak at correct lag.
6. **Fusion state machine** — LTC → AudioFallback → None transitions.
7. **UI screenshot** — `ui.png` (already in repo root).
8. **Test track 08 timeline** — amplitude vs time showing LTC fade and src= transitions.
