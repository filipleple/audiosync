# Engineering Note - LTC Quality Scoring Layer

## Table of Contents

1. [Motivation](#1-motivation)
2. [Architecture Overview](#2-architecture-overview)
3. [Per-Channel Window Metrics](#3-per-channel-window-metrics)
   - 3.1 [Windowing Scheme](#31-windowing-scheme)
   - 3.2 [`lock_ratio`](#32-lock_ratio)
   - 3.3 [`continuity_score`](#33-continuity_score)
   - 3.4 [`pulse_consistency`](#34-pulse_consistency)
   - 3.5 [`transition_reliability`](#35-transition_reliability)
   - 3.6 [`signal_strength`](#36-signal_strength)
   - 3.7 [`sync_word_rate`](#37-sync_word_rate)
   - 3.8 [`fps_plausibility`](#38-fps_plausibility)
4. [`Q_LTC` Composite Score](#4-q_ltc-composite-score)
   - 4.1 [Weighting](#41-weighting)
   - 4.2 [State Thresholds](#42-state-thresholds)
5. [Processor-Level Metrics](#5-processor-level-metrics)
   - 5.1 [`dt_deviation` - Channel Agreement](#51-dt_deviation--channel-agreement)
   - 5.2 [`drift_per_s` - Drift Suspicion](#52-drift_per_s--drift-suspicion)
   - 5.3 [`fallback_requested`](#53-fallback_requested)
6. [Persistent Counters](#6-persistent-counters)
7. [New Fields in `tc_data`](#7-new-fields-in-tc_data)
8. [New Fields in `AutoSyncAudioProcessor`](#8-new-fields-in-newprojectaudioprocessor)
9. [Instrumentation Points in `handleTimecode()`](#9-instrumentation-points-in-handletimecode)
10. [Diagnostic Update Cycle in `processBlock()`](#10-diagnostic-update-cycle-in-processblock)
11. [GUI Diagnostics Panel](#11-gui-diagnostics-panel)
12. [Test Track Generator - `gen_test_tracks.py`](#12-test-track-generator--gen_test_trackspy)
    - 12.1 [Encoding Method](#121-encoding-method)
    - 12.2 [Track Specifications](#122-track-specifications)
    - 12.3 [Expected Results](#123-expected-results)
13. [Design Decisions and Limitations](#13-design-decisions-and-limitations)

---

## 1. Motivation

The previous plugin implementation decoded LTC silently. When the signal degraded - due to low SNR, dropouts, drift, or mis-configured FPS - the plugin continued reporting the last decoded timecode without any indication that the measurement was unreliable. There was no signal to external systems to switch to a fallback synchronisation source.

This note describes a diagnostic layer added on top of the existing decoder that:

- Computes seven sub-metrics per channel in rolling 0.5 s windows.
- Combines them into a single `Q_LTC` quality score in [0, 1].
- Maps the score to a three-level state: `VALID`, `SUSPECT`, `FAIL`.
- Raises a `fallback_requested` flag when either channel is `FAIL` or a cross-channel drift trend is detected.
- Exposes all metrics to the GUI and (via existing public fields) to DAW automation.

No changes were made to the core LTC decoder logic. All new code is additive instrumentation.

---

## 2. Architecture Overview

```
Audio input (per sample)
        │
        ▼
handleTimecode()          ← unchanged decoder logic
        │  instrumented at 4 points (see §9)
        │
        ├─► per-sample accumulators  (w_env_sum, w_trans_total, …)
        ├─► per-transition counters  (w_trans_valid, w_pulse_dev_sum, …)
        └─► per-sync-word counters   (w_sync_hits, w_frames_decoded, w_continuity_ok)
                │
                ▼  every W_SIZE = 22050 samples (~0.5 s)
        computeAndResetWindow()
                │
                ├─► q_lock_ratio, q_continuity_score, q_pulse_consistency,
                │   q_transition_reliability, q_signal_strength,
                │   q_sync_word_rate, q_fps_plausibility
                ├─► Q_LTC  (weighted sum)
                ├─► ltc_state  (VALID / SUSPECT / FAIL)
                └─► fallback_requested  (per-channel)

processBlock()  every 4410 samples (~0.1 s)
        │
        ├─► copy per-channel metrics to processor public fields
        ├─► push d_ms into dt_history (rolling 20-sample window)
        ├─► compute dt_deviation  (std dev of Δt)
        ├─► compute drift_per_s   (linear regression slope of Δt)
        └─► combine into processor-level fallback_requested
```

---

## 3. Per-Channel Window Metrics

### 3.1 Windowing Scheme

Each `tc_data` instance accumulates statistics independently. The window size is:

```cpp
const int W_SIZE = 22050;  // samples
```

At 44100 Hz this is exactly 0.5 s. At 48000 Hz it is approximately 0.46 s. The window is not time-aligned to frame boundaries; it resets whenever `w_sample_count` reaches `W_SIZE` inside `handleTimecode()`. Accumulators are zeroed after each computation. The `w_prev_frms` and `w_samples_since_trans` fields persist across window boundaries to maintain continuity.

**Expected frames per window:**

```
expected_frames = W_SIZE × fps / sample_rate
                = 22050 × 25 / 44100 = 12.5   (25 fps)
                = 22050 × 30 / 44100 = 15.0   (30 fps)
```

---

### 3.2 `lock_ratio`

**What it measures:** the fraction of expected LTC frames that were successfully decoded (sync word found and BCD fields extracted) in the window.

**Accumulation:** `w_frames_decoded` is incremented once per detected sync word.

**Computation:**

```
q_lock_ratio = min(1, w_frames_decoded / expected_frames)
```

**Interpretation:** 1.0 means every expected frame arrived. 0.0 means no frames decoded at all. Degrades quickly with heavy noise or dropouts (missed sync words) and slowly with occasional single-bit errors (which can corrupt the sync word match).

**Weight in Q_LTC:** 0.25 (highest - the most direct measure of decodability).

---

### 3.3 `continuity_score`

**What it measures:** the fraction of consecutive decoded frame pairs where the frame counter advanced by exactly one.

**Accumulation:** at each sync word detection, the current `frms` field is compared to `w_prev_frms`:

```cpp
bool step = (data.frms == data.w_prev_frms + 1);
bool wrap = (data.frms == 0 && data.w_prev_frms >= 23);
if (step || wrap) ++data.w_continuity_ok;
data.w_prev_frms = data.frms;
```

The `wrap` condition handles second-boundary rollovers for any frame rate ≥ 24 fps. It does not handle cases where the generator uses drop-frame timecode (not relevant for this plugin's use cases).

**Computation:**

```
q_continuity_score = min(1, w_continuity_ok / (w_frames_decoded − 1))
                   = 0  if w_frames_decoded ≤ 1
```

**Interpretation:** 1.0 means continuous lock. Values below 1.0 indicate jumped frames (signal burst, phase slip, or source discontinuity). Unlike `lock_ratio`, this catches the case where the decoder is re-locking on each window but the timecode values are inconsistent.

**Weight in Q_LTC:** 0.20.

---

### 3.4 `pulse_consistency`

**What it measures:** how close the actual BMC bit-cell duration is to the theoretical value at the configured FPS.

**Background:** a BMC bit cell is exactly `2 × pulsesize` samples wide. `pulsesize = sample_rate / fps / 160`. The decoder commits a bit only when `sillen > 1.8 × pulsesize`, so the measured `sillen` at commit time is always in the range `[1.8p, 2.2p]` under normal conditions (the upper bound comes from the silence detector timeout).

**Accumulation:** at each bit commit, before `sillen` is reset:

```cpp
float dev = |sillen − 2 × pulsesize| / (2 × pulsesize);
w_pulse_dev_sum += dev;
w_sillen_sum    += sillen;
++w_pulse_dev_count;
```

`dev` is a normalised fractional deviation. A clean 25 fps signal at 44100 Hz has `pulsesize = 11.025` samples. Rounding distributes 4 extra samples across 160 half-pulses per frame, producing `dev ≈ 0.01–0.025` per commit. Noise and jitter push `dev` upward.

**Computation:**

```
avg_dev = w_pulse_dev_sum / w_pulse_dev_count
q_pulse_consistency = max(0, 1 − avg_dev × 3)
```

The factor of 3 maps `dev = 0.33` (33% timing error) to a score of 0. In practice, the decoder loses lock before dev reaches 0.33.

**Weight in Q_LTC:** 0.15.

---

### 3.5 `transition_reliability`

**What it measures:** the fraction of signal transitions that occur within a plausible timing window relative to the previous transition.

**Background:** in a valid BMC signal, transitions are separated by either ~`pulsesize` (intra-bit, for '1' bits) or ~`2 × pulsesize` (inter-bit boundary). No other interval should appear. High-frequency noise produces rapid spurious transitions; low-frequency interference produces over-long gaps.

**Accumulation:** `w_samples_since_trans` is incremented every sample and reset to 0 on every detected transition (including transitions that do not yet trigger a bit commit). At each transition:

```cpp
float interval = w_samples_since_trans;
if (interval >= pulsesize × 0.4 && interval <= pulsesize × 2.5)
    ++w_trans_valid;
++w_trans_total;
w_samples_since_trans = 0;
```

The window `[0.4p, 2.5p]` accepts both short (mid-bit) and long (inter-bit) intervals with 40% tolerance on each side.

**Computation:**

```
q_transition_reliability = w_trans_valid / w_trans_total
                         = 0 if w_trans_total == 0
```

**Interpretation:** approaches 1.0 for a clean signal. Heavy noise drives this toward 0.5 or lower because it creates many transitions at random intervals outside the valid range.

**Weight in Q_LTC:** 0.15.

---

### 3.6 `signal_strength`

**What it measures:** the time-averaged envelope level relative to the noise floor constant.

**Background:** `threshold` is a per-sample exponential moving average of `|s|` with time constant ≈ 100 ms. In silence, it decays to `minthresh = 0.0001`. With a healthy LTC signal at normalized amplitude `A`, it converges to `≈ A` in steady state. Thus `threshold / minthresh` is a proxy for the signal-to-floor ratio.

**Accumulation:**

```cpp
w_env_sum += threshold;
++w_env_count;
```

**Computation:**

```
avg_env = w_env_sum / w_env_count
q_signal_strength = min(1, avg_env / (minthresh × 100))
                  = min(1, avg_env / 0.01)
```

For `avg_env = 0.01` (amplitude ≈ 0.01 = −40 dBFS): `q_signal_strength = 1.0`.
For `avg_env = 0.001` (amplitude ≈ 0.001 = −60 dBFS): `q_signal_strength = 0.1`.
For `avg_env = 0.0001` (silence / noise floor): `q_signal_strength ≈ 0.01`.

**Weight in Q_LTC:** 0.10.

---

### 3.7 `sync_word_rate`

**What it measures:** how frequently the sync word pattern (`0011111111111101`) is found in the bit stream, compared to what is expected.

**Accumulation:** `w_sync_hits` is incremented each time the 16-bit sync word check passes. This is identical to `w_frames_decoded` (every passing sync word corresponds to one decoded frame).

**Computation:**

```
q_sync_word_rate = min(1, w_sync_hits / expected_frames)
```

**Relationship to `lock_ratio`:** these two metrics are computed from the same counter and will therefore track each other closely. They are kept as separate fields for logging clarity and to allow future decoupling (e.g., if a partial-decode mode is added that counts `lock_ratio` differently).

**Weight in Q_LTC:** 0.10.

---

### 3.8 `fps_plausibility`

**What it measures:** whether the actual timing of transitions is consistent with a 25 or 30 fps LTC signal, independent of the FPS setting in the GUI.

**Computation:** the empirical half-pulse duration is derived from the `w_sillen_sum` accumulator:

```
empirical_half_pulse = (w_sillen_sum / w_pulse_dev_count) / 2
estimated_fps        = sample_rate / (empirical_half_pulse × 160)
diff                 = min(|estimated_fps − 25|, |estimated_fps − 30|)
q_fps_plausibility   = max(0, 1 − diff / 5)
```

The tolerance of 5 fps means `q_fps_plausibility > 0` for any measured fps within 5 fps of either supported rate. This catches the scenario (F4) where the GUI fps selector is set to the wrong rate: the decoder will fail to find sync words, `w_pulse_dev_count` will be low, and `estimated_fps` will be unreliable - which is reflected in the score.

**Note:** if `w_pulse_dev_count == 0` (no bits committed in the window), `q_fps_plausibility = 0` and `estimated_fps` is not updated (retains its last computed value).

**Weight in Q_LTC:** 0.05 (lowest - acts as a secondary indicator, not a primary quality signal).

---

## 4. `Q_LTC` Composite Score

### 4.1 Weighting

```
Q_LTC = 0.25 × q_lock_ratio
      + 0.20 × q_continuity_score
      + 0.15 × q_pulse_consistency
      + 0.15 × q_transition_reliability
      + 0.10 × q_signal_strength
      + 0.10 × q_sync_word_rate
      + 0.05 × q_fps_plausibility
```

Weights sum to 1.0. The four metrics with weight ≥ 0.15 (`lock_ratio`, `continuity_score`, `pulse_consistency`, `transition_reliability`) directly reflect the structural integrity of the decoded bit stream. The lower-weight metrics (`signal_strength`, `sync_word_rate`, `fps_plausibility`) provide corroborating evidence.

### 4.2 State Thresholds

| `Q_LTC` range | `ltc_state` | Meaning |
|---|---|---|
| > 0.8 | `VALID` (2) | Reliable decode; Δt measurement can be trusted |
| > 0.5 and ≤ 0.8 | `SUSPECT` (1) | Decode active but degraded; treat Δt with caution |
| ≤ 0.5 | `FAIL` (0) | Decoder not locking reliably; Δt unreliable |

`ltc_state` is stored as `enum class LTCState : uint8_t { FAIL=0, SUSPECT=1, VALID=2 }` in `tc_data`, and exposed to the processor and GUI as `int` (0/1/2).

`fallback_requested` (per-channel) is set `true` whenever `ltc_state == FAIL` and cleared when the window improves to `SUSPECT` or `VALID`.

---

## 5. Processor-Level Metrics

These metrics operate on the cross-channel Δt history rather than on individual decoder state.

### 5.1 `dt_deviation` - Channel Agreement

**Purpose:** detect the case (F3) where both channels are individually decoding valid LTC but the measured offset Δt is unstable, indicating inconsistent or drifting source timecodes.

**Implementation:** every 4410 samples (~0.1 s), the current `d_ms` is pushed onto `dt_history` (a `std::deque<double>` capped at 20 entries = 2 s history). Zero values are excluded (no valid measurement).

```
dt_deviation = sqrt( Σ(d_i − mean)² / N )   in ms
```

**Interpretation:** for a perfect static offset, `dt_deviation = 0`. Values above ~1 ms indicate instability; values above ~5 ms suggest inconsistent sources or heavy noise on one channel.

---

### 5.2 `drift_per_s` - Drift Suspicion

**Purpose:** detect slow linear drift (F6) - the case where Δt has a consistent monotonic trend rather than random fluctuation around a mean.

**Implementation:** ordinary least squares on the `dt_history` buffer, fitting `d_i = slope × i + intercept`. The slope in ms per 0.1 s interval is converted to ms/s:

```cpp
drift_per_s = slope × 10.0
```

`drift_suspected` is set `true` when `|drift_per_s| > 5.0` ms/s for **3 consecutive 0.1 s windows** (i.e., the condition must hold for at least 0.3 s). A single window above threshold is ignored; a sustained trend is required. The confirm counter (`drift_confirm_count`) resets to 0 whenever a window falls back below the threshold.

At 5 ms/s, a 2-second observation window would show a 10 ms total drift, which is a quarter-frame at 25 fps. The threshold was raised from 2 ms/s and the confirmation requirement added after observing false positives at SNR 20 dB, where occasional decode errors cause momentary OLS slope spikes without any real drift.

The regression is computed only when `dt_history.size() >= 3`.

---

### 5.3 `fallback_requested`

The processor-level `fallback_requested` flag is the primary output of the quality scoring system:

```cpp
fallback_requested = (chnl1_in.fallback_requested ||
                      chnl2_in.fallback_requested ||
                      drift_suspected);
```

It is set if **either** input channel reaches `FAIL` state, **or** the cross-channel drift rate exceeds 5 ms/s for 3 consecutive windows. An external system polling this flag should treat the current Δt measurement as invalid and switch to an alternative synchronisation source.

---

## 6. Persistent Counters

Two counters in `tc_data` are **not** reset by `computeAndResetWindow()`. They survive across window boundaries and are reset by `clear()` only.

| Field | Incremented when | Reset when |
|---|---|---|
| `decoder_reset_count` | `clear()` is called | Never (accumulates for session lifetime) |
| `rejected_frames_count` | BCD-range check in `handleTimecode()` rejects an impossible SMPTE value (hrs>23, mnts>59, scnds>59, frms≥fps) **or** the temporal-coherence gate rejects a frame outside the ±1..+3 step window | Never |

**Note on `rejected_frames_count`:** In the current implementation, rejections are performed
inside `handleTimecode()` itself (BCD validation and coherence gate, see §9.1 below), not in
the legacy `calc_delay()` function. `calc_delay()` still contains a sanity cap for the
1-hour magnitude limit but is no longer called in the main master-slave processing path; the
sample-accurate SM-based d_ms computation in `processBlock()` supersedes it.

---

## 7. New Fields in `tc_data`

All fields are in-class initialized (C++11 style). `const` fields are per-instance constants.

### Window Accumulators (reset by `computeAndResetWindow`)

| Field | Type | Description |
|---|---|---|
| `w_sample_count` | `int` | Samples elapsed in current window |
| `W_SIZE` | `const int` = 22050 | Window size in samples |
| `w_frames_decoded` | `int` | Sync words found in window |
| `w_continuity_ok` | `int` | Consecutive-frame pairs with correct +1 increment |
| `w_prev_frms` | `int` | `frms` value from previous decoded frame (persists across windows) |
| `w_pulse_dev_sum` | `float` | Sum of normalised pulse deviation per bit commit |
| `w_sillen_sum` | `float` | Sum of `sillen` values at bit commits (for empirical fps) |
| `w_pulse_dev_count` | `int` | Number of bit commits in window |
| `w_trans_valid` | `int` | Transitions with interval in `[0.4p, 2.5p]` |
| `w_trans_total` | `int` | All transitions in window |
| `w_samples_since_trans` | `int` | Samples since last transition (persists across windows) |
| `w_env_sum` | `float` | Sum of `threshold` values per sample |
| `w_env_count` | `int` | Samples in envelope accumulation |
| `w_sync_hits` | `int` | Sync word matches in window |

### Computed Metrics (written by `computeAndResetWindow`)

| Field | Type | Range | Description |
|---|---|---|---|
| `q_lock_ratio` | `float` | [0, 1] | Decoded frames / expected frames |
| `q_continuity_score` | `float` | [0, 1] | Consecutive-frame step continuity |
| `q_pulse_consistency` | `float` | [0, 1] | Pulse timing regularity |
| `q_transition_reliability` | `float` | [0, 1] | In-range inter-transition intervals |
| `q_signal_strength` | `float` | [0, 1] | Envelope level relative to floor |
| `q_sync_word_rate` | `float` | [0, 1] | Sync words / expected frames |
| `q_fps_plausibility` | `float` | [0, 1] | Empirical fps vs 25/30 |
| `Q_LTC` | `float` | [0, 1] | Composite quality score |
| `estimated_fps` | `float` | ~fps | FPS derived from empirical pulse timing |
| `ltc_state` | `LTCState` | 0/1/2 | FAIL / SUSPECT / VALID |

### Persistent Fields

| Field | Type | Description |
|---|---|---|
| `decoder_reset_count` | `int` | Accumulated `clear()` calls |
| `rejected_frames_count` | `int` | Frames rejected by `calc_delay()` outlier filter |
| `fallback_requested` | `bool` | True while `ltc_state == FAIL` |

---

## 8. New Fields in `AutoSyncAudioProcessor`

### Diagnostic Outputs (public, written by audio thread, read by GUI timer)

| Field | Type | Source |
|---|---|---|
| `ch1_Q_LTC` | `float` | `chnl1_in.Q_LTC` |
| `ch2_Q_LTC` | `float` | `chnl2_in.Q_LTC` |
| `ch1_ltc_state` | `int` | `(int)chnl1_in.ltc_state` |
| `ch2_ltc_state` | `int` | `(int)chnl2_in.ltc_state` |
| `ch1_estimated_fps` | `float` | `chnl1_in.estimated_fps` |
| `ch2_estimated_fps` | `float` | `chnl2_in.estimated_fps` |
| `ch1_decoder_resets` | `int` | `chnl1_in.decoder_reset_count` |
| `ch2_decoder_resets` | `int` | `chnl2_in.decoder_reset_count` |
| `ch1_rejected_frames` | `int` | `chnl1_in.rejected_frames_count` |
| `ch2_rejected_frames` | `int` | `chnl2_in.rejected_frames_count` |
| `dt_deviation` | `double` | std dev of last 20 Δt samples (ms) |
| `drift_per_s` | `double` | OLS slope of Δt history (ms/s) |
| `drift_suspected` | `bool` | `|drift_per_s| > 5.0` for 3 consecutive windows |
| `fallback_requested` | `bool` | Either channel FAIL or drift suspected |

### Private Implementation Fields

| Field | Type | Description |
|---|---|---|
| `dt_history` | `std::deque<double>` | Rolling Δt history, max 20 entries |
| `dt_sample_counter` | `int` | Counts samples toward 4410-sample update interval |

---

## 9. Temporal-Coherence Gate

The quality scoring layer is purely observational (it accumulates metrics but does not filter
decoded frames). However, a separate **temporal-coherence gate** in `handleTimecode()` actively
rejects frames that are syntactically valid (sync word + BCD in range) but temporally incoherent
— i.e. frames that decode to an impossible timecode progression relative to the previous frame.
This guards against speech transients that happen to satisfy both the sync-word match and the
BCD range checks but encode nonsense timecodes.

### BCD Range Validation

Before accepting any decoded frame, `handleTimecode()` checks:

```
hrs ≤ 23  AND  mnts ≤ 59  AND  scnds ≤ 59  AND  frms < fps
```

Frames outside this range increment `rejected_frames_count` and reset `syncpos` to -1. The
decoded digit fields (`hrs`, `mnts`, `scnds`, `frms`) are rolled back to the last accepted
values so garbage digits cannot reach the `d_ms` computation in `processBlock()`.

### Lock / Unlock State Machine

```
LOCK_N  = 10   // consecutive valid-step frames needed to enter locked state
UNLOCK_M = 20  // consecutive rejected frames needed to drop lock
```

| State | Transition in | Transition out |
|-------|--------------|----------------|
| Unlocked | Initial / after UNLOCK_M rejects | After LOCK_N consecutive +1..+3 forward steps |
| Locked | (see above) | After UNLOCK_M consecutive frames outside [-1..+3] step window |

While **locked**, the decoder requires each new BCD-valid frame's timecode to be within
`[-1.5, +3.5]` frame-durations of `last_accepted_tc_ms`. Frames outside this window are
rejected (they are likely speech transients). The tolerance of −1.5 frames accommodates
one-frame-backward jitter; +3.5 frames allows up to 3 dropped frames without losing lock.

While **unlocked** (re-acquiring after a long dropout), the decoder is permissive — it accepts
any BCD-valid frame and counts consecutive +1..+3 forward steps toward re-lock. This allows
fast re-acquisition after genuine LTC gaps without the strict rejection of the locked state.

The `locked` field in `tc_data` (boolean) is also written to `MasterSlot.locked` in shared
memory so that the slave can gate its `d_ms` update: both decoders must be locked before a
new offset is committed (see `bothLocked` in master-slave-architecture.md §7).

---

## 10. Instrumentation Points in `handleTimecode()`

Four instrumentation blocks were inserted into the existing function. No existing logic was modified.

**Point 1 - per-sample envelope and inter-transition counter:**

```cpp
++data.sillen;
++data.w_samples_since_trans;              // ← NEW
// ... silence detector (resets w_samples_since_trans on timeout) ...
data.threshold = ...;                      // existing threshold update
data.w_env_sum += (float)data.threshold;   // ← NEW
++data.w_env_count;                        // ← NEW
```

**Point 2 - at every detected transition:**

```cpp
data.lastsign *= -1;
// NEW block:
++data.w_trans_total;
float interval = (float)data.w_samples_since_trans;
if (interval >= data.pulsesize * 0.4f && interval <= data.pulsesize * 2.5f)
    ++data.w_trans_valid;
data.w_samples_since_trans = 0;
// end NEW block
++data.gotbit;
```

**Point 3 - at every bit commit (inside `sillen > 1.8 × pulsesize` block), before `sillen = 0`:**

```cpp
// NEW block:
float dev = std::abs((float)data.sillen - 2.0f * data.pulsesize)
          / (2.0f * data.pulsesize);
data.w_pulse_dev_sum += dev;
data.w_sillen_sum    += (float)data.sillen;
++data.w_pulse_dev_count;
// end NEW block
data.gotbit = std::min(data.gotbit, 1);
data.sillen = 0;
```

**Point 4 - at every sync word detection, after BCD extraction:**

```cpp
// NEW block:
++data.w_sync_hits;
++data.w_frames_decoded;
if (data.w_prev_frms >= 0) {
    bool step = (data.frms == data.w_prev_frms + 1);
    bool wrap = (data.frms == 0 && data.w_prev_frms >= 23);
    if (step || wrap) ++data.w_continuity_ok;
}
data.w_prev_frms = data.frms;
// end NEW block
```

**Point 5 - window boundary (end of function):**

```cpp
++data.w_sample_count;
if (data.w_sample_count >= data.W_SIZE)
    data.computeAndResetWindow((double)srate, frates[slider]);
```

---

## 11. Diagnostic Update Cycle in `processBlock()`

The diagnostic copy and Δt analysis run every 4410 samples (≈ 0.1 s at 44100 Hz), controlled by `dt_sample_counter`. This rate is low enough not to impact audio-thread performance and high enough to give the GUI a responsive update at its 1 ms timer interval.

Inside each diagnostic update:

1. Ten public fields are copied from `chnl1_in` and `chnl2_in` (two float reads each, no allocation).
2. If `d_ms != 0`, push to `dt_history`; pop front if size > 20.
3. If `dt_history.size() >= 2`, compute `dt_deviation` via single-pass variance.
4. If `dt_history.size() >= 3`, compute OLS slope for `drift_per_s`.
5. Update processor-level `fallback_requested`.

---

## 12. GUI Diagnostics Panel

The plugin window was expanded from 400 × 300 px to **400 × 380 px**. A horizontal separator line is drawn in `paint()` at y = 302. Four new `juce::Label` components are added below the separator.

| Label | Position (x, y, w, h) | Content |
|---|---|---|
| `qual_title` | 0, 306, 400, 16 | Static: "--- QUALITY DIAGNOSTICS ---" |
| `qual_ch1_label` | 4, 325, 396, 16 | CH1 state, Q, fps, reset/reject counts |
| `qual_ch2_label` | 4, 343, 396, 16 | CH2 state, Q, fps, reset/reject counts |
| `qual_sync_label` | 4, 361, 396, 16 | Δt dev, drift rate, fallback flag |

**Example display (healthy signal):**

```
--- QUALITY DIAGNOSTICS ---
CH1: VALID  Q=0.92  fps=25.0  Rst=0  Rej=0
CH2: VALID  Q=0.91  fps=25.1  Rst=0  Rej=0
dt_dev=0.3ms  drift=0.01ms/s  Fallback=no
```

**Example display (degraded channel 2):**

```
--- QUALITY DIAGNOSTICS ---
CH1: VALID    Q=0.88  fps=25.0  Rst=0  Rej=0
CH2: SUSPECT  Q=0.62  fps=24.8  Rst=3  Rej=12
dt_dev=8.4ms  drift=0.23ms/s  Fallback=no
```

**Example display (noise floor):**

```
--- QUALITY DIAGNOSTICS ---
CH1: FAIL  Q=0.11  fps=0.0   Rst=14  Rej=0
CH2: FAIL  Q=0.08  fps=0.0   Rst=11  Rej=0
dt_dev=0.0ms  drift=0.00ms/s  Fallback=YES
```

Labels are updated in `timerCallback()` at 1 ms intervals (existing timer). `std::clamp(ch_ltc_state, 0, 2)` guards the `stateNames[]` array lookup.

---

## 13. Test Track Generator - `gen_test_tracks.py`

### 12.1 Encoding Method

The generator (`gen_test_tracks.py`) produces synthetic SMPTE LTC audio without any external dependencies (standard library only: `wave`, `struct`, `math`, `random`).

**BMC signal generation:**

Each bit cell consists of two half-pulses of duration `half_pulse = sample_rate / fps / 160`. Since this value is non-integer (44100 / 25 / 160 = 11.025), samples are generated using a fractional accumulator:

```python
t += half_pulse
samples.extend([polarity] * (int(t) - int(t_prev)))
```

This distributes rounding across the stream rather than per-frame, producing a consistent timing pattern. For 44100 Hz / 25 fps: 156 half-pulses of 11 samples and 4 half-pulses of 12 samples per frame, totalling 1764 samples = 44100 / 25 exactly.

The 4 long half-pulses fall at positions 39, 79, 119, 159 (0-based) within the 160 half-pulses of each frame. This is where the fractional part `i × 0.025` first exceeds 0.975 within each 40-step cycle.

**Timecode structure:**

- CH1 starts at `01:00:00:00` (25 fps).
- CH2 starts at `01:00:00:02` (= `+2 frames` = **80 ms** ahead of CH1).
- The plugin should therefore report `d_ms = −80 ms` (CH2 leads CH1) or `delay_frames = −2`, depending on polarity convention.

**Noise model:** white Gaussian noise added to the scaled LTC signal. SNR is computed relative to the signal RMS after amplitude scaling.

**Dropout model:** Poisson-distributed burst events with exponential duration. `rate_per_sec` controls event frequency; `avg_dur_ms` controls mean burst length. Gaps are filled with 0.0 samples.

---

### 12.2 Track Specifications

All tracks: stereo, 44100 Hz, 16-bit PCM WAV. CH1 = left, CH2 = right. Duration = 15 s (375 frames). Output directory: `test_tracks/`.

| File | Signal amplitude | SNR (dB) | Dropout rate (/s) | Mean dropout (ms) | Approx. size |
|---|---|---|---|---|---|
| `01_clean.wav` | 0.70 | - | 0 | - | 2.6 MB |
| `02_snr20dB.wav` | 0.70 | 20 | 0 | - | 2.6 MB |
| `03_snr10dB.wav` | 0.70 | 10 | 0 | - | 2.6 MB |
| `04_snr6dB_dropouts.wav` | 0.50 | 6 | 0.5 | 30 | 2.6 MB |
| `05_snr3dB_heavy.wav` | 0.30 | 3 | 2.0 | 50 | 2.6 MB |
| `06_noise_floor.wav` | 0.08 | 0 | 5.0 | 80 | 2.6 MB |
| `07_gradual_degradation.wav` | ramp | ramp | ramp | ramp | 10.1 MB |

**`07_gradual_degradation.wav` - segment map (60 s total):**

| Time | Amplitude | SNR | Dropout /s | Mean ms | Segment label |
|---|---|---|---|---|---|
| 0–10 s | 0.70 | - | 0 | - | Clean |
| 10–20 s | 0.70 | 20 dB | 0 | - | Light noise |
| 20–30 s | 0.60 | 10 dB | 0 | - | Moderate noise |
| 30–40 s | 0.50 | 6 dB | 0.5 | 30 | SNR 6 dB + dropouts |
| 40–50 s | 0.30 | 3 dB | 2.0 | 50 | Heavy noise + dropouts |
| 50–60 s | 0.08 | 0 dB | 5.0 | 80 | Noise floor |

---

### 12.3 Expected Results

The following table gives expected `Q_LTC` ranges, state, and GUI indicators. Values are approximate - exact scores depend on random seed (fixed at 42), host sample rate, and whether the FPS selector matches the encoded rate (25 fps).

| File | Expected `Q_LTC` | Expected `ltc_state` | `fallback_requested` | Notes |
|---|---|---|---|---|
| `01_clean.wav` | 0.88 – 0.95 | VALID | no | Fast lock (~0.5 s). `estimated_fps` ≈ 25.0. `dt_deviation` < 1 ms. |
| `02_snr20dB.wav` | 0.82 – 0.90 | VALID | no | Occasional transition jitter; `q_pulse_consistency` may drop to ~0.8. |
| `03_snr10dB.wav` | 0.65 – 0.82 | VALID / SUSPECT | no | Score may oscillate across the VALID/SUSPECT threshold between windows. `q_transition_reliability` notably degraded. |
| `04_snr6dB_dropouts.wav` | 0.48 – 0.68 | SUSPECT | intermittent | Dropouts cause missed sync words → `q_lock_ratio` dips. Some windows hit FAIL. `Rej` counter may increment. |
| `05_snr3dB_heavy.wav` | 0.15 – 0.45 | FAIL (most windows) | yes (most of the time) | Decoder loses and re-locks frequently. `decoder_reset_count` rises. `dt_deviation` elevated. |
| `06_noise_floor.wav` | 0.00 – 0.15 | FAIL | yes (persistent) | No consistent lock. `Q_LTC` near 0. `estimated_fps` = 0 (no bit commits). `fallback=YES`. |

**`07_gradual_degradation.wav` - state progression:**

| Time | Expected `ltc_state` | `fallback_requested` |
|---|---|---|
| 0–10 s | VALID | no |
| 10–20 s | VALID | no |
| 20–30 s | VALID or SUSPECT | no |
| 30–40 s | SUSPECT | intermittent |
| 40–50 s | FAIL | yes |
| 50–60 s | FAIL | yes (persistent) |

**The expected transition to persistent `fallback=YES` should occur during the 40–50 s segment.** The exact sample at which `ltc_state` first crosses into FAIL depends on which 0.5 s window first accumulates enough degraded measurements; in practice expect it within the first 1–3 s of that segment.

**Known deviations from ideal:**

- At SNR 10 dB (`03`), the decoder may hold VALID for the entire 15 s if the noise happens to leave the sync word intact. Re-running with a different seed (`random.seed(N)`) will produce variation.
- `q_signal_strength` saturates at 1.0 for any amplitude ≥ 0.01 (−40 dBFS) because the normalization constant `minthresh × 100 = 0.01` is a low bar. It becomes the primary discriminator only for track `06` where amplitude is 0.08 (attenuated further by noise clipping).
- `estimated_fps` is only meaningful when `w_pulse_dev_count > 0`. On tracks `05` and `06`, many windows will have `estimated_fps = 0` displayed in the GUI.
- Channel agreement (`dt_deviation`) will be low even on degraded tracks because both channels are degraded symmetrically. A more realistic stress test for channel agreement would use track `01` on CH1 and track `04` or `05` on CH2.

---

## 14. Design Decisions and Limitations

**No channel_agreement sub-metric in Q_LTC:** channel agreement is computed at the processor level from `d_ms` history, not folded into per-channel `Q_LTC`. This is intentional: a single channel can score VALID while cross-channel Δt is noisy, and the two failure modes should be reported separately.

**`channel_agreement` is not a named metric in the GUI:** `dt_deviation` serves this function. High `dt_deviation` in conjunction with two VALID channels indicates F3 (inconsistent but individually valid sources).

**Window size is sample-count-based, not frame-count-based:** `W_SIZE = 22050` is fixed in samples. At 48000 Hz the window would cover fewer frames (~11.4 at 25 fps vs 12.5 at 44100 Hz), slightly reducing `q_lock_ratio` sensitivity. A future improvement could make `W_SIZE` proportional to `sample_rate`.

**Thread safety:** all public diagnostic fields in `AutoSyncAudioProcessor` are written on the audio thread and read on the GUI timer thread without locking. This follows the existing pattern used by `d_ms`, `input_ch1`, etc., throughout the plugin. For a small number of float/int/bool reads, the practical risk of a torn read producing a visible artefact on the GUI is accepted. The fields are not used for any control path decision.

**`fallback_requested` is a flag, not an event:** it does not fire a one-shot notification. An external system must poll it or compare successive values. This matches the GUI's polling architecture (1 ms timer).

**`rejected_frames_count` accumulates for the session lifetime** and is never zeroed by `computeAndResetWindow`. On very long sessions with a noisy source, this counter may reach high values. It is informational only and does not affect Q_LTC.
