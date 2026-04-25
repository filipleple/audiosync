# Technical Documentation - "AudioSync" JUCE VST Plugin

**Version:** 1.4 (UI label), plugin version string 1.0.0
**Format:** VST3, AU, Standalone
**Purpose:** Decode SMPTE LTC timecodes from two stereo audio channels, compute their temporal difference, apply compensating audio delay to the leading channel, and transmit the measured offset via 14-bit MIDI CC.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Source File Structure](#2-source-file-structure)
3. [The `tc_data` Class](#3-the-tc_data-class)
4. [SMPTE LTC Timecode - Format Reference](#4-smpte-ltc-timecode--format-reference)
5. [LTC Decoder - `handleTimecode()`](#5-ltc-decoder--handletimecode)
   - 5.1 [Pulse Size Calculation](#51-pulse-size-calculation)
   - 5.2 [DC Offset Removal (High-Pass Filter)](#52-dc-offset-removal-high-pass-filter)
   - 5.3 [Silence / Loss-of-Signal Detection](#53-silence--loss-of-signal-detection)
   - 5.4 [Adaptive Amplitude Threshold](#54-adaptive-amplitude-threshold)
   - 5.5 [Zero-Crossing / Transition Detection](#55-zero-crossing--transition-detection)
   - 5.6 [Bit Decoding (Biphase Mark Code)](#56-bit-decoding-biphase-mark-code)
   - 5.7 [Circular Bit Buffer](#57-circular-bit-buffer)
   - 5.8 [Sync Word Detection](#58-sync-word-detection)
   - 5.9 [Time Field Extraction (BCD Decoding)](#59-time-field-extraction-bcd-decoding)
   - 5.10 [Change Detection via `new_time`](#510-change-detection-via-new_time)
6. [`processTimeCode()` - Per-Sample Wrapper](#6-processtimecode--per-sample-wrapper)
7. [`calc_delay()` - Timecode Difference Calculation](#7-calc_delay--timecode-difference-calculation)
   - 7.1 [Frame-Based Arithmetic](#71-frame-based-arithmetic)
   - 7.2 [Stability / Outlier Filter](#72-stability--outlier-filter)
8. [Audio Delay Engine](#8-audio-delay-engine)
   - 8.1 [`handle_const_delay()` - Historical Lookahead Buffer](#81-handle_const_delay--historical-lookahead-buffer)
   - 8.2 [`delay()` - Sample-Level Delay Line](#82-delay--sample-level-delay-line)
9. [`processBlock()` - Main Audio Processing Loop](#9-processblock--main-audio-processing-loop)
   - 9.1 [Channel Routing](#91-channel-routing)
   - 9.2 [MIDI Output](#92-midi-output)
   - 9.3 [Per-Sample Processing Loop](#93-per-sample-processing-loop)
   - 9.4 [Delay Stability Smoothing](#94-delay-stability-smoothing)
   - 9.5 [Active Delay Routing](#95-active-delay-routing)
   - 9.6 [Inactive (Bypass) Mode](#96-inactive-bypass-mode)
10. [GUI - `PluginEditor`](#10-gui--plugineditor)
    - 10.1 [Layout and Controls](#101-layout-and-controls)
    - 10.2 [Timer-Based State Polling](#102-timer-based-state-polling)
    - 10.3 [FPS Selector](#103-fps-selector)
    - 10.4 [Delay Slider](#104-delay-slider)
    - 10.5 [Delay Activate Button](#105-delay-activate-button)
11. [MIDI Parameter (`myParameter`)](#11-midi-parameter-myparameter)
12. [Plugin Configuration (`JucePluginDefines.h`)](#12-plugin-configuration-juceplugindesinesh)
13. [Known Limitations and Design Notes](#13-known-limitations-and-design-notes)

---

## 1. Project Overview

This plugin is designed to solve the audio–video synchronization problem that arises when recording with a 360-degree camera rig. The camera and a reference audio source each carry an LTC (Longitudinal Timecode) track. By feeding both LTC signals into the two channels of a stereo plugin instance, the plugin:

1. Independently decodes the SMPTE timecode on each channel.
2. Computes the signed frame-and-millisecond offset between them.
3. Optionally delays the temporally leading channel in real time to bring both signals into alignment.
4. Exposes the measured offset as a 14-bit MIDI CC value and a DAW automation parameter so that external devices or recorders can act on the measurement.

The plugin is intended to be loaded in a DAW (the README cites REAPER) on a stereo channel that carries the two LTC audio streams.

---

## 2. Source File Structure

```
Source/
  PluginProcessor.h     - tc_data class definition; AutoSyncAudioProcessor declaration
  PluginProcessor.cpp   - All DSP logic: LTC decoder, delay engine, processBlock
  PluginEditor.h        - AutoSyncAudioProcessorEditor declaration
  PluginEditor.cpp      - GUI layout, timer callback, user controls
JuceLibraryCode/
  JucePluginDefines.h   - Auto-generated plugin metadata macros
  JuceHeader.h          - Aggregated JUCE module header
  include_juce_*.cpp/mm - JUCE module compilation units
AudioSync.jucer   - Projucer project file
```

The entire processing logic lives in **`PluginProcessor.cpp`**. There are four free (non-member) inline functions at file scope:

| Function | Role |
|---|---|
| `handleTimecode()` | Core LTC decoder - operates on one sample |
| `handle_const_delay()` | Feeds the historical sample ring buffer |
| `delay()` | Applies a variable-length delay to one sample |
| `calc_delay()` | Converts two decoded timecodes to a millisecond offset |

One private member function:

| Function | Role |
|---|---|
| `processTimeCode()` | Calls `handleTimecode()` and formats the result string on change |

---

## 3. The `tc_data` Class

`tc_data` is a plain data class (no virtual methods) that holds all mutable state for a single LTC decoder instance. In the current master-slave architecture two instances are used per processor:

| Instance | Purpose |
|---|---|
| `chnl1_in` | Decode the LTC-carrying input channel |
| `chnl2_in` | Decode the secondary input channel (used for two-channel single-instance mode) |

> **Note:** the earlier v1.4 description of four instances (`chnl1_in`, `chnl2_in`, `chnl1`,
> `chnl2`) no longer reflects the current code. The output-side `chnl1`/`chnl2` decoder pair
> was removed when the delay engine was refactored into the master-slave architecture.

### Fields

#### Decoded Timecode Fields

| Field | Type | Description |
|---|---|---|
| `hrs` | `int` | Decoded hours (0–23) |
| `mnts` | `int` | Decoded minutes (0–59) |
| `scnds` | `int` | Decoded seconds (0–59) |
| `frms` | `int` | Decoded frames (0–fps-1) |

#### Bit-Decoder State

| Field | Type | Initial | Description |
|---|---|---|---|
| `buf` | `int[80]` | all 0 | Circular buffer holding the last 80 decoded bits |
| `bufpos` | `int` | 0 | Write pointer into `buf`; advances modulo 80 after each committed bit |
| `gotbit` | `int` | -1 | Counts zero-crossings since last committed bit; -1 = "awaiting first crossing" |
| `syncpos` | `int` | -1 | Counts bits since the last sync word was found; -1 = not yet synced |
| `syncstate` | `int` | 0 | 0 = no sync, 1 = searching, 2 = synced |
| `sillen` | `int` | 0 | Samples elapsed since the last zero-crossing (used for silence detection and bit period timing) |
| `lastsign` | `long double` | 1.0 | Polarity of the signal at the last zero-crossing (+1.0 or -1.0) |

#### Adaptive Threshold

| Field | Type | Description |
|---|---|---|
| `threshold` | `long double` | Running RMS-like envelope; adapts to incoming signal level |
| `threshenv` | `const long double` | Exponential smoothing coefficient for the envelope tracker: `e^(-1 / (0.1 * 44100))`. Time constant ≈ 100 ms at 44100 Hz |
| `minthresh` | `const long double` | Floor for `threshold` (0.0001) to prevent divide-by-near-zero |

#### DC Offset Filter State

| Field | Type | Description |
|---|---|---|
| `itm1` | `long double` | Previous input sample (used in the DC-blocking 1-pole IIR filter) |
| `otm1` | `long double` | Previous output sample (used in the DC-blocking filter) |

#### Timing

| Field | Type | Description |
|---|---|---|
| `pulsesize` | `float` | Duration of one LTC half-pulse in samples; recalculated per block from sample rate and fps |
| `new_time` | `int` | Encoded timecode integer for the most recently decoded frame |
| `old_time` | `int` | Last value of `new_time` that was acted upon; used to detect frame changes |
| `timecode_counter` | `int` | Counts consecutive frames where the measured delay exceeded the 10-second outlier threshold |

#### Delay Engine State

| Field | Type | Description |
|---|---|---|
| `delay_buf` | `std::deque<float>` | FIFO for the active delay line |
| `delay_size` | `size_t` | Target length of `delay_buf` in samples; set once when delay is first activated |
| `active_delay` | `bool` | True once this channel has been designated as the one to be delayed |
| `const_buf` | `std::deque<float>` | Ring buffer of the last 500,000 input samples (~11.3 s at 44100 Hz) |
| `const_buf_size` | `const int` | Fixed capacity of `const_buf` (500,000) |

#### Quality Scoring Fields (added post-v1.4)

These fields are computed by the windowed quality scorer that runs inside `handleTimecode()`
every `W_SIZE` frames. See `quality_scoring.md` for full derivation.

| Field | Type | Description |
|---|---|---|
| `Q_LTC` | `float` | Composite quality score [0.0–1.0]; weighted sum of 7 sub-metrics |
| `ltc_state` | `LTCState` | Enum: `FAIL` (Q<0.5), `SUSPECT` (0.5–0.8), `VALID` (>0.8) |
| `fallback_requested` | `bool` | True when `ltc_state == FAIL`; signals fusion layer |
| `q_lock_ratio` | `float` | Fraction of measurement window where sync was held |
| `q_continuity_score` | `float` | Smoothed inter-frame timecode continuity |
| `q_pulse_consistency` | `float` | Pulse-width variance relative to expected half-pulse |
| `q_transition_reliability` | `float` | Fraction of transitions that fell in expected timing window |
| `q_signal_strength` | `float` | Normalised threshold relative to the floor |
| `q_sync_word_rate` | `float` | Fraction of frames with correct sync word |
| `q_fps_plausibility` | `float` | How closely measured FPS matches the configured rate |
| `estimated_fps` | `float` | FPS measured from half-pulse width; used for plausibility metric |
| `rejected_frames_count` | `int` | Count of frames rejected by the scorer in the current window |

### `clear()` Method

Resets all decoder and delay state to initial values. Called when the measured delay changes by more than one frame (indicating a sync jump), to ensure the delay line is re-established with fresh measurements.

---

## 4. SMPTE LTC Timecode - Format Reference

LTC (Longitudinal Timecode, SMPTE 12M) is an audio-bandwidth signal that encodes a timecode of the form **HH:MM:SS:FF** (hours, minutes, seconds, frames).

### Biphase Mark Coding (BMC)

LTC uses Biphase Mark Code (also called FM or differential Manchester coding) to encode bits:

- **Every** bit cell begins with a transition (polarity flip).
- A **'1'** bit has an **additional** transition at the **mid-point** of the cell, yielding **two** transitions per cell.
- A **'0'** bit has **no** mid-cell transition, yielding **one** transition per cell.

This means the decoder never needs to know absolute polarity - only the count of transitions within a bit period determines the bit value.

### Frame Structure

Each LTC frame consists of exactly **80 bits**, transmitted LSB-first within each field. At 30 fps the total bit rate is 30 × 80 = 2400 bits/second.

| Bit positions | Field |
|---|---|
| 0–3 | Frame units (BCD, 4 bits) |
| 4–7 | User bits group 1 |
| 8–9 | Frame tens (BCD, 2 bits: encodes 0–2) |
| 10 | Drop-frame flag |
| 11 | Color-frame flag |
| 12–15 | User bits group 2 |
| 16–19 | Seconds units (BCD, 4 bits) |
| 20–23 | User bits group 3 |
| 24–26 | Seconds tens (BCD, 3 bits: encodes 0–5) |
| 27 | Biphase mark correction bit |
| 28–31 | User bits group 4 |
| 32–35 | Minutes units (BCD, 4 bits) |
| 36–39 | User bits group 5 |
| 40–42 | Minutes tens (BCD, 3 bits: encodes 0–5) |
| 43 | Binary group flag |
| 44–47 | User bits group 6 |
| 48–51 | Hours units (BCD, 4 bits) |
| 52–55 | User bits group 7 |
| 56–57 | Hours tens (BCD, 2 bits: encodes 0–2) |
| 58 | Binary group flag |
| 59 | Reserved |
| 60–63 | User bits group 8 |
| 64–79 | **Sync word** `0011111111111101` |

The sync word `0011111111111101` is unique in LTC: it is the only 16-bit sequence guaranteed not to occur in the data fields (because the biphase mark correction bit at position 27 ensures no run of more than 12 ones can occur in the data region). It marks the end of each frame and enables the decoder to find frame boundaries.

### Supported Frame Rates

| fps | Description |
|---|---|
| 30 | SMPTE 30 fps (default; used for NTSC non-drop) |
| 25 | EBU 25 fps (PAL) |

The `fps` selector in the GUI (ComboBox) sets `audioProcessor.fps`, which controls both the `pulsesize` calculation and the frame-count arithmetic in `calc_delay()`.

---

## 5. LTC Decoder - `handleTimecode()`

```cpp
inline void handleTimecode(const long double& sample, tc_data& data,
                           const int& srate, const int& slider = 0)
```

This is the innermost DSP function. It is called **once per audio sample** for each of the four `tc_data` instances. The `slider` parameter is an index into a static frame-rate lookup table, not a UI slider.

```cpp
static const double frates[] = {30, 24, 25, 30000.0 / 1001};
```

The frame-rate table contains four entries (30, 24, 25, 29.97), indexed by `slider`. The GUI only exposes indices 0 (30 fps) and 2 (25 fps); indices 1 and 3 exist in the table but are unused by current UI code.

### 5.1 Pulse Size Calculation

```cpp
data.pulsesize = srate / frates[slider] / 160;
```

**Derivation:**

- One video frame lasts `1 / fps` seconds.
- One LTC frame carries 80 bits, each encoded as two half-pulses.
- Therefore one LTC frame contains `80 × 2 = 160` half-pulses.
- The duration of one half-pulse is `(1/fps) / 160 = srate / fps / 160` samples.

At 44100 Hz and 30 fps: `44100 / 30 / 160 = 9.1875` samples/half-pulse.
This matches the hardcoded default value `float pulsesize = 9.1875` in `tc_data`.

At 44100 Hz and 25 fps: `44100 / 25 / 160 = 11.025` samples/half-pulse.

`pulsesize` is recalculated every time `handleTimecode()` is called, so an fps change takes effect immediately without any state reset.

### 5.2 DC Offset Removal (High-Pass Filter)

```cpp
data.otm1 = 0.999 * data.otm1 + sample - data.itm1;
data.itm1 = sample;
const long double s = data.otm1;
```

This is a first-order IIR DC-blocking filter with the transfer function:

```
H(z) = (1 - z^-1) / (1 - 0.999 * z^-1)
```

**Derivation:**
- `itm1` = x[n-1] (previous input)
- `otm1` = y[n-1] (previous output)
- `y[n] = 0.999 * y[n-1] + x[n] - x[n-1]`

This is equivalent to a high-pass filter with a 3 dB cutoff of approximately `(1 - 0.999) / (2π) ≈ 0.16 Hz`. It removes any DC bias that may be present in the LTC audio signal (a common artifact from consumer audio equipment) while passing all audio frequencies including the lowest LTC fundamental.

The filtered sample `s` is used for all subsequent processing.

### 5.3 Silence / Loss-of-Signal Detection

```cpp
++data.sillen;

if (data.sillen > data.pulsesize * 2.2)
{
    data.syncpos = -1;
    data.sillen = 0;
    data.gotbit = -1;
    data.syncstate = 1;
}
```

`sillen` counts audio samples since the last detected zero-crossing. If more than `2.2 × pulsesize` samples pass without any transition (i.e., longer than one complete BMC bit cell), the decoder considers the signal absent or corrupted and resets:

- `syncpos = -1` - loses frame sync
- `gotbit = -1` - discards any partially accumulated bit
- `syncstate = 1` - enters "searching for sync" state

At 30 fps: `2.2 × 9.1875 ≈ 20.2` samples (≈ 0.46 ms). This is tight enough to detect even a single dropped cycle.

### 5.4 Adaptive Amplitude Threshold

```cpp
data.threshold = data.threshold * data.threshenv
               + std::abs(s) * (1 - data.threshenv);
if (data.threshold < data.minthresh)
    data.threshold = data.minthresh;
```

`threshold` is a running exponential moving average of the absolute (rectified) signal amplitude. The smoothing coefficient `threshenv = e^(-1 / (0.1 × 44100))` gives a time constant of 100 ms - slow enough to follow gradual level changes but fast enough to track level drops.

**Purpose:** LTC signals may arrive at varying levels depending on the playback device and cable length. A fixed threshold would either miss weak signals or trigger spuriously on noise. The adaptive threshold anchors the crossing detector at 80% of the recent peak amplitude (`±threshold × 0.8`).

The floor `minthresh = 0.0001` prevents the threshold from collapsing to zero during a silent period, which would cause false triggers on noise once signal resumes.

### 5.5 Zero-Crossing / Transition Detection

```cpp
if ((s < -data.threshold * 0.8 && data.lastsign > 0)
 || (s > data.threshold * 0.8 && data.lastsign < 0))
{
    data.lastsign *= -1;
    ++data.gotbit;
    // ... bit commit check
}
```

A transition is detected when the signal crosses 80% of the adaptive threshold in the opposite direction from the previous crossing. The hysteresis factor of 0.8 prevents spurious re-triggers from noise near the zero line.

- `lastsign` tracks the current polarity (+1.0 or −1.0). It is flipped on every detected transition.
- `gotbit` is incremented on every transition. It accumulates the transition count since the last committed bit.

**Why 80%?** The BMC waveform is roughly square. Triggering slightly below the peak (rather than at zero) makes the crossing time more reproducible under band-limited conditions.

### 5.6 Bit Decoding (Biphase Mark Code)

```cpp
if (data.sillen > data.pulsesize * 1.8)
{
    data.gotbit = std::min(data.gotbit, 1);
    data.sillen = 0;

    data.buf[data.bufpos] = data.gotbit;
    // advance bufpos ...
    data.gotbit = -1;
}
```

A bit is **committed** when the interval since the last committed bit exceeds `1.8 × pulsesize` samples (≈ one full BMC bit cell). At that point:

| `gotbit` value at commit time | Interpretation | Committed bit |
|---|---|---|
| 0 (exactly one crossing since last commit) | Only the mandatory BMC boundary transition occurred - no mid-cell transition | **0** |
| 1 (two crossings) | Both boundary and mid-cell transitions occurred | **1** |
| ≥2 (clamped to 1 by `std::min`) | Multiple transitions (noise or glitch) - treated as **1** |

**Why 1.8 × pulsesize and not exactly 2.0?** The factor 1.8 adds 10% tolerance below the expected bit period, accommodating small timing jitter from the LTC source clock without waiting for the full 2.0 × pulsesize.

After committing, `gotbit` is reset to -1 so the **next** boundary transition (which is guaranteed by BMC) increments it to 0, ready for the following bit.

### 5.7 Circular Bit Buffer

```cpp
data.buf[data.bufpos] = data.gotbit;
if (++data.bufpos >= 80)
    data.bufpos = 0;
```

`buf` is a 80-element integer array used as a circular (ring) buffer. `bufpos` is the write pointer, advanced modulo 80 after each write.

After a write, `bufpos` points to the slot that will receive the **next** bit. Therefore:

| Array index expression | Which LTC bit it holds |
|---|---|
| `(bufpos + 0) % 80` | Oldest bit in buffer (80 bits ago) → LTC bit 0 of current frame |
| `(bufpos + k) % 80` | LTC bit k of current frame |
| `(bufpos + 79) % 80` | Most recently written bit → LTC bit 79 |

This means the circular index arithmetic `(bufpos + k) % 80` naturally maps buffer slot offsets to LTC frame bit positions, making the sync word check and field extraction straightforward.

### 5.8 Sync Word Detection

```cpp
if (data.syncpos < 0 || data.syncpos >= 80)
{
    data.syncpos = -1;

    if (
        data.buf[(data.bufpos + 64) % 80] == 0 &&
        data.buf[(data.bufpos + 65) % 80] == 0 &&
        data.buf[(data.bufpos + 66) % 80] == 1 &&
        // ... bits 67–77 all == 1 ...
        data.buf[(data.bufpos + 78) % 80] == 0 &&
        data.buf[(data.bufpos + 79) % 80] == 1
    ) { /* frame parsed */ }
}
```

After every committed bit, the decoder checks whether the most recent 16 bits (`bufpos+64` through `bufpos+79`) match the LTC sync word:

```
Bit positions 64–79:  0 0 1 1 1 1 1 1 1 1 1 1 1 1 0 1
                      ^                             ^
                     bit 64                       bit 79
```

In binary: `0011111111111101` (reading left to right, bit 64 first).

The check is performed only when `syncpos < 0` (not yet synced) or `syncpos >= 80` (a full frame has elapsed since last sync - frame-boundary re-lock). Once a sync word is detected, `syncpos` is set to 0 and incremented each subsequent committed bit; when it reaches 80 the frame has completed and the check is repeated.

### 5.9 Time Field Extraction (BCD Decoding)

Upon sync word detection, all four time fields are extracted from the circular buffer using BCD (Binary Coded Decimal) arithmetic.

BCD represents decimal digits in 4-bit groups. Two groups represent a two-digit decimal number:
- **Units digit** = bits weighted 1, 2, 4, 8
- **Tens digit** = bits weighted 1, 2 (or 1, 2, 4 for seconds/minutes)

#### Frames

```cpp
data.frms = data.buf[(data.bufpos + 0) % 80] * 1   // bit 0: units 2^0
          + data.buf[(data.bufpos + 1) % 80] * 2    // bit 1: units 2^1
          + data.buf[(data.bufpos + 2) % 80] * 4    // bit 2: units 2^2
          + data.buf[(data.bufpos + 3) % 80] * 8    // bit 3: units 2^3
          + 10 * (
              data.buf[(data.bufpos + 8) % 80] * 1  // bit 8: tens 2^0
            + data.buf[(data.bufpos + 9) % 80] * 2  // bit 9: tens 2^1
            );
```

- Bits 0–3: units digit of frames (range 0–9)
- Bits 8–9: tens digit of frames (range 0–2, giving totals 0–29)
- Bits 4–7 are user bits (ignored)
- Result range: 0–29 (at 30 fps) or 0–24 (at 25 fps)

#### Seconds

```cpp
data.scnds = data.buf[(data.bufpos + 16) % 80] * 1
           + data.buf[(data.bufpos + 17) % 80] * 2
           + data.buf[(data.bufpos + 18) % 80] * 4
           + data.buf[(data.bufpos + 19) % 80] * 8
           + 10 * (
               data.buf[(data.bufpos + 24) % 80] * 1
             + data.buf[(data.bufpos + 25) % 80] * 2
             + data.buf[(data.bufpos + 26) % 80] * 4
             );
```

- Bits 16–19: units digit of seconds (0–9)
- Bits 24–26: tens digit of seconds (0–5, giving totals 0–59)
- Bits 20–23 and bit 27 are user/correction bits (ignored)

#### Minutes

```cpp
data.mnts = data.buf[(data.bufpos + 32) % 80] * 1
          + data.buf[(data.bufpos + 33) % 80] * 2
          + data.buf[(data.bufpos + 34) % 80] * 4
          + data.buf[(data.bufpos + 35) % 80] * 8
          + 10 * (
              data.buf[(data.bufpos + 40) % 80] * 1
            + data.buf[(data.bufpos + 41) % 80] * 2
            + data.buf[(data.bufpos + 42) % 80] * 4
            );
```

- Bits 32–35: units digit of minutes (0–9)
- Bits 40–42: tens digit of minutes (0–5)

#### Hours

```cpp
data.hrs = data.buf[(data.bufpos + 48) % 80] * 1
         + data.buf[(data.bufpos + 49) % 80] * 2
         + data.buf[(data.bufpos + 50) % 80] * 4
         + data.buf[(data.bufpos + 51) % 80] * 8
         + 10 * (
             data.buf[(data.bufpos + 56) % 80] * 1
           + data.buf[(data.bufpos + 57) % 80] * 2
           );
```

- Bits 48–51: units digit of hours (0–9)
- Bits 56–57: tens digit of hours (0–2, giving totals 0–23)

### 5.10 Change Detection via `new_time`

```cpp
data.new_time = ((data.hrs * 80 + data.mnts) * 80 + data.scnds) * 100 + data.frms;
```

`new_time` is a single integer encoding the full timecode for fast inequality comparison. The multipliers (80, 80, 100) are not strictly correct for timecode arithmetic (proper multipliers would be 60, 60, fps) but they are large enough to guarantee uniqueness for all valid timecode values in the expected range. This value is only used to detect changes, never for arithmetic.

In `processTimeCode()`, `new_time != old_time` triggers a string format update and sets `old_time = new_time`.

---

## 6. `processTimeCode()` - Per-Sample Wrapper

```cpp
inline void AutoSyncAudioProcessor::processTimeCode(
    const float& sample, tc_data& channel, std::string& msg,
    const int& index, const float& srate, const int& slider)
```

This private member function is the per-sample entry point called from `processBlock()`. It:

1. Calls `handleTimecode(sample, channel, srate, slider)` to advance the decoder by one sample.
2. Checks `channel.new_time != channel.old_time` - i.e., whether a new frame has been decoded.
3. If a new frame is available, formats it as a `HH:MM:SS:FF` string:

```cpp
msg = std::to_string(channel.hrs / 10) + std::to_string(channel.hrs % 10) + ":"
    + std::to_string(channel.mnts / 10) + std::to_string(channel.mnts % 10) + ":"
    + std::to_string(channel.scnds / 10) + std::to_string(channel.scnds % 10) + ":"
    + std::to_string(channel.frms / 10) + std::to_string(channel.frms % 10);
```

Each digit is extracted by integer division and modulo to produce exactly two characters per field, zero-padded (e.g., `hrs=5` → `"05"`). The result is written to one of the public `std::string` members of `AutoSyncAudioProcessor` (`tc`, `output_c2`, `input_ch1`, `input_ch2`), which the GUI timer reads on the message thread.

The `srate` parameter defaults to `44100` (hardcoded). The plugin does not query `getSampleRate()` dynamically; see [Section 13](#13-known-limitations-and-design-notes).

---

## 7. `calc_delay()` - Timecode Difference Calculation

```cpp
inline double calc_delay(tc_data& data1, tc_data& data2, int fps = 30)
```

### 7.1 Frame-Based Arithmetic

Both timecodes are converted to an absolute frame count since 00:00:00:00:

```cpp
int time1_in_frames = fps * 3600 * h1 + fps * 60 * m1 + fps * s1 + f1;
int time2_in_frames = fps * 3600 * h2 + fps * 60 * m2 + fps * s2 + f2;
```

The difference in milliseconds:

```cpp
const double delay_ms = (time2_in_frames - time1_in_frames) * 1000.0 / fps;
```

`delay_ms` is positive when channel 2 is ahead of channel 1 (i.e., channel 2 has a larger timecode value). The frame difference is also stored in the file-scope variable `delay_frames` and exposed via `audioProcessor.delay_ms`.

**Sign convention:**
- `delay_ms > 0`: channel 2 is temporally ahead → channel 2 must be delayed
- `delay_ms < 0`: channel 1 is temporally ahead → channel 1 must be delayed

### 7.2 Stability / Outlier Filter

Large delay values can arise from:
- Initial conditions where one decoder has locked and the other has not
- Timecode wrapping (e.g., near 00:00:00:00)
- LTC corruption / dropout

A two-tier filter is applied:

**Tier 1 - Early rejection (counter < 20):**
```cpp
if (delay_ms > 10000)
{
    ++data2.timecode_counter;
    return 0;
}
if (delay_ms < -10000)
{
    ++data1.timecode_counter;
    return 0;
}
```

If `|delay_ms| > 10 seconds` and neither counter has exceeded 20, the measurement is rejected (returns 0) and the counter for the channel with the "leading" timecode is incremented. The 10-second threshold is chosen because no realistic synchronization problem should exceed it - if one channel shows a timecode more than 10 seconds ahead of the other, it is almost certainly a decoder error.

**Tier 2 - Acceptance after repeated confirmation:**
```cpp
if (data1.timecode_counter > 20 || data2.timecode_counter > 20
    && std::abs(delay_ms) > 10000)
{
    delay_frames = time2_in_frames - time1_in_frames;
    return delay_ms;
}
```

After one channel has produced more than 20 consecutive large-delay readings, the filter concludes that the delay is genuine (e.g., the recordings truly start far apart) and accepts it. The counters are reset when a measurement within ±10 seconds is seen:

```cpp
data1.timecode_counter = 0;
data2.timecode_counter = 0;
```

**Note:** There is a C++ operator precedence bug in the Tier 2 check:
```cpp
data1.timecode_counter > 20 || data2.timecode_counter > 20 && std::abs(delay_ms) > 10000
```
Due to `&&` binding more tightly than `||`, this evaluates as:
```
(data1.timecode_counter > 20) || (data2.timecode_counter > 20 && abs(delay_ms) > 10000)
```
This means if only `data1.timecode_counter > 20`, large delays are accepted regardless of which direction. This is likely unintentional but has limited practical impact given the 10-second threshold.

---

## 8. Audio Delay Engine

The delay engine consists of two components: a constantly-updated historical ring buffer and the active delay FIFO.

### 8.1 `handle_const_delay()` - Historical Lookahead Buffer

```cpp
inline void handle_const_delay(const float& sample, tc_data& data)
{
    data.const_buf.push_back(sample);
    if (data.const_buf.size() == data.const_buf_size)
        data.const_buf.pop_front();
}
```

This function is called for **channel 1 only** on every sample, before any delay is applied, and regardless of whether the delay is active. It maintains a rolling window of the last 500,000 input samples:

- At 44100 Hz: `500000 / 44100 ≈ 11.34 seconds` of audio history
- Implemented as a `std::deque<float>` with size capped at `const_buf_size`

**Purpose:** When the delay is first activated, if the required delay is less than the amount of history in `const_buf`, the plugin can immediately output properly delayed audio by reading from this historical buffer rather than outputting silence. This avoids the silence artifact that would otherwise occur during the initial fill of the delay line.

Only `chnl1` populates `const_buf` (see `processBlock()`). Channel 2 does not have a pre-filled history buffer - it uses the standard FIFO-based delay with initial silence.

### 8.2 `delay()` - Sample-Level Delay Line

```cpp
inline void delay(float* writePtr, const int& index, tc_data& data)
```

This function modifies `writePtr[index]` in-place. Three cases:

**Case 1 - Serve from historical buffer:**
```cpp
if (data.delay_size < data.const_buf.size())
{
    writePtr[index] = data.const_buf[data.const_buf.size() - data.delay_size];
    return;
}
```

If the requested delay is smaller than the amount of history available, the output is taken directly from `const_buf` at offset `delay_size` from the end. This is only applicable to channel 1 (the only channel that populates `const_buf`). The index `const_buf.size() - delay_size` addresses the sample that entered the buffer exactly `delay_size` samples ago.

**Case 2 - Filling the FIFO:**
```cpp
if (data.delay_buf.size() < data.delay_size)
{
    float tmp = writePtr[index];
    data.delay_buf.push_back(tmp);
    writePtr[index] = 0;
}
```

While the delay FIFO is not yet full, incoming samples are queued but the output is zero (silence). This is the initial fill phase.

**Case 3 - Steady-state FIFO delay:**
```cpp
else
{
    float tmp = writePtr[index];
    data.delay_buf.push_back(tmp);
    float tmp2 = data.delay_buf[0];
    writePtr[index] = tmp2;
    data.delay_buf.pop_front();
}
```

The new sample is appended to the back of the deque and the oldest sample is taken from the front, achieving a delay of exactly `delay_size` samples. This is a standard first-in/first-out delay line.

---

## 9. `processBlock()` - Main Audio Processing Loop

> **This section describes the v1.4 single-instance design and is significantly outdated.**
> The current `processBlock()` implements a master-slave multi-track architecture with audio
> fallback, alpha-beta smoothing, anchored NCC, and hysteresis on large delay jumps.
> For the current processing loop, see `fallback_docs.md §9` and
> `master-slave-architecture.md`. The sub-sections below are retained as historical reference
> for the core `handleTimecode` → `calc_delay` → `delay()` pipeline, which is still present.

```cpp
void AutoSyncAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& midiMessages)
```

This is the real-time audio callback. It is called by the host with a buffer of audio samples and a MIDI buffer.

### 9.1 Channel Routing

```cpp
auto* write1 = buffer.getWritePointer(0);  // Channel 1 (left)
auto* write2 = buffer.getWritePointer(1);  // Channel 2 (right)
```

The plugin operates on exactly two channels. Excess output channels (if any) are cleared. Both channels carry LTC audio on input and produce delayed (or pass-through) LTC audio on output.

### 9.2 MIDI Output

Before the per-sample loop, one batch of MIDI messages is emitted at sample position 0:

```cpp
int value = static_cast<int>(
    ((std::abs(d_ms) + by_slider) / 10000) * 16383.0f);
if (value < 0) value = 0;
int valueMSB = (value >> 7) & 0x7F;
int valueLSB = value & 0x7F;

midiMessages.addEvent(juce::MidiMessage::controllerEvent(1,  6, valueMSB), 0);
midiMessages.addEvent(juce::MidiMessage::controllerEvent(1, 38, valueLSB), 0);
midiMessages.addEvent(juce::MidiMessage::pitchWheel(1, value), 0);
```

**Encoding:**
- The total delay (`|d_ms|` + `by_slider` in ms) is scaled to the range 0–16383 over a 0–10000 ms span.
- It is split into a 7-bit MSB and 7-bit LSB.
- **CC 6** (MIDI RPN MSB / High-resolution controller) carries the MSB on MIDI channel 1.
- **CC 38** (LSB for CC 6, per MIDI standard) carries the LSB. Together, CC 6+38 form a standard 14-bit high-resolution controller message with 16384 steps over 10 seconds.
- **Pitch wheel** carries the same 14-bit value. Pitch bend natively uses 14-bit resolution (0–16383), providing an alternative modulation target.

These MIDI messages are added once per `processBlock()` call (not per sample), using the value of `d_ms` and `by_slider` that was current at the end of the previous block.

### 9.3 Per-Sample Processing Loop

```cpp
for (int i = 0; i < buffer.getNumSamples(); ++i)
{
    handle_const_delay(write1[i], chnl1);

    processTimeCode(write1[i], chnl1_in, input_ch1, i);  // or with fps=25 variant
    processTimeCode(write2[i], chnl2_in, input_ch2, i);

    d_ms = calc_delay(chnl1_in, chnl2_in, fps);
    // ... stability check, delay apply, output TC decode
}
```

For each sample:

1. `handle_const_delay` feeds the pre-fill buffer for channel 1.
2. Both input channels are decoded via `processTimeCode` → `handleTimecode`.
3. `calc_delay` computes the current frame offset and millisecond delay.
4. Stability filtering may reset the delay state (see §9.4).
5. If delay is active, the appropriate channel is delayed and output timecodes are decoded (§9.5).
6. If delay is inactive, output TC strings are mirrored from input and delay state is reset (§9.6).

### 9.4 Delay Stability Smoothing

```cpp
if (std::abs(prev_frames - delay_frames) > 1)
{
    chnl1.clear();
    chnl2.clear();
}
else if (std::abs(prev_frames - delay_frames) == 1)
{
    if (d_ms > prev_ms)
    {
        d_ms = prev_ms;
        delay_frames = prev_frames;
    }
}
```

`prev_frames` holds the `delay_frames` value from the previous sample. Because timecode only updates once per video frame (≈ 1/30 s = 1470 samples at 30 fps), `delay_frames` should be constant within a frame and change by at most ±1 at a frame boundary.

- **Jump > 1 frame:** Indicates a measurement discontinuity (new LTC lock, corruption, or channel reset). Both output delay states (`chnl1`, `chnl2`) are fully cleared and the delay will be re-established from scratch.
- **Jump == 1 frame:** May indicate a frame boundary transition or a one-frame measurement jitter. The conservative choice is taken: if the new delay is larger than the previous delay, keep the previous (smaller) value. This prevents the delay line from growing unexpectedly due to a stale measurement on one channel.

### 9.5 Active Delay Routing

When `active_delay == true` (set by the GUI button):

**Direction determination (one-time, per activation):**
```cpp
if (d_ms > 0 && !chnl1.active_delay && !chnl2.active_delay)
{
    chnl2.active_delay = true;   // ch2 is ahead → delay ch2
    chnl1.active_delay = false;
}
if (d_ms < 0 && !chnl1.active_delay && !chnl2.active_delay)
{
    chnl2.active_delay = false;
    chnl1.active_delay = true;   // ch1 is ahead → delay ch1
}
```

The direction is determined the first time a non-zero `d_ms` is seen after activation (when neither channel yet has `active_delay == true`). Once set, the direction does not change until `chnl1.clear()` / `chnl2.clear()` is called (e.g., on a large delay jump).

**Delay application:**
```cpp
if (chnl2.active_delay)
{
    if (chnl2.delay_size == 0)
        chnl2.delay_size = std::floor(d_ms / 1000 * 44100);
    delay(write2, i, chnl2);
}
if (chnl1.active_delay)
{
    if (!chnl1.delay_size)
        chnl1.delay_size = std::floor(std::abs(d_ms) / 1000 * 44100);
    delay(write1, i, chnl1);
}
```

`delay_size` is set **once** (when it is zero/false) from the current `d_ms` measurement and is held fixed for the entire delay session. This freeze prevents the delay line length from continuously changing as `d_ms` fluctuates at frame boundaries.

Conversion: `delay_size = floor(|d_ms| / 1000 × 44100)` converts milliseconds to samples at 44100 Hz.

**Output TC decode (after delay):**
After applying the delay, the output samples are decoded through `chnl1` and `chnl2` to verify and display the aligned timecodes in the OUTPUT section of the GUI.

### 9.6 Inactive (Bypass) Mode

```cpp
else  // active_delay == false
{
    tc = input_ch1;
    output_c2 = input_ch2;
    chnl1.clear();
    chnl2.clear();
}
```

When delay is inactive:
- Output TC display strings mirror the input TC strings.
- Both output `tc_data` states are cleared on every sample. This ensures a clean state when the user subsequently activates the delay.

---

## 10. GUI - `PluginEditor`

> **This section describes the v1.4 UI and is outdated.** The UI was completely redesigned
> into a dark-themed 4-card layout (header, signal, delay sync, diagnostics). The
> absolute-pixel layout described below no longer exists in `PluginEditor.cpp`.

`AutoSyncAudioProcessorEditor` extends `juce::AudioProcessorEditor` and `juce::Timer`. The editor window is fixed at **400 × 300 pixels**.

### 10.1 Layout and Controls

All layout is done in `resized()` with absolute pixel positions (no responsive layout).

| Control | Type | Position | Purpose |
|---|---|---|---|
| `input` | `Label` | (10, 0) | Static text "INPUT" |
| `timecode_input1` | `Label` | (10, 10) | Live display of `input_ch1` (ch1 input TC) |
| `timecode_input2` | `Label` | (10, 30) | Live display of `input_ch2` (ch2 input TC) |
| `output` | `Label` | right-anchored at x=390, y=0 | Static text "OUTPUT" |
| `timecode_box` | `Label` | right-anchored | Live display of `tc` (ch1 output TC) |
| `timecode_box_chanel2` | `Label` | right-anchored | Live display of `output_c2` (ch2 output TC) |
| `delay` | `Label` | (160, 120) | Static text "IN DELAY, frames" |
| `delay_box` | `Label` | (160, 150) | Live display of `delay_ms` (frame count) |
| `o_delay` | `Label` | (160, 75) | Static text "IN DELAY, ms" |
| `o_delay_box` | `Label` | (160, 105) | Live display of `o_delay_ms` (ms value) |
| `delay_slider` | `Slider` | (10, 120), 150×50 | Manual delay trim, range −1500 to +1500 ms |
| `delay_button` | `TextButton` | right of center, y=200 | Toggles `active_delay` |
| `delay_state_label` | `Label` | near button | Shows "ACTIVE" / "INACTIVE" |
| `show_MIDI` | `Label` | (290, 170) | Static text "OUT MIDI, ms" |
| `value_MIDI` | `Label` | (290, 180) | Live display of total MIDI output value in ms |
| `fps_label` | `Label` | (110, 240) | Shows currently selected fps |
| `fps_box` | `ComboBox` | (160, 250) | FPS selector: "30FPS" (id=1), "25FPS" (id=2) |
| `version` | `Label` | (290, 230) | Static text "ver 1.4" |

### 10.2 Timer-Based State Polling

```cpp
startTimer(1);  // 1 ms interval → ~1000 callbacks/second
```

The editor starts a 1 ms JUCE timer in its constructor. `timerCallback()` is called on the **message thread** at this rate to poll `audioProcessor`'s public state variables and update label text:

```cpp
void AutoSyncAudioProcessorEditor::timerCallback()
{
    timecode_box.setText(audioProcessor.tc, juce::dontSendNotification);
    timecode_box_chanel2.setText(audioProcessor.output_c2, juce::dontSendNotification);
    delay_box.setText(audioProcessor.delay_ms, juce::dontSendNotification);
    timecode_input1.setText(audioProcessor.input_ch1, juce::dontSendNotification);
    timecode_input2.setText(audioProcessor.input_ch2, juce::dontSendNotification);
    o_delay_box.setText(audioProcessor.o_delay_ms, juce::dontSendNotification);
    value_MIDI.setText(std::to_string(std::floor(audioProcessor.by_slider)
                                    + std::abs(audioProcessor.d_ms)), ...);
    // ...
    repaint();
}
```

`juce::dontSendNotification` is used to avoid triggering change callbacks on the label components, since these are display-only. `repaint()` is called at the end to redraw the component.

**Thread safety note:** `audioProcessor.tc`, `input_ch1`, etc. are `std::string` members written by the audio thread and read by the message thread without any lock. This is a data race by the C++ standard. In practice at 44100 Hz and with a 1 ms GUI timer, updates are rare and incoherent reads are benign (worst case: briefly garbled text), but it is a known design limitation.

### 10.3 FPS Selector

```cpp
fps_box.addItem("30FPS", 1);
fps_box.addItem("25FPS", 2);
fps_box.setSelectedId(1);

fps_box.onChange = [&]() {
    const auto id = fps_box.getSelectedId();
    if (id == 1) { audioProcessor.fps = 30; fps_label.setText("30FPS", ...); }
    else if (id == 2) { audioProcessor.fps = 25; fps_label.setText("25FPS", ...); }
};
```

Setting `audioProcessor.fps` affects:
- The `slider` index passed to `processTimeCode` (0 for 30 fps, 2 for 25 fps), which selects the frame rate from `frates[]`.
- The fps argument passed to `calc_delay()`.

The mapping is:

| `audioProcessor.fps` | `slider` passed to `processTimeCode` | `frates[slider]` |
|---|---|---|
| 30 | 0 | 30.0 |
| 25 | 2 | 25.0 |

### 10.4 Delay Slider

```cpp
delay_slider.setRange(-1500, 1500);
delay_slider.setValue(0);
delay_slider.setTextValueSuffix("ms");

delay_slider.onValueChange = [&]() {
    this->delay_by_slider = delay_slider.getValue();
    audioProcessor.by_slider = this->delay_by_slider;
};
```

The slider provides a manual correction trim in the range **−250 to +250 ms**. Its value is stored in `audioProcessor.by_slider` and is applied in three places:
- **Audio delay line:** `targetMs = d_ms + by_slider` (or `ab.estMs + by_slider` during fallback) - the slider shifts the actual applied audio delay
- **MIDI output:** `|d_ms| + by_slider` in the 14-bit CC/pitch-wheel value
- **DAW parameter:** `(|targetMs| + floor(by_slider)) / 4000`, normalised to 0–1

The `by_slider` offset is added *after* the LTC/NCC estimation pipeline, so the anchor, NCC search window, and alpha-beta tracker all operate in raw-delay space. The slider is a post-estimation trim only and does not disturb the fallback algorithm's internal state.

### 10.5 Delay Activate Button

```cpp
delay_button.onClick = [&]() {
    audioProcessor.active_delay = !audioProcessor.active_delay;
};
```

Toggles `active_delay` on the processor. When transitioning from active to inactive, the audio thread's per-sample loop calls `chnl1.clear()` and `chnl2.clear()` on every sample, ensuring a clean state.

---

## 11. MIDI Parameter (`myParameter`)

```cpp
addParameter(myParameter = new juce::AudioParameterFloat(
    "myParam", "Delay, ms", 0.0f, 4000.0f, 0.0f));
```

`myParameter` is a standard JUCE `AudioParameterFloat` registered with the processor. Range: 0 to 4000 ms, default 0.

It is updated per sample in `processBlock()`:
```cpp
myParameter->setValueNotifyingHost(
    static_cast<float>((std::abs(d_ms) + std::floor(by_slider)) / 4000));
```

The value is normalized to 0.0–1.0 over the 0–4000 ms range. Calling `setValueNotifyingHost()` informs the host DAW of the parameter change, enabling:
- Automation recording
- Modulation routing in the host
- External control mapping

The parameter is exposed alongside the MIDI CC and pitch wheel messages, giving three independent mechanisms for downstream devices to consume the measured delay.

---

## 12. Plugin Configuration (`JucePluginDefines.h`)

Key build-time settings:

| Define | Value | Meaning |
|---|---|---|
| `JucePlugin_Build_VST3` | 1 | Build as VST3 |
| `JucePlugin_Build_AU` | 1 | Build as Audio Unit (macOS) |
| `JucePlugin_Build_Standalone` | 1 | Build as standalone app |
| `JucePlugin_WantsMidiInput` | 1 | Accept incoming MIDI |
| `JucePlugin_ProducesMidiOutput` | 1 | Emit MIDI output |
| `JucePlugin_IsMidiEffect` | 0 | Not a MIDI-only effect (has audio I/O) |
| `JucePlugin_IsSynth` | 0 | Not a synthesizer |
| `JucePlugin_Name` | "AudioSync" | Plugin display name |
| `JucePlugin_Vst3Category` | "Fx" | VST3 category |
| `JucePlugin_VSTNumMidiInputs` | 16 | Supports up to 16 MIDI input channels |
| `JucePlugin_VSTNumMidiOutputs` | 16 | Supports up to 16 MIDI output channels |

---

## 13. Known Limitations and Design Notes

### ~~Hardcoded Sample Rate~~ - FIXED

~~`processTimeCode()` defaults `srate` to `44100`.~~ The current implementation passes
`currentSampleRate` (stored from `prepareToPlay()`) to all decoder calls. The plugin
operates correctly at 44100, 48000, 96000 Hz and other DAW sample rates.

### ~~Hardcoded Delay Sample Rate~~ - FIXED

~~The delay engine hardcoded `44100` in `delay_size` computation.~~ The current code derives
`delay_size` from `currentSampleRate`, so delay accuracy is maintained at any sample rate.

### ~~`delay_frames` Global Variable~~ - FIXED

~~`delay_frames` was a file-scope `static int` causing cross-instance interference.~~
Multi-instance operation is now supported via the master-slave IPC architecture
(`SharedGroupMemory.h`). Per-instance state is correctly encapsulated.

### Thread Safety of Display Strings - MITIGATED

The audio thread writes diagnostic values (`d_ms`, `delay_ms`, etc.) and the GUI timer
reads them. The mitigation is a "diagnostic copy" pattern: the audio thread writes to
`aud_*` public fields in a ~100 ms diagnostic block (not per-sample), reducing the window
for torn reads. This is not a formally race-free design, but in practice the values are
`double`/`float`/`int` and incoherent reads produce only momentarily garbled display text.

### `delay_size` Tracking - IMPROVED

The v1.4 freeze-on-first-activation behaviour has been replaced by a rebuild-on-shift
mechanism (`rebuildThreshMs = 2 frames`): if `targetMs` drifts more than 2 frames from
`activeDelayMs`, the delay FIFOs are cleared and recommitted from the new target.

### `const_buf` Only for Channel 1 - UNCHANGED

The pre-fill historical buffer (`handle_const_delay`) is still only fed from channel 1.
Channel 2 delay uses the standard FIFO with an initial silence period. This asymmetry is
acceptable because in master-slave mode the master track always passes through undelayed.

### User Bits and Drop-Frame Flag Ignored - UNCHANGED

The decoder reads only HH:MM:SS:FF. Drop-frame timecode (29.97 fps), user bits, and flag
bits are not handled. The 29.97 fps rate is present in the `frates[]` table but is not
wired to the UI.

### User Bits and Drop-Frame Flag Ignored

The decoder reads only the timecode digits (hours, minutes, seconds, frames). It ignores:
- User bits (groups 1–8)
- Drop-frame flag (bit 10)
- Color-frame flag (bit 11)
- Biphase mark correction bit (bit 27)
- Binary group flags (bits 43, 58)

Drop-frame timecode (29.97 fps with frame-number skipping) is not handled. The 29.97 fps rate (`30000/1001`) is present in the `frates[]` table but not wired to the GUI.

### Reverse LTC Not Supported

The sync word check only matches the forward LTC sync word. Reverse-played LTC (common on tape machines in rewind) would present the bitstream in reverse and not be recognized.
