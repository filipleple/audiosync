# Audio Fallback Synchronisation Layer - Engineering Documentation

**Applies to:** `PluginProcessor.h/cpp` additions committed after base version 1.4
**Depends on:** `docs.md` (base LTC decoder and delay engine), `quality_scoring.md` (Q_LTC scoring)

This document covers the audio-domain lag estimator, the fusion policy that arbitrates between
LTC and the audio estimator, and the phase-2 delay engine changes that allow the fusion output
to steer compensation. It does not re-explain SMPTE LTC, the BMC decoder, `calc_delay()`, or
the Q_LTC windowed quality metrics - those are in the documents above.

---

## Table of Contents

1. [Motivation and Scope](#1-motivation-and-scope)
2. [System Architecture](#2-system-architecture)
3. [Short-Time Energy and the Novelty Function](#3-short-time-energy-and-the-novelty-function)
   - 3.1 [Hop-Based Energy Accumulation](#31-hop-based-energy-accumulation)
   - 3.2 [Novelty as Positive Energy Flux](#32-novelty-as-positive-energy-flux)
   - 3.3 [Why Novelty Rather Than Raw Energy](#33-why-novelty-rather-than-raw-energy)
   - 3.4 [Circular Buffer Organisation](#34-circular-buffer-organisation)
4. [Normalised Cross-Correlation for Lag Estimation](#4-normalised-cross-correlation-for-lag-estimation)
   - 4.1 [The Lag Estimation Problem](#41-the-lag-estimation-problem)
   - 4.2 [NCC Definition and Implementation](#42-ncc-definition-and-implementation)
   - 4.3 [Computational Cost](#43-computational-cost)
   - 4.4 [Sign Convention](#44-sign-convention)
5. [Confidence Scoring](#5-confidence-scoring)
   - 5.1 [Prominence](#51-prominence)
   - 5.2 [Stability](#52-stability)
   - 5.3 [Combined Confidence](#53-combined-confidence)
   - 5.4 [Validity Gate](#54-validity-gate)
5a. [HPF Preprocessing](#5a-hpf-preprocessing)
5b. [Activity Gate](#5b-activity-gate)
5c. [Anchored NCC Mode](#5c-anchored-ncc-mode)
5d. [Alpha-Beta Tracker](#5d-alpha-beta-tracker)
6. [Fusion Policy](#6-fusion-policy)
   - 6.1 [Source Selection Logic](#61-source-selection-logic)
   - 6.2 [Hold-on-Abstain Behaviour](#62-hold-on-abstain-behaviour)
   - 6.3 [Relation to Q_LTC and fallback_requested](#63-relation-to-q_ltc-and-fallback_requested)
7. [Phase-2 Delay Engine Integration](#7-phase-2-delay-engine-integration)
   - 7.1 [targetMs Derivation](#71-targetms-derivation)
   - 7.2 [Rebuild-on-Shift](#72-rebuild-on-shift)
   - 7.3 [activeDelayMs Lifecycle](#73-activedelayms-lifecycle)
8. [Data Structures](#8-data-structures)
   - 8.1 [AudioFallbackState](#81-audiofallbackstate)
   - 8.2 [FusionState](#82-fusionstate)
9. [Processing Loop Integration](#9-processing-loop-integration)
   - 9.1 [Call Sites in processBlock()](#91-call-sites-in-processblock)
   - 9.2 [Timing Budget](#92-timing-budget)
10. [Parameters and Tuning](#10-parameters-and-tuning)
11. [Diagnostic Outputs](#11-diagnostic-outputs)
12. [Fundamental Limitation: Signal Content Requirement](#12-fundamental-limitation-signal-content-requirement)
13. [Test Track Design](#13-test-track-design)

---

## 1. Motivation and Scope

The LTC decoder (documented in `docs.md`) decodes timecodes as long as the LTC signal is intact
and dominant. When the signal degrades - through noise, dropouts, level loss, or being mixed with
programme audio at similar amplitude - `Q_LTC` drops and `fallback_requested` is raised. At that
point there is no fallback: the plugin reports the last decoded offset or nothing.

The audio fallback addresses this by maintaining a second, independent lag estimate derived from
the programme audio content on both channels, without relying on any timecode structure. It uses
the observation that any shared acoustic content (speech, music, ambience) will produce correlated
amplitude envelopes across both channels, offset by the same propagation delay that separates the
two recording paths. Detecting this offset by cross-correlation of energy curves gives a direct
measurement of the inter-channel delay that is immune to LTC degradation.

The fallback is **estimator-only in phase 1**: it runs continuously in the background, logging
its estimate, but does not touch the delay engine. **Phase 2** allows the fusion policy to route
the fallback estimate to the delay engine when LTC fails.

---

## 2. System Architecture

```
Audio input (per sample, both channels)
         │
         ├─► handleTimecode()             LTC path
         │        │
         │        └─► Q_LTC, ltc_state, d_ms
         │                │
         │                └─► anchor update (anchorMs, anchorHops)
         │
         └─► HPF (100 Hz, 1-pole IIR)    Audio fallback path
                  │
                  │  per-hop (every 10 ms)
                  ▼
             energy accumulation
             noise-floor follower → activityGate (8 dB above floor)
             novelty[n] = max(0, E[n] - E[n-1])
                  │
                  │ [gate closed → skip NCC, alpha-beta coasts]
                  │ [gate open]
                  │  every 200 ms (20 hops)
                  ▼
             estimateAudioFallbackOffset()
               ├─ anchored mode (anchor fresh): ±NARROW_HALF=30 hops (±300 ms)
               │    K_eff staleness correction aligns master/slave rings
               └─ wide mode (cold-start):       ±lagRange=200 hops (±2 s)
                  │
                  ├─► bestLag, peakCorr, secondPeak
                  ├─► prominence, stableCount, stability
                  └─► confAud, deltaAudMs, valid
                  │
                  ▼
             fuseLtcAndAudioFallback()
                  │
                  ├─► source: LTC / AudioFallback / None
                  └─► selectedMs, selectedConf, fallbackActive
                  │
                  ▼
             AlphaBetaTracker (α=0.20, β=0.02, cap 0.20 ms/s)
                  │
                  └─► ab.estMs → targetMs → delay_size, activeDelayMs
```

---

## 3. Short-Time Energy and the Novelty Function

### 3.1 Hop-Based Energy Accumulation

Audio analysis runs at a hop rate of 10 ms (441 samples at 44100 Hz; adjusted for other sample
rates in `init()`). Each hop accumulates squared samples:

```
E[n] = (1/H) * Σ_{k=0}^{H-1} x[nH + k]²
```

where H = `hopSamples` and n is the hop index. This is the mean short-time power (energy per
sample) over the nth hop.

In code (`pushAudioAnalysisSample`):

```cpp
audFallback.energyAcc1 += ch1 * ch1;   // accumulate over H samples
...
float e1 = audFallback.energyAcc1 / (float)audFallback.hopSamples;
```

Both channels are accumulated in parallel, independently.

### 3.2 Novelty as Positive Energy Flux

The novelty function retains only positive frame-to-frame energy increases:

```
nov[n] = max(0,  E[n] - E[n-1])
```

This is a half-wave rectified first difference of the short-time energy. It is non-zero only
when the signal becomes louder - i.e., at onsets, transient attacks, and the rising edges of
speech phonemes.

```cpp
float nov1 = std::max(0.0f, e1 - audFallback.prevEnergy1);
```

### 3.3 Why Novelty Rather Than Raw Energy

**Raw energy** is correlated between channels but is dominated by the spectral envelope of the
dominant signal. For mixed LTC + programme audio, the LTC component contributes a near-constant
energy term (square wave at constant amplitude → constant power). This constant offset
contributes zero variance to the cross-correlation and would not bias the lag estimate, but it
reduces the effective dynamic range available for speech-driven variation.

More importantly, cross-correlating raw energy over a 2-second window on a 25 fps signal would
find strong spurious peaks at every multiple of the LTC frame period (40 ms at 25 fps), because
the LTC bit structure repeats periodically. The NCC would be unable to distinguish the true lag
from `true_lag ± k × 40 ms`.

**Novelty** breaks this periodicity. LTC has nearly constant power - its novelty is near zero.
Programme audio has irregular onsets - its novelty is sparse and aperiodic. The novelty function
therefore acts as a soft detector of acoustic events, producing a sparse signal whose
cross-correlation is dominated by shared acoustic content rather than the LTC waveform structure.

Mathematically, for a signal that is the sum of a constant-power component c(t) and a programme
component p(t):

```
E[n] = P[n] + C    where C = constant LTC power contribution
nov[n] = max(0, E[n] - E[n-1]) = max(0, P[n] - P[n-1])
```

The constant C cancels in the difference. The novelty is driven entirely by P.

### 3.4 Circular Buffer Organisation

Each channel maintains a circular novelty buffer of length `windowFrames`. The write pointer
`writePos` advances modulo `windowFrames` on each hop.

```
novelty1[writePos % W] = nov1
writePos = (writePos + 1) % W
```

`windowFrames` is set in `init()` to `MASTER_NOV_REF_SIZE = 2000` (20 seconds at 10 ms/hop)
so that the anchored NCC can reach deep into history (up to ±19.69 s) without resizing.
The NCC itself only reads a sub-window of this ring:

- **Anchored mode** (`anchorUsable == true`): `ANCHORED_WIN = 120` frames (1.2 s)
- **Wide mode** (cold-start): `WIDE_WIN = 400` frames (4 s)

The full 2000-frame ring is held to support deep anchor offsets; only the sub-window is
linearised for each NCC run.

The buffer is considered ready once `framesFilled >= ANCHORED_WIN` (anchored) or
`framesFilled >= WIDE_WIN` (wide). `framesFilled` is clamped at `windowFrames`.

Pre-allocated linearisation scratch buffers `linBuf1`, `linBuf2` (size `windowFrames`) avoid
heap allocation inside the audio thread at estimation time.

---

## 4. Normalised Cross-Correlation for Lag Estimation

### 4.1 The Lag Estimation Problem

Let `n1[k]` and `n2[k]` be the novelty curves for channels 1 and 2 respectively, indexed over
the 2-second analysis window (k = 0..199). If channel 2's content is delayed relative to
channel 1 by τ hops:

```
n2[k] ≈ n1[k + τ]    (for shared content)
```

The lag τ that maximises the cross-correlation of n1 and n2 is the inter-channel delay estimate.
τ is converted to milliseconds by `δt_AUD = τ × hop_ms`.

### 4.2 NCC Definition and Implementation

The Normalised Cross-Correlation at lag τ is:

```
                  Σ_{k} (n1[k] - μ₁)(n2[k + τ] - μ₂)
NCC(τ) = ─────────────────────────────────────────────────
             |{k : 0 ≤ k < W, 0 ≤ k+τ < W}| · σ₁ · σ₂
```

where:
- μ₁, μ₂ are the sample means of n1, n2 over the window
- σ₁, σ₂ are the sample standard deviations
- The denominator normalises by the number of valid (non-boundary) sample pairs

Normalisation by σ₁σ₂ confines NCC(τ) to [-1, 1] and makes the result independent of the
absolute energy level of the programme material. This is important because the two channels may
have different recording gains.

The search range depends on mode:

- **Wide mode** (cold-start, no fresh anchor): `lagRange = 200` hops = ±2 seconds.
- **Anchored mode** (anchor fresh and K_eff in range): `NARROW_HALF = 30` hops = ±300 ms,
  centred on the K_eff-corrected anchor offset.

The best lag is:

```
τ* = argmax_{τ ∈ [-L, L]} NCC(τ)
```

Implementation (`estimateAudioFallbackOffset`):

```cpp
// Linearise circular buffers first
for (int i = 0; i < N; ++i)
{
    int src = (audFallback.writePos + i) % N;
    audFallback.linBuf1[i] = audFallback.novelty1[src];
    audFallback.linBuf2[i] = audFallback.novelty2[src];
}

// Means and standard deviations
float m1 = 0, m2 = 0;
for (int i = 0; i < N; ++i) { m1 += linBuf1[i]; m2 += linBuf2[i]; }
m1 /= N;  m2 /= N;

float s1 = 0, s2 = 0;
for (int i = 0; i < N; ++i)
{
    s1 += (linBuf1[i]-m1)*(linBuf1[i]-m1);
    s2 += (linBuf2[i]-m2)*(linBuf2[i]-m2);
}
s1 = sqrt(s1/N);  s2 = sqrt(s2/N);

// NCC search
for (int lag = -L; lag <= L; ++lag)
{
    double sum = 0;  int cnt = 0;
    for (int i = 0; i < N; ++i)
    {
        int j = i + lag;
        if (j >= 0 && j < N)
        { sum += (linBuf1[i]-m1) * (linBuf2[j]-m2);  ++cnt; }
    }
    double corr = cnt > 0 ? sum / (cnt * s1 * s2) : 0.0;
    // track bestCorr and secondBest ...
}
```

If σ₁ < 1e-6 or σ₂ < 1e-6 (one channel has effectively constant novelty - no detectable
events), the estimator exits without updating: `valid = false`, `stableCount = 0`.

### 4.3 Computational Cost

The NCC inner loop cost depends on the active mode:

| Mode | Lags | Window frames | MACs per NCC run |
|---|---|---|---|
| Anchored | 2×30+1 = 61 | 120 | 61 × 120 = 7,320 |
| Wide | 2×200+1 = 401 | 400 | 401 × 400 = 160,400 |

Both run once every `refreshEvery = 20` hops = 200 ms. Worst-case (wide mode):
```
160,400 / 0.200 s ≈ 802,000 MAC/s
```
At 44100 Hz the audio thread budget is approximately 44,100 MACs/sample. Wide mode uses
under 2% of the budget; anchored mode under 0.1%.

No dynamic allocation occurs at estimation time. All buffers are allocated in `init()`.

The anchored mode (the common case once a valid LTC lock has been established) is
significantly cheaper: ~22× fewer MACs than wide mode.

No dynamic allocation occurs at estimation time. The only heap-allocated structures
(`novelty1`, `novelty2`, `linBuf1`, `linBuf2`) are allocated during `init()` which is called
from `prepareToPlay()`, never from the audio callback.

### 4.4 Sign Convention

The NCC is computed as `Σ n1[i] · n2[i + lag]`. If channel 2 is τ hops **ahead** of channel
1:

```
n2[k] ≈ n1[k + τ]    →    NCC peaks at lag = -τ
```

A positive τ (CH2 leading CH1) produces `bestLag < 0`. This is the opposite sign from the
LTC convention used throughout the plugin, where `d_ms > 0` when CH2 leads CH1.

The conversion applied in `estimateAudioFallbackOffset` is therefore:

```cpp
audFallback.deltaAudMs = -(double)bestLag * audFallback.hopMs;
```

This ensures `deltaAudMs` is directly comparable to `d_ms` from `calc_delay()`.

---

## 5. Confidence Scoring

The NCC peak value alone is not a reliable confidence indicator. A strong-but-narrow peak in a
featureless correlation function is more trustworthy than a broad, diffuse maximum. Two
complementary terms address different failure modes.

### 5.1 Prominence

Prominence measures how much the best peak exceeds the rest of the correlation function. Let
`r₁ = bestCorr` and `r₂ = secondBest` (the highest NCC value at any lag other than τ*):

```
prominence = (r₁ - max(0, r₂)) / (r₁ + ε)
```

Range: [0, 1].

- **Prominence → 1:** a single sharp, isolated peak - the correct lag is unambiguous.
- **Prominence → 0:** the best lag is barely above the runner-up - either the content is too
  repetitive (e.g., a regular metronome at a period close to the lag), or the signal is too
  weak to form any coherent peak.

The `max(0, r₂)` term handles the case where the second-best is negative - a trivially
prominent peak that should not be penalised for having negative second-best.

### 5.2 Stability

Stability measures temporal consistency of the estimate. `prevBestLag` stores the `bestLag`
from the previous estimation window (200 ms ago). If the current `bestLag` agrees within ±2
hops (±20 ms):

```
stable = (|bestLag - prevBestLag| ≤ 2)
```

A counter `stableCount` increments on agreement and resets to 0 on disagreement:

```cpp
if (stable) ++stableCount;
else        stableCount = 0;

stability = min(1.0, stableCount / 3.0)
```

`stability` reaches 1.0 after 3 consecutive consistent estimates (600 ms), and drops
immediately back to 0 on any instability event.

**Rationale:** a single strong NCC peak could be a statistical artefact in a brief burst of
correlated noise. Requiring consistent agreement across multiple 200 ms windows ensures the
estimate reflects a stable underlying offset rather than a transient coincidence.

### 5.3 Combined Confidence

```
conf_AUD = clamp(0.6 × prominence + 0.4 × stability, 0, 1)
```

The 60/40 split weights shape quality slightly higher than temporal consistency, because a
single unambiguous peak in the NCC is more informative than three ambiguous ones that happened
to agree. In practice, the two terms are correlated: a signal with strong, consistent content
produces both high prominence (sharp peak) and high stability (repeatable lag).

### 5.4 Validity Gate

`valid` is set true when:

```
stableCount ≥ 3   AND   conf_AUD > 0.3
```

This requires at least 600 ms of consistent evidence before the fallback is considered
trustworthy. The fusion uses `valid` as a precondition and separately applies `conf_AUD > 0.4`
as a threshold for switching to the fallback source.

---

## 5a. HPF Preprocessing

Before energy accumulation, each channel sample passes through a 1-pole IIR high-pass filter
with a 100 Hz cutoff:

```cpp
float applyHPF(float x, float& prevX, float& prevY) const noexcept
{
    float y = hpfAlpha * (prevY + x - prevX);
    prevX = x; prevY = y;
    return y;
}
```

The coefficient `hpfAlpha = 1 / (1 + 2π · 100 / sampleRate)` is computed once in `init()`.
At 48 kHz: `hpfAlpha ≈ 0.9872`.

**Purpose:** The LTC carrier contributes a near-constant energy level below ~2.4 kHz. Without
HPF, the noise floor estimate is inflated by the carrier, and the activity gate opens later
than necessary once the LTC stops. The 100 Hz pole removes DC and sub-bass content without
affecting the speech/transient band used by the novelty estimator.

The HPF does not band-limit the upper end - wind, HVAC artefacts, and codec noise above
3.5 kHz are still included. A low-pass stage would improve selectivity but is not yet
implemented (see `what_was_missing.md`).

---

## 5b. Activity Gate

The activity gate suppresses NCC computation during silence, very-low-level content, or
steady-state noise (e.g. the LTC carrier alone). Without it, the NCC would run on flat
novelty curves and produce random lag estimates that poison the stability counter.

### Noise Floor Follower

Each channel maintains an asymmetric exponential follower on the HPF-filtered energy `e`:

```cpp
const float tc = (e < noiseFloor) ? NOISE_FLOOR_TC_FALL : NOISE_FLOOR_TC_RISE;
noiseFloor = tc * noiseFloor + (1 - tc) * e;
```

| Constant | Value | Time constant at 10 ms/hop |
|---|---|---|
| `NOISE_FLOOR_TC_RISE` | 0.995 | ~2 s |
| `NOISE_FLOOR_TC_FALL` | 0.80 | ~45 ms |

**Rationale for asymmetry:** The rise time constant is slow so the floor doesn't flutter
up during brief loud events. The fall time constant is fast (≈ 45 ms) so that when the
LTC carrier stops, the noise floor drops quickly to match the quieter programme audio,
allowing the gate to open within ~150–250 ms rather than waiting seconds.

### Gate Decision

At each hop boundary, `activityGate` is set:

```cpp
const float gate_thresh = noiseFloor * std::pow(10.0f, ACTIVITY_GATE_DB / 10.0f);
activityGate = (e1 > gate_thresh || e2 > gate_thresh);
```

`ACTIVITY_GATE_DB = 8.0 dB`. If neither channel exceeds the floor by 8 dB, the NCC is
skipped and the alpha-beta tracker coasts on its current velocity estimate.

---

## 5c. Anchored NCC Mode

When a valid LTC anchor exists and is recent enough, the NCC search is constrained to
a narrow window centred on the anchor. This reduces search cost by ~22× and prevents
the estimator from locking onto spurious lags far from the expected offset.

### Anchor Lifecycle

- **Set:** whenever `d_ms` is committed by the LTC decoder (including the hysteresis
  guard, so only confirmed values set the anchor).
- **Age tracking:** `anchorAgeMs` increments every processBlock call. If it exceeds
  `ANCHOR_MAX_AGE_MS = 30,000 ms`, the anchor is considered stale.
- **Cleared:** on `prepareToPlay()` / `reset()`.

### K_eff Staleness Correction (Slave Mode)

The master writes its novelty ring to shared memory approximately every 100 ms. The slave
reads it on its own independent 100 ms timer. This introduces up to ~200 ms of relative
misalignment between the master and slave novelty time axes.

`K_eff` compensates by pre-shifting the master reference before the NCC:

```
K_eff = anchorHops - timeDeltaHops
```

where `timeDeltaHops = round((slaveSampleCount - masterSampleCount) / hopSamples)`.

- `K_eff > 0`: slave is late relative to master → shift master's past forward.
- `K_eff < 0`: slave is ahead → shift slave's past backward.

Exactly one of `masterShift` or `slaveShift` is non-zero per NCC run.

### Mode Selection

```
anchorAgeOk  = (anchorAgeMs < ANCHOR_MAX_AGE_MS)
anchorUsable = anchorAgeOk
            && hasMasterRef
            && |K_eff| < windowFrames - NARROW_HALF - 1
            && masterFramesFilled >= ANCHORED_WIN + masterShift
            && framesFilled       >= ANCHORED_WIN + slaveShift

if (anchorAgeOk && !anchorUsable):
    // Anchor fresh but history too short → hold anchor, confAud = 0.8
    deltaAudMs = anchorMs
    return

nccN    = anchorUsable ? ANCHORED_WIN : WIDE_WIN
lagHalf = anchorUsable ? NARROW_HALF  : lagRange
```

When the anchor is age-ok but the history buffer isn't deep enough yet (e.g., right after
startup), the estimator returns the anchor value directly with `confAud = 0.8` rather than
running a potentially bogus wide NCC.

---

## 5d. Alpha-Beta Tracker

The raw NCC estimate (`deltaAudMs`) is not fed directly to the delay engine. It passes
through an alpha-beta tracker that smooths position noise and enforces a velocity cap.

### State and Parameters

```cpp
struct AlphaBetaTracker {
    static constexpr double ALPHA            = 0.20;
    static constexpr double BETA             = 0.02;
    static constexpr double MAX_VEL_MS_PER_S = 0.20;

    double estMs    = 0.0;   // smoothed position estimate
    double velMsPerS = 0.0;  // velocity estimate
    bool   initialized = false;
};
```

### Update Equation

Called once per NCC refresh cycle (~200 ms), with `dtS ≈ 0.200`:

```
residual  = measured - estMs
estMs    += ALPHA * residual
velMsPerS = clamp(velMsPerS + BETA * residual / dtS,
                  -MAX_VEL_MS_PER_S, +MAX_VEL_MS_PER_S)
```

When `feedMeasured = false` (gate closed or fusion in LTC mode), the update coasts:

```
estMs += velMsPerS * dtS
```

### Seeding

`ab.seed(value)` sets `estMs = value`, `velMsPerS = 0`, `initialized = true`. It is called:

1. At the first LTC→fallback transition (seeds from last good `d_ms`).
2. When the NCC estimate jumps more than 150 ms from the tracker's current `estMs`
   (large-jump fast path - handles manual track shifts in the DAW).

The 150 ms threshold is above normal NCC jitter (±10 ms one hop) but below a typical
manual re-sync operation, so spurious NCC outliers are absorbed by the tracker rather
than triggering a re-seed.

---

## 6. Fusion Policy

`fuseLtcAndAudioFallback()` runs every 200 ms, triggered from within
`pushAudioAnalysisSample()` at each refresh boundary. It reads the current LTC quality state
and the audio fallback estimate and writes to the `FusionState` struct.

### 6.1 Source Selection Logic

```
if (CH1 VALID  AND  CH2 VALID  AND  not drift_suspected):
    source = LTC
    selectedMs = d_ms                    (current live LTC offset)
    selectedConf = min(Q_LTC_ch1, Q_LTC_ch2)

else if (audFallback.valid  AND  audFallback.confAud > 0.4):
    source = AudioFallback
    selectedMs = audFallback.deltaAudMs
    selectedConf = audFallback.confAud

else:
    source = None
    selectedMs = 0
    selectedConf = 0
```

The LTC branch uses `ltc_state == VALID` directly, not the `fallback_requested` flag. The flag
fires only at FAIL; the fusion switches away from LTC also when either channel is SUSPECT (Q_LTC
≤ 0.8), because a degraded-but-not-failed LTC reading combined with a confident audio estimate
is better served by the audio estimate.

`drift_suspected` (defined in `quality_scoring.md`) is also checked: a linearly drifting Δt
indicates inconsistent timecode sources, making the LTC value untrustworthy regardless of its
per-channel Q score.

### 6.2 Hold-on-Abstain Behaviour

When the fusion abstains (`source = None`), the delay engine is not zeroed. This is handled in
the `targetMs` computation in `processBlock()` (see section 7.1). The intent is to avoid a
rebuild transient: if LTC has just failed and the audio fallback has not yet reached `valid`,
holding the last committed delay is better than cutting to zero. The GUI clearly shows
`src=NONE` so the operator is aware that the output is coasting on a stale value.

### 6.3 Relation to Q_LTC and fallback_requested

`quality_scoring.md` defines:
- `fallback_requested` (per-channel): set when `ltc_state == FAIL` only
- `fallback_requested` (processor-level): either channel FAIL, or `drift_suspected`

The fusion is a **superset** of these signals:
- It fires when either channel is SUSPECT (Q ≤ 0.8), not only FAIL
- It provides a concrete alternative offset, not just a binary flag
- It distinguishes "LTC degraded, audio fallback ready" from "nothing available"

The processor-level `fallback_requested` flag is retained for downstream systems that only
need a boolean "something is wrong" signal. The fusion state is the richer output.

---

## 7. Phase-2 Delay Engine Integration

Prior to phase 2, the delay engine was driven exclusively by `d_ms` from `calc_delay()`, and
`delay_size` (the programmed delay in samples) was set once on first activation and never
updated. Phase 2 introduces `targetMs` and `activeDelayMs` to make the delay engine track the
fusion output.

### 7.1 targetMs Derivation

At each sample in the processing loop, `targetMs` is resolved as follows:

```
if   fusion.source == LTC:
     targetMs = d_ms                 (always-current LTC offset from calc_delay)

elif fusion.source == AudioFallback:
     targetMs = fusion.selectedMs    (last NCC estimate, updated every 200 ms)

elif activeDelayMs != 0:
     targetMs = activeDelayMs        (hold last committed value - see §7.2)

else:
     targetMs = d_ms                 (pre-fusion fallback: raw LTC, initial startup)
```

The LTC branch uses the live `d_ms` rather than the fusion snapshot of `d_ms` (which is 200 ms
old) to preserve the frame-accurate, sub-millisecond resolution that the LTC path provides when
it is healthy.

### 7.2 Rebuild-on-Shift

`delay_size` (the number of samples held in the delay FIFO) is programmed once per "epoch"
(activation or rebuild). It does not update sample-by-sample. When the fusion target shifts by
more than 2 frames, the epoch ends and a rebuild is triggered:

```
rebuildThreshMs = 2000 / fps          (= 80 ms at 25 fps, 66.7 ms at 30 fps)

if  active_delay
AND activeDelayMs != 0
AND targetMs != 0
AND |targetMs - activeDelayMs| > rebuildThreshMs:
    chnl1.clear()
    chnl2.clear()
    activeDelayMs = 0
```

`chnl1.clear()` and `chnl2.clear()` reset the delay FIFOs and all decoder state (see
`docs.md §3`). On the next sample where `targetMs != 0`, `delay_size` is recommitted from the
new `targetMs` and `activeDelayMs` is updated.

**Why 2 frames?** The audio fallback has a 10 ms hop resolution. Normal NCC noise produces
lag estimates that vary by ±1 hop (±10 ms) between consecutive windows, well below the 2-frame
threshold. A genuine source switch (LTC → audio fallback reporting a different offset) would
produce a difference of at least 2 frames if the two estimates disagree significantly. The
threshold prevents rebuilds from NCC jitter while catching real offset corrections.

**Rebuild transient:** `chnl2.clear()` resets `chnl2.delay_buf`, causing brief silence (zero
output) on channel 2 while the FIFO fills to the new target size. `chnl1` is served from
`const_buf` (a 500,000-sample historical ring buffer), so channel 1 is not silenced. For
typical lag values (tens to hundreds of milliseconds), the channel 2 silence lasts the same
duration as the target delay.

### 7.3 activeDelayMs Lifecycle

| Event | Effect on activeDelayMs |
|---|---|
| `delay_size` committed on first activation | Set to `targetMs` |
| `delay_size` committed after rebuild | Set to `targetMs` |
| `active_delay` button turned off | Set to 0 |
| LTC jump resets delay (`|prev_frames - delay_frames| > 1`) | Set to 0 |
| Fusion target exceeds rebuild threshold | Set to 0, then recommitted |

`activeDelayMs` is the single ground truth for what is currently programmed into the hardware
delay engine. It is copied to `aud_activeDelayMs` in the 0.1 s diagnostic update for display
in the GUI.

---

## 8. Data Structures

### 8.1 AudioFallbackState

Declared in `PluginProcessor.h` as a plain struct before `AutoSyncAudioProcessor`.

**Parameters (set by `init(double sampleRate)`, not const):**

| Field | Type | Value (44100 Hz) | Description |
|---|---|---|---|
| `hopSamples` | `int` | 441 | Samples per energy accumulation hop (10 ms) |
| `windowFrames` | `int` | 2000 | Full novelty ring length (20 s; = MASTER_NOV_REF_SIZE) |
| `lagRange` | `int` | 200 | Wide NCC search range in hops (±2 s) |
| `refreshEvery` | `int` | 20 | Hops between NCC re-computations (200 ms) |
| `hopMs` | `double` | 10.0 | Hop duration in ms (used for lag→ms conversion) |
| `hpfAlpha` | `float` | 0.9872 @ 48 kHz | 1-pole HPF coefficient; cutoff 100 Hz; computed in `init()` |

**Constants (compile-time, not changed by `init()`):**

| Constant | Value | Description |
|---|---|---|
| `NARROW_HALF` | 30 hops | Half-width of anchored NCC search (±300 ms) |
| `ANCHORED_WIN` | 120 frames | NCC window used in anchored mode (1.2 s) |
| `WIDE_WIN` | 400 frames | NCC window used in wide/cold-start mode (4 s) |
| `NOISE_FLOOR_TC_RISE` | 0.995 | Noise-floor follower slow-rise time constant (~2 s) |
| `NOISE_FLOOR_TC_FALL` | 0.80 | Noise-floor follower fast-fall time constant (~150 ms) |
| `ACTIVITY_GATE_DB` | 8.0 dB | Signal must exceed noise floor by this to open gate |

**Per-hop accumulators (reset at each hop boundary):**

| Field | Type | Description |
|---|---|---|
| `energyAcc1/2` | `float` | Sum of HPF-filtered x² over current hop, both channels |
| `prevEnergy1/2` | `float` | Mean energy of previous hop (for novelty difference) |
| `hopSampleCounter` | `int` | Samples elapsed in current hop |
| `hopsSinceRefresh` | `int` | Hops elapsed since last NCC computation |

**HPF state:**

| Field | Type | Description |
|---|---|---|
| `hpfPrevX1/2` | `float` | Previous input sample for per-channel 1-pole HPF |
| `hpfPrevY1/2` | `float` | Previous output sample for per-channel 1-pole HPF |

**Noise floor and activity gate:**

| Field | Type | Description |
|---|---|---|
| `noiseFloor1/2` | `float` | Exponentially tracked per-channel noise energy floor |
| `activityGate` | `bool` | True when either channel exceeds ACTIVITY_GATE_DB above floor |

**Novelty buffers:**

| Field | Type | Description |
|---|---|---|
| `novelty1/2` | `vector<float>` | Circular novelty buffers, length `windowFrames` (2000) |
| `linBuf1/2` | `vector<float>` | Pre-allocated scratch for NCC linearisation |
| `writePos` | `int` | Circular buffer write pointer |
| `framesFilled` | `int` | Valid hops accumulated; clamped at `windowFrames` |

**Master reference (slave mode only):**

| Field | Type | Description |
|---|---|---|
| `masterNoveltyRef` | `vector<float>` | Linearised master novelty copied from shared memory every ~100 ms |
| `hasMasterRef` | `bool` | True once a valid master novelty snapshot has been received |
| `masterFramesFilled` | `int` | Number of valid frames in the master reference snapshot |

**Anchor state:**

| Field | Type | Description |
|---|---|---|
| `hasAnchor` | `bool` | True when at least one LTC-confirmed anchor exists |
| `anchorMs` | `double` | Last LTC-confirmed offset used to centre the anchored NCC search |
| `anchorHops` | `int` | `round(anchorMs / hopMs)`, cached to avoid per-call division |
| `lastEstimateAnchored` | `bool` | Set by `estimateAudioFallbackOffset`; read by `fuseLtcAndAudioFallback` |

**Estimation results (written by `estimateAudioFallbackOffset`):**

| Field | Type | Description |
|---|---|---|
| `deltaAudMs` | `double` | Estimated inter-channel lag in ms, LTC sign convention |
| `confAud` | `double` | Combined confidence score [0, 1] |
| `peakCorr` | `double` | NCC value at best lag (for diagnostics) |
| `secondPeak` | `double` | NCC value at second-best lag (for diagnostics) |
| `bestLag` | `int` | Best lag in hops (signed) |
| `prevBestLag` | `int` | Best lag from previous window (INT_MAX = uninitialised) |
| `stableCount` | `int` | Consecutive windows with consistent bestLag |
| `valid` | `bool` | True when stableCount ≥ 3 and confAud > 0.3 |

### 8.2 FusionState

| Field | Type | Description |
|---|---|---|
| `source` | `enum Source` | `None`, `LTC`, or `AudioFallback` |
| `selectedMs` | `double` | Offset selected by fusion policy (ms) |
| `selectedConf` | `double` | Confidence of selected source |
| `fallbackActive` | `bool` | True when `source == AudioFallback` |

`FusionState` is an audio-thread-only struct. The GUI reads `aud_fusionSource` (an `int` copy
updated every 0.1 s in the diagnostic block), not `fusion.source` directly.

---

## 9. Processing Loop Integration

### 9.1 Call Sites in processBlock()

```
per sample:
    handle_const_delay(write1[i], chnl1)           // feed historical buffer (CH1)
    processTimeCode(ltcSample, chnl1_in, ...)      // LTC decode on LTC channel
      → Q_LTC update, anchor refresh (anchorMs, anchorHops)
    pushAudioAnalysisSample(ltcSample, sceneSample)
      // ltcSample  = LTC-carrying channel (raw, undelayed)
      // sceneSample = non-LTC channel (scene audio if available, else ~silence)
      // internally: HPF applied to both channels before energy accumulation
      // internally: noise-floor follower → activityGate update
    d_ms = calc_delay(chnl1_in, chnl2_in, fps)    // LTC offset (hysteresis guarded)
    [LTC jump detection + d_ms_pending hysteresis]
    targetMs = ab.estMs (or d_ms if ab not yet seeded) // alpha-beta smoothed
    [rebuild check: |targetMs - activeDelayMs| > 2 frames → clear FIFOs]
    delay(write2, i, chnl2) or delay(write1, .) // delay engine uses targetMs

every ~200 ms (inside pushAudioAnalysisSample, at hop boundary):
    [if activityGate closed → return, alpha-beta coasts]
    estimateAudioFallbackOffset()
      // anchored mode if anchorUsable, else wide mode
      // K_eff corrects for SM read-staleness in slave mode
    fuseLtcAndAudioFallback()
    if (source == AudioFallback && large jump > 150 ms): ab.seed(deltaAudMs)
    ab.update(deltaAudMs, feedMeasured)            // alpha-beta step

every ~0.1 s (diagnostic block):
    copy fusion fields to public aud_* members
    write master novelty snapshot to shared memory (master mode)
    read master novelty snapshot from shared memory (slave mode)
```

`pushAudioAnalysisSample` must be called **before** `delay()` modifies the write pointers.
The fallback operates on raw, undelayed input samples - the K_eff anchor mechanism in
`estimateAudioFallbackOffset` accounts for the inter-track time offset mathematically
rather than by buffering delayed audio.

### 9.2 Timing Budget

| Operation | Frequency | Cost |
|---|---|---|
| Energy accumulation | Every sample | 2 multiplies + 2 adds |
| Hop boundary bookkeeping | Every 441 samples | ~10 operations |
| NCC computation | Every 8820 samples (~200 ms) | ~60,200 MACs |
| Fusion policy | Every 8820 samples | ~10 comparisons |
| Diagnostic copy | Every 4410 samples (~100 ms) | ~15 field copies |

The dominant cost is the NCC, which runs at most 5 times per second. At a typical DAW block
size of 512 samples, the NCC fires approximately once every 17 blocks. Within those 17 blocks
it adds about 60,200/17 ≈ 3,540 MACs per block - negligible relative to the LTC decoder's
per-sample processing.

---

## 10. Parameters and Tuning

| Parameter | Location | Default | Effect |
|---|---|---|---|
| `hopMs` / `hopSamples` | `AudioFallbackState::init()` | 10 ms | Lag resolution and novelty time granularity. Smaller = finer lag grid but more NCC lags. |
| `windowFrames` | `AudioFallbackState::init()` | 2000 (20 s ring) | Full novelty ring; NCC sub-windows are ANCHORED_WIN=120 or WIDE_WIN=400. |
| `lagRange` | `AudioFallbackState::init()` | 200 (±2 s wide) | Wide-mode maximum detectable delay. Anchored mode uses NARROW_HALF=30 (±300 ms). |
| `NARROW_HALF` | compile-time constant | 30 hops (±300 ms) | Half-width of anchored NCC search. Must be < ANCHORED_WIN/2. |
| `ANCHORED_WIN` | compile-time constant | 120 frames (1.2 s) | NCC window in anchored mode. Controls how quickly the estimator reacts to a manual track shift. |
| `WIDE_WIN` | compile-time constant | 400 frames (4 s) | NCC window in wide mode. |
| `ANCHOR_MAX_AGE_MS` | compile-time constant | 30,000 ms | How long an anchor remains valid without LTC confirmation. Beyond this the estimator falls back to wide mode. |
| `refreshEvery` | `AudioFallbackState::init()` | 20 (200 ms) | NCC re-computation interval. Lower = faster tracking, higher CPU. |
| `ACTIVITY_GATE_DB` | compile-time constant | 8 dB | Signal must exceed tracked noise floor by this margin for the NCC to run. |
| `ALPHA` | `AlphaBetaTracker` | 0.20 | Alpha-beta position gain. Higher = faster response but more jitter. |
| `BETA` | `AlphaBetaTracker` | 0.02 | Alpha-beta velocity gain. Higher = faster velocity adaptation. |
| `MAX_VEL_MS_PER_S` | `AlphaBetaTracker` | 0.20 ms/s | Velocity cap; prevents tracker from following moving acoustic sources. |
| conf_AUD threshold | `fuseLtcAndAudioFallback()` | 0.4 | Minimum confidence to switch from LTC to audio fallback. |
| `stableCount` gate | `estimateAudioFallbackOffset()` | ≥ 3 | Consecutive consistent estimates required before `valid = true` (600 ms minimum). |
| stability agreement | `estimateAudioFallbackOffset()` | ±2 hops | Agreement window for stability counting. Covers ±10 ms NCC grid plus one hop of noise. |
| rebuild threshold | `processBlock()` | 2 frames | `|targetMs - activeDelayMs|` must exceed this to trigger a delay engine rebuild. |
| drift threshold | diagnostic block | 5.0 ms/s × 3 windows | Inherited from Q_LTC layer; see `quality_scoring.md` for derivation. |

---

## 11. Diagnostic Outputs

The following public fields are written by the audio thread in the 0.1 s diagnostic block and
read by the GUI timer (existing pattern; see `quality_scoring.md §13` for the thread-safety
rationale):

| Field | Type | Content |
|---|---|---|
| `aud_deltaMs` | `double` | `audFallback.deltaAudMs` - last NCC lag estimate in ms |
| `aud_conf` | `double` | `audFallback.confAud` - combined confidence [0, 1] |
| `aud_fusionSource` | `int` | `(int)fusion.source` - 0=None, 1=LTC, 2=AudioFallback |
| `aud_activeDelayMs` | `double` | `activeDelayMs` - offset currently in the delay engine |

The GUI label `qual_fallback_label` formats these as:

```
AUD: dt=Xms  conf=Y.YY  src=SRC  applied=Zms
```

Example readings:

```
AUD: dt=80ms   conf=0.85  src=LTC   applied=80ms    <- LTC healthy, audio confirms
AUD: dt=80ms   conf=0.78  src=AUD   applied=80ms    <- LTC failed, fallback active
AUD: dt=---    conf=0.00  src=NONE  applied=80ms    <- both failed, holding last value
AUD: dt=80ms   conf=0.12  src=NONE  applied=80ms    <- audio has peak but not stable yet
```

The `applied` field is the most operationally significant: it shows what the delay engine is
actually doing, independent of which source is currently providing evidence.

---

## 12. Fundamental Limitation: Signal Content Requirement

The audio fallback requires **shared acoustic content** to produce a reliable lag estimate.
The NCC of novelty curves finds a peak only if both channels respond to the same acoustic
events. Three scenarios affect this:

**LTC-only channels:** LTC has near-constant power; its novelty is near zero. If both channels
carry only LTC, both novelty curves are flat and σ₁ or σ₂ will be below the 1e-6 guard,
causing the estimator to exit without an estimate. The fallback will never produce `valid = true`
in this case.

**Mixed LTC + programme audio:** The novelty is driven by the programme audio component (the
LTC contribution cancels in the energy difference). The fallback will work if the programme audio
has sufficient dynamic variation (speech, music with transients). It will not work on sustained
tones, fade-outs, or silence.

**Programme audio only (LTC absent):** Best case for the fallback. The novelty is driven
entirely by the programme content, and the NCC will find the lag reliably given enough acoustic
events. This is the target operating condition for the fallback.

**Practical implication for test tracks:** The recommended test design is `08_audio_ltc_fading.wav`:
LTC dominant for the first 30 seconds (audio fallback accumulates evidence), sharp LTC cut at
30 seconds (fallback takes over). A gradual LTC fade creates an intermediate zone where LTC is
too degraded to decode (speech cancels transitions at the hysteresis threshold) but the audio
fallback has not yet accumulated a full window - both paths fail simultaneously.

---

## 13. Test Track Design

`gen_audio_ltc_track.py` generates `test_tracks/08_audio_ltc_fading.wav` for validation:

| Time | LTC amp | Speech amp | Expected plugin state |
|---|---|---|---|
| 0–10 s | 0.70 | 0.20 | VALID, src=LTC, fallback accumulating |
| 10–20 s | 0.70 | 0.20 | VALID, src=LTC, fallback window full |
| 20–30 s | 0.70 | 0.20 | VALID, src=LTC, conf_AUD should reach > 0.4 |
| 30–40 s | 0.00 | 0.20 | FAIL, src=NONE → AUD, convergence |
| 40–50 s | 0.00 | 0.20 | FAIL, src=AUD, applied=+80ms |
| 50–60 s | 0.00 | 0.20 | FAIL, src=AUD, applied=+80ms (stable) |

The LTC amplitude of 0.70 with speech at 0.20 gives a 3.5× amplitude ratio (≈ 11 dB). At this
ratio, the LTC transitions at ±0.70 cannot be cancelled by speech peaks at ±0.20 - the
combined signal in the LTC-positive half-cycle ranges from 0.50 to 0.90, always above the
threshold at 0.8 × 0.70 = 0.56. The decoder is reliable throughout the first 30 seconds.

The sharp cut at 30 s (LTC amp drops to 0.00 within one segment boundary) is preferred over a
gradual fade because a fade creates a "danger zone" where LTC is too degraded to decode but the
audio fallback hasn't yet seen 600 ms of consistent evidence. Both estimators fail simultaneously
and the plugin has nothing to offer. A sharp cut ensures the audio fallback has a full, clean
window from the LTC-healthy period to work from immediately after the cut.

The expected convergence time after the cut is approximately:
- `stableCount` requires 3 × 200 ms = 600 ms of consistent estimates
- First estimate runs 200 ms after the cut
- Therefore `valid = true` and `src=AUD` by approximately t = 30.8 s in the best case

In practice, the confidence may take 1–2 additional windows to cross the 0.4 threshold if the
speech content in that window is sparse. The `applied` field should match `+80ms` throughout
the LTC period and remain at `+80ms` during the NONE phase due to hold-on-abstain, then be
confirmed by `src=AUD` once the fallback converges.
