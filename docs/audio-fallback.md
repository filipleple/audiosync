# Audio Fallback Synchronisation Layer - Engineering Documentation

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
6. [Band-Pass Preprocessing](#6-band-pass-preprocessing)
7. [Activity Gate](#7-activity-gate)
8. [Anchored NCC Mode](#8-anchored-ncc-mode)
9. [Alpha-Beta Tracker](#9-alpha-beta-tracker)
10. [Fusion Policy](#10-fusion-policy)
    - 10.1 [Source Selection Logic](#101-source-selection-logic)
    - 10.2 [Hold-on-Abstain Behaviour](#102-hold-on-abstain-behaviour)
    - 10.3 [Relation to Q_LTC and fallback_requested](#103-relation-to-q_ltc-and-fallback_requested)
11. [Delay Engine Integration](#11-delay-engine-integration)
    - 11.1 [targetMs Derivation](#111-targetms-derivation)
    - 11.2 [Rebuild-on-Shift](#112-rebuild-on-shift)
    - 11.3 [activeDelayMs Lifecycle](#113-activedelayms-lifecycle)
12. [Data Structures](#12-data-structures)
    - 12.1 [AudioFallbackState](#121-audiofallbackstate)
    - 12.2 [FusionState](#122-fusionstate)
    - 12.3 [AlphaBetaState](#123-alphabetastate)
13. [Processing Loop Integration](#13-processing-loop-integration)
    - 13.1 [Call Sites in processBlock()](#131-call-sites-in-processblock)
    - 13.2 [Timing Budget](#132-timing-budget)
14. [Parameters and Tuning](#14-parameters-and-tuning)
15. [Diagnostic Outputs](#15-diagnostic-outputs)
16. [Fundamental Limitation: Signal Content Requirement](#16-fundamental-limitation-signal-content-requirement)
17. [Test Track Design](#17-test-track-design)

---

## 1. Motivation and Scope

The LTC decoder (documented in `docs.md`) decodes timecodes as long as the LTC signal is intact
and dominant. When the signal degrades - through noise, dropouts, level loss, or being mixed with
programme audio at similar amplitude - `Q_LTC` drops and `fallback_requested` is raised. At that
point without a fallback the plugin reports the last decoded offset or nothing.

The audio fallback addresses this by maintaining a second, independent lag estimate derived from
the programme audio content on both channels, without relying on any timecode structure. It uses
the observation that any shared acoustic content (speech, music, ambience) will produce correlated
amplitude envelopes across both channels, offset by the same propagation delay that separates the
two recording paths. Detecting this offset by cross-correlation of energy curves gives a direct
measurement of the inter-channel delay that is immune to LTC degradation.

---

## 2. System Architecture

```
Audio input (per sample, both channels)
         │
         ├─► handleTimecode() / SM read        LTC path (slave mode)
         │        │
         │        └─► Q_LTC, ltc_state, d_ms
         │                │
         │                └─► anchor update (anchorMs, anchorHops)
         │
         └─► Bandpass (HPF 100 Hz + LPF 3500 Hz)    Audio fallback path
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
             AlphaBetaTracker (α=0.20, β=0.02, cap 0.10 ms/s)
                  │
                  └─► ab.estMs → targetMs (+by_slider) → delay_size, activeDelayMs
```

---

## 3. Short-Time Energy and the Novelty Function

### 3.1 Hop-Based Energy Accumulation

Audio analysis runs at a hop rate of 10 ms (441 samples at 44100 Hz; adjusted for other sample
rates in `init()`). Each hop accumulates squared samples of the bandpass-filtered signal:

```
E[n] = (1/H) * Σ_{k=0}^{H-1} x_bp[nH + k]²
```

where H = `hopSamples`, n is the hop index, and `x_bp` is the bandpass-filtered input.

In code (`pushAudioAnalysisSample`):

```cpp
// Bandpass: HPF (100 Hz) then LPF (3500 Hz)
const float h1 = audFallback.applyHPF(ch1, audFallback.hpfPrevX1, audFallback.hpfPrevY1);
const float b1 = audFallback.applyLPF(h1, audFallback.lpfPrev1);
audFallback.energyAcc1 += b1 * b1;   // accumulate over H samples
...
float e1 = audFallback.energyAcc1 / (float)audFallback.hopSamples;
```

Both channels are accumulated in parallel, independently.

### 3.2 Novelty as Positive Log-Energy Flux

The novelty function retains only positive frame-to-frame increases in log-energy:

```
nov[n] = max(0, log(E[n] + ε) - log(E[n-1] + ε))
```

where ε = 1e-10 prevents log(0). This is a half-wave rectified first difference of the
log short-time energy, non-zero only when the signal becomes louder (onsets, transient attacks,
rising edges of speech phonemes).

```cpp
static constexpr float NOV_EPS = 1e-10f;
float nov1 = std::max(0.0f,
    std::log(e1 + NOV_EPS) - std::log(audFallback.prevEnergy1 + NOV_EPS));
```

### 3.3 Why Log-Energy Flux Rather Than Linear Flux

**Gain invariance** is the primary motivation for log-scale. The master and slave microphones
are rarely at matched recording levels — a proportional gain difference of 2× on linear flux
would shift the NCC peak because the same transient produces proportionally different magnitudes
on each side. On log scale, a gain difference of A is an additive constant:

```
log(A·E[n] + ε) - log(A·E[n-1] + ε) ≈ log(E[n]) - log(E[n-1])  (for E >> ε/A)
```

So log-flux compares relative energy changes, not absolute values, making the NCC peak
position independent of recording gain.

**Carrier suppression** is a secondary benefit. LTC has nearly constant power, so its
log-energy novelty is near zero. Programme audio has irregular onsets — its novelty is sparse
and aperiodic. Cross-correlating raw energy would find spurious peaks at every multiple of the
LTC frame period (40 ms at 25 fps) because the LTC bit structure repeats periodically.
Log-energy novelty breaks this periodicity just as linear novelty would, while also providing
the gain-invariance benefit above.

### 3.4 Stereo-LTC Guard on Channel 2 Novelty

In the slave instance, `novelty1` tracks the LTC/scene-audio channel and is used for the NCC
against the master reference. `novelty2` nominally tracks the opposite channel (scene audio).
However, when a slave's two input channels are both LTC (stereo routing), carrier energy
entering `novelty2` would corrupt the NCC.

The code suppresses `nov2` proportionally to the slave's own LTC quality:

```cpp
nov2 *= (1.0f - std::min(1.0f, chnl1_in.Q_LTC / 0.5f));
```

At `Q_LTC = 0.5` (FAIL/SUSPECT boundary) `nov2` is fully zeroed. As LTC fades and `Q_LTC`
drops, the gate opens automatically, allowing speech onsets on the scene-audio channel to
accumulate in the novelty buffer cleanly before the NCC fallback activates.

### 3.5 Circular Buffer Organisation

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
the analysis window. If channel 2's content is delayed relative to channel 1 by τ hops:

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
absolute energy level of the programme material. This is reinforced by the log-energy novelty
function (§3.2), which handles gain mismatches before the NCC stage.

The search range depends on mode:

- **Wide mode** (cold-start, no fresh anchor): `lagRange = 200` hops = ±2 seconds.
- **Anchored mode** (anchor fresh and K_eff in range): `NARROW_HALF = 30` hops = ±300 ms,
  centred on the K_eff-corrected anchor offset.

The best lag is:

```
τ* = argmax_{τ ∈ [-L, L]} NCC(τ)
```

Implementation in `estimateAudioFallbackOffset()`:

```cpp
// Means and standard deviations over nccN frames
float m1 = 0.0f, m2 = 0.0f;
for (int i = 0; i < nccN; ++i) { m1 += linBuf1[i]; m2 += linBuf2[i]; }
m1 /= nccN;  m2 /= nccN;

float s1 = 0.0f, s2 = 0.0f;
for (int i = 0; i < nccN; ++i) { /* accumulate variance */ }
s1 = sqrt(s1/nccN);  s2 = sqrt(s2/nccN);

if (s1 < 1e-6f || s2 < 1e-6f) { valid = false; stableCount = 0; return; }

// NCC search - cached to avoid recomputation in runner-up scan
std::array<double, 2*lagRange+1> corrVals{};
for (int lag = -searchHalf; lag <= searchHalf; ++lag)
{
    double sum = 0.0;  int cnt = 0;
    for (int i = 0; i < nccN; ++i)
    {
        int j = i + lag;
        if (j >= 0 && j < nccN)
        { sum += (linBuf1[i]-m1) * (double)(linBuf2[j]-m2); ++cnt; }
    }
    corrVals[lag + searchHalf] = cnt > 0 ? sum / (cnt * s1 * s2) : 0.0;
}
```

If σ₁ < 1e-6 or σ₂ < 1e-6 (one channel has effectively constant novelty - no detectable
events), the estimator exits without updating: `valid = false`, `stableCount = 0`.

**Distance-gated runner-up:** The second-best correlation value is found only at lags ≥ 5 hops
from the best lag, preventing adjacent samples on the same correlation peak from being mistaken
for a competing peak. This is important for prominence calculation:

```cpp
static constexpr int MIN_PEAK_DIST = 5;
double secondBest = -2.0;
for (int lag = -searchHalf; lag <= searchHalf; ++lag)
{
    if (std::abs(lag - bestRelLag) < MIN_PEAK_DIST) continue;
    if (corrVals[lag + searchHalf] > secondBest)
        secondBest = corrVals[lag + searchHalf];
}
```

**Sub-hop parabolic interpolation:** After finding the integer best lag, a parabola is fitted
through the three points `{NCC(τ*-1), NCC(τ*), NCC(τ*+1)}` to estimate a fractional sub-hop
offset. This sharpens lag accuracy from ±10 ms to roughly ±2 ms at no additional NCC cost:

```cpp
// Only applied when peak is not at the search boundary
double y0 = corrVals[bestRelLag - 1 + searchHalf];
double y1 = bestCorr;
double y2 = corrVals[bestRelLag + 1 + searchHalf];
double denom = y2 - 2.0 * y1 + y0;
if (std::abs(denom) > 1e-9)
    subHopOffset = std::max(-0.5, std::min(0.5, -0.5 * (y2 - y0) / denom));
// absoluteLagSub = bestRelLag + subHopOffset  (used in ms conversion)
```

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

No dynamic allocation occurs at estimation time. All buffers are allocated in `init()`, which
is called from `prepareToPlay()`, never from the audio callback.

The anchored mode (the common case once a valid LTC lock has been established) is
approximately 22× cheaper than wide mode.

### 4.4 Sign Convention

The NCC is computed as `Σ n1[i] · n2[i + lag]`. If channel 2 is τ hops **ahead** of channel 1:

```
n2[k] ≈ n1[k + τ]    →    NCC peaks at lag = -τ
```

A positive τ (CH2 leading CH1) produces `bestLag < 0`. This is the opposite sign from the
LTC convention used throughout the plugin, where `d_ms > 0` when CH2 leads CH1.

The conversion applied in `estimateAudioFallbackOffset` is therefore:

```cpp
audFallback.deltaAudMs = -(double)bestLag * audFallback.hopMs;
```

This ensures `deltaAudMs` is directly comparable to `d_ms` from the SM-based delay measurement.

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
```

The stability divisor and required threshold depend on the NCC mode:

| Mode | `stabDivisor` | `stabThresh` | Time to `valid` |
|------|---------------|--------------|-----------------|
| Anchored | 2.0 | 2 | 2 × 200 ms = 400 ms |
| Wide | 3.0 | 3 | 3 × 200 ms = 600 ms |

```cpp
const double stabDivisor = anchorUsable ? 2.0 : 3.0;
const int    stabThresh  = anchorUsable ? 2   : 3;
stability = std::min(1.0, stableCount / stabDivisor);
```

Anchored mode uses a relaxed threshold (2 vs 3) because the narrow ±300 ms search window
dramatically reduces the probability of a false peak: the estimator is constrained to a
neighbourhood where the true peak is known to sit.

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
(or `> 0.25` in anchored mode) as a threshold for switching to the fallback source.

---

## 6. Band-Pass Preprocessing

Before energy accumulation, each channel sample passes through a cascaded 1-pole HPF + 1-pole
LPF, forming a bandpass between approximately 100 Hz and 3500 Hz:

```cpp
// HPF: 1-pole IIR, cutoff 100 Hz
float applyHPF(float x, float& prevX, float& prevY) const noexcept
{
    float y = hpfAlpha * (prevY + x - prevX);
    prevX = x; prevY = y;
    return y;
}

// LPF: 1-pole IIR, cutoff 3500 Hz
float applyLPF(float x, float& prev) const noexcept
{
    float y = prev + lpfAlpha * (x - prev);
    prev = y;
    return y;
}
```

Coefficients computed in `init()` from the host sample rate:

```cpp
// HPF: alpha = 1 / (1 + 2π·fc/sr),  fc = 100 Hz
hpfAlpha = 1.0 / (1.0 + 2π * 100.0 / sampleRate);   // ≈ 0.9872 @ 48 kHz

// LPF: alpha = 2π·fc / (sr + 2π·fc),  fc = 3500 Hz
lpfAlpha = 2π * 3500.0 / (sampleRate + 2π * 3500.0); // ≈ 0.314 @ 48 kHz
```

**HPF rationale (100 Hz):** The LTC carrier contributes a near-constant energy level in the
sub-2.4 kHz band. Without HPF, the noise floor estimate is inflated by the carrier and the
activity gate opens later than necessary once the LTC stops. The 100 Hz pole removes DC and
sub-bass content without affecting the speech/transient band.

**LPF rationale (3500 Hz):** Wind, HVAC artefacts, and codec noise above 3.5 kHz add
uncorrelated high-frequency energy to the novelty envelope. This high-frequency noise reduces
the NCC peak prominence without contributing any useful cross-channel correlation. Cutting above
3500 Hz confines the estimator to the dominant perceptual band for speech and typical programme
audio.

The combined effect is a bandpass filter centred on the mid-frequency speech band, which
maximises the novelty signal-to-noise ratio for the NCC estimator.

---

## 7. Activity Gate

The activity gate suppresses NCC computation during silence, very-low-level content, or
steady-state noise (e.g. the LTC carrier alone). Without it, the NCC would run on flat
novelty curves and produce random lag estimates that poison the stability counter.

### Noise Floor Follower

Each channel maintains an asymmetric exponential follower on the bandpass-filtered energy `e`:

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

## 8. Anchored NCC Mode

When a valid LTC anchor exists and is recent enough, the NCC search is constrained to
a narrow window centred on the anchor. This reduces search cost by ~22× and prevents
the estimator from locking onto spurious lags far from the expected offset.

### Anchor Lifecycle

- **Set:** whenever `d_ms` is committed by the SM-based delay measurement (including the
  hysteresis guard, so only confirmed values set the anchor).
- **Age tracking:** `anchorTimestampMs` records when the anchor was last confirmed. If the
  elapsed time exceeds `ANCHOR_MAX_AGE_MS = 30,000 ms` without a fresh LTC confirmation or
  valid anchored NCC hit, the anchor is considered stale.
- **Refreshed:** a coherent anchored NCC hit also updates `anchorTimestampMs`, so the anchor
  stays live as long as audio correlation is healthy.
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
anchorAgeOk  = (elapsed since last anchor confirmation < ANCHOR_MAX_AGE_MS)
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

### Anchor-Coast During Ring Refill

`purgeNoveltyRing()` is called on the LTC→FAIL edge (see §10.1). This resets `framesFilled`
to 0, which would normally block the estimator for up to 20 seconds (the full ring refill time
at `MASTER_NOV_REF_SIZE = 2000` hops). During this window the anchor and α-β tracker still
hold the true offset — blocking the estimator would cause `fusion.source = None` for the whole
refill period.

The anchor-coast path handles this: if `anchorAgeOk` is true but `framesFilled < windowFrames`,
the estimator immediately returns `deltaAudMs = anchorMs, confAud = 0.8, valid = true`. This
lets the fuser carry the pre-fade offset through the refill window, switching to audio source as
soon as the ring fills and a proper NCC can run.

```
// Entry point of estimateAudioFallbackOffset():
if (anchorAgeOk && framesFilled < windowFrames)
{
    deltaAudMs = anchorMs;  confAud = 0.8;  valid = true;
    return;                 // coast on LTC-captured anchor during ring refill
}
```

### Wide-Mode Guard for Out-of-Range Anchors

If the anchor has aged out but `abs(anchorHops) > lagRange`, any peak found in the wide NCC
search (±lagRange = ±200 hops = ±2 s) is guaranteed to be spurious — the true offset is
beyond the search range. In this case the estimator suppresses NCC, sets `valid = false`, and
lets the α-β tracker coast on its last velocity rather than seeding it to a wildly wrong value:

```cpp
if (!anchorUsable && hasAnchor && std::abs(anchorHops) > lagRange)
{
    valid       = false;
    stableCount = 0;
    return;
}
```

---

## 9. Alpha-Beta Tracker

The raw NCC estimate (`deltaAudMs`) is not fed directly to the delay engine. It passes
through an alpha-beta tracker that smooths position noise and enforces a velocity cap.

### State and Parameters

```cpp
struct AlphaBetaState {
    static constexpr double ALPHA            = 0.20;
    static constexpr double BETA             = 0.02;
    static constexpr double MAX_VEL_MS_PER_S = 0.10;  // ms/s

    double estMs     = 0.0;   // smoothed position estimate
    double velMsPerS = 0.0;   // velocity estimate
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

**Velocity cap rationale (0.10 ms/s):** This prevents the tracker from following acoustic
time-of-arrival differences caused by moving sound sources. A source moving at 34 m/s (the
speed of sound) would be required to move at 34 cm/s relative to the two microphones to create
a 1 ms/s rate of change - far faster than any realistic recorder drift. The 0.10 ms/s cap
covers realistic clock drift rates while rejecting acoustic contamination.

### Seeding

`ab.seed(value)` sets `estMs = value`, `velMsPerS = 0`, `initialized = true`. It is called:

1. At the LTC→fallback transition (seeds from last good `d_ms`), ensuring the tracker starts
   from the known-good anchor value with zero initial velocity.
2. When the NCC estimate jumps more than 150 ms from the tracker's current `estMs`
   (large-jump fast path - handles manual track shifts in the DAW).

The 150 ms threshold is above normal NCC jitter (±10 ms / one hop) but below a typical
manual re-sync operation, so spurious NCC outliers are absorbed by the tracker rather
than triggering a re-seed.

---

## 10. Fusion Policy

`fuseLtcAndAudioFallback()` runs every 200 ms, triggered from within
`pushAudioAnalysisSample()` at each refresh boundary. It reads the current LTC quality state
and the audio fallback estimate and writes to the `FusionState` struct.

### 10.1 Source Selection Logic

In slave mode, the LTC quality gate (`ltcOk`) requires:

```
ltcOk = masterValid                          // master SM data is fresh (< 2 s old)
     && (chnl1_in.ltc_state == VALID)        // slave's own LTC Q > 0.8
     && !drift_suspected                     // no cross-channel drift anomaly
```

The full source selection:

```
if ltcOk:
    source = LTC
    selectedMs = d_ms                    (current SM-derived offset)
    selectedConf = Q_LTC (slave channel)

else if (not inTransitionHold)
     && audFallback.valid
     && audFallback.confAud > confThresh:   // 0.25 anchored, 0.4 wide
    source = AudioFallback
    selectedMs = audFallback.deltaAudMs
    selectedConf = audFallback.confAud

else:
    source = None
    selectedMs = 0
    selectedConf = 0
```

`inTransitionHold` is true for 2.5 s after LTC drops to FAIL - this hold period lets the
LTC carrier clear from the novelty buffer before the NCC is allowed to run on a potentially
contaminated window. A novelty ring purge is also triggered at the FAIL edge.

The tighter confidence threshold in anchored mode (`confThresh = 0.25` vs `0.4`) reflects the
lower false-peak rate when the search window is constrained to ±300 ms.

### 10.2 Hold-on-Abstain Behaviour

When the fusion abstains (`source = None`), the delay engine is not zeroed. The intent is to
avoid a rebuild transient: if LTC has just failed and the audio fallback has not yet reached
`valid`, holding the last committed delay is better than cutting to zero. The GUI clearly shows
`src=NONE` so the operator is aware that the output is coasting on a stale value.

### 10.3 Relation to Q_LTC and fallback_requested

`quality_scoring.md` defines:
- `fallback_requested` (per-channel): set when `ltc_state == FAIL` only
- `fallback_requested` (processor-level): either channel FAIL, or `drift_suspected`

The fusion is a **superset** of these signals:
- It fires when the slave channel is SUSPECT (Q ≤ 0.8), not only FAIL
- It requires `masterValid` in addition to the slave's LTC state
- It provides a concrete alternative offset, not just a binary flag
- It distinguishes "LTC degraded, audio fallback ready" from "nothing available"

The processor-level `fallback_requested` flag is retained for downstream systems that only
need a boolean "something is wrong" signal. The fusion state is the richer output.

---

## 11. Delay Engine Integration

### 11.1 targetMs Derivation

At each sample in the processing loop, `targetMs` is resolved as follows:

```
if masterValid:
    targetMs = d_ms + by_slider       // SM-derived offset + manual correction

elif ab.initialized:
    targetMs = ab.estMs + by_slider   // alpha-beta estimate + manual correction

else:
    targetMs = 0.0                    // no data yet; no delay applied
```

`d_ms` is the sample-accurate inter-track delay derived from the master's shared memory
slot - specifically from the difference between the two decoders' last frame-detect sample
positions on the shared DAW timeline, corrected for the timecode offset:

```
d_ms = (slave_decode_sample - master_decode_sample) / sampleRate * 1000
       - (tc_slave_ms - tc_master_ms)
```

`by_slider` is the manual correction offset set by the operator (range ±250 ms). It is
added at the `targetMs` derivation point so that the NCC, anchor, and alpha-beta tracker
all operate in raw-delay space; the manual correction is purely a post-estimation trim.

When `masterValid` is false (master SM data stale > 2 s), `ab.estMs` carries the last
reliable estimate forward, decaying at velocity `velMsPerS`. The fusion source at this
point is typically `None`, but the tracker continues to provide a plausible value for
the delay engine so that the output does not abruptly cut.

### 11.2 Rebuild-on-Shift

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

`chnl1.clear()` and `chnl2.clear()` reset the delay FIFOs and all decoder state. On the next
sample where `targetMs != 0`, `delay_size` is recommitted from the new `targetMs` and
`activeDelayMs` is updated.

**Why 2 frames?** The audio fallback has a 10 ms hop resolution. Normal NCC noise produces
lag estimates that vary by ±1 hop (±10 ms) between consecutive windows, well below the 2-frame
threshold. A genuine source switch or manual correction would produce a difference of at least
2 frames if the two estimates disagree significantly. The threshold prevents rebuilds from NCC
jitter while catching real offset corrections.

**Rebuild transient:** `chnl2.clear()` resets `chnl2.delay_buf`, causing brief silence (zero
output) on channel 2 while the FIFO fills to the new target size. `chnl1` is served from
`const_buf` (a 500,000-sample historical ring buffer), so channel 1 is not silenced. For
typical lag values (tens to hundreds of milliseconds), the channel 2 silence lasts the same
duration as the target delay.

### 11.3 activeDelayMs Lifecycle

| Event | Effect on activeDelayMs |
|---|---|
| `delay_size` committed on first activation | Set to `targetMs` |
| `delay_size` committed after rebuild | Set to `targetMs` |
| `active_delay` button turned off | Set to 0 |
| LTC jump resets delay (`|newDtMs - d_ms| > 2 frames`) | Set to 0, then recommitted |
| Fusion target exceeds rebuild threshold | Set to 0, then recommitted |

`activeDelayMs` is the single ground truth for what is currently programmed into the delay
engine. It is copied to `aud_activeDelayMs` in the 0.1 s diagnostic update for display in the GUI.

---

## 12. Data Structures

### 12.1 AudioFallbackState

Declared in `PluginProcessor.h` as a plain struct before `AutoSyncAudioProcessor`.

**Parameters (set by `init(double sampleRate)`):**

| Field | Type | Value (44100 Hz) | Description |
|---|---|---|---|
| `hopSamples` | `int` | 441 | Samples per energy accumulation hop (10 ms) |
| `windowFrames` | `int` | 2000 | Full novelty ring length (20 s; = MASTER_NOV_REF_SIZE) |
| `lagRange` | `int` | 200 | Wide NCC search range in hops (±2 s) |
| `refreshEvery` | `int` | 20 | Hops between NCC re-computations (200 ms) |
| `hopMs` | `double` | 10.0 | Hop duration in ms (used for lag→ms conversion) |
| `hpfAlpha` | `float` | ≈0.9856 @ 44100 Hz | 1-pole HPF coefficient; cutoff 100 Hz; computed in `init()` |
| `lpfAlpha` | `float` | ≈0.3330 @ 44100 Hz | 1-pole LPF coefficient; cutoff 3500 Hz; computed in `init()` |

**Compile-time constants:**

| Constant | Value | Description |
|---|---|---|
| `NARROW_HALF` | 30 hops | Half-width of anchored NCC search (±300 ms) |
| `ANCHORED_WIN` | 120 frames | NCC window used in anchored mode (1.2 s) |
| `WIDE_WIN` | 400 frames | NCC window used in wide/cold-start mode (4 s) |
| `NOISE_FLOOR_TC_RISE` | 0.995 | Noise-floor follower slow-rise time constant (~2 s) |
| `NOISE_FLOOR_TC_FALL` | 0.80 | Noise-floor follower fast-fall time constant (~45 ms) |
| `ACTIVITY_GATE_DB` | 8.0 dB | Signal must exceed noise floor by this to open gate |

**Per-hop accumulators:**

| Field | Type | Description |
|---|---|---|
| `energyAcc1/2` | `float` | Sum of bandpass-filtered x² over current hop, both channels |
| `prevEnergy1/2` | `float` | Mean energy of previous hop (for novelty difference) |
| `hopSampleCounter` | `int` | Samples elapsed in current hop |
| `hopsSinceRefresh` | `int` | Hops elapsed since last NCC computation |

**Band-pass filter state:**

| Field | Type | Description |
|---|---|---|
| `hpfPrevX1/2` | `float` | Previous input sample for per-channel 1-pole HPF |
| `hpfPrevY1/2` | `float` | Previous output sample for per-channel 1-pole HPF |
| `lpfPrev1/2` | `float` | Previous output sample for per-channel 1-pole LPF |

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

**Estimation results:**

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

### 12.2 FusionState

| Field | Type | Description |
|---|---|---|
| `source` | `enum Source` | `None`, `LTC`, or `AudioFallback` |
| `selectedMs` | `double` | Offset selected by fusion policy (ms) |
| `selectedConf` | `double` | Confidence of selected source |
| `fallbackActive` | `bool` | True when `source == AudioFallback` |

`FusionState` is an audio-thread-only struct. The GUI reads `aud_fusionSource` (an `int` copy
updated every 0.1 s in the diagnostic block), not `fusion.source` directly.

### 12.3 AlphaBetaState

| Field | Type | Description |
|---|---|---|
| `estMs` | `double` | Smoothed position estimate (ms) |
| `velMsPerS` | `double` | Velocity / drift estimate (ms/s) |
| `initialized` | `bool` | False until first `seed()` call |
| `lastUpdateMs` | `int64_t` | Wall-clock time of last update (for dt computation) |
| `ALPHA` | const double = 0.20 | Position correction gain |
| `BETA` | const double = 0.02 | Velocity correction gain |
| `MAX_VEL_MS_PER_S` | const double = 0.10 | Velocity cap (ms/s) |

---

## 13. Processing Loop Integration

### 13.1 Call Sites in processBlock()

```
per sample (slave mode):
    handle_const_delay(write1[i], chnl1)           // feed historical buffer (CH1)
    processTimeCode(ltcSample, chnl1_in, ...)      // LTC decode on LTC channel
      → Q_LTC update
    pushAudioAnalysisSample(ltcSample, sceneSample)
      // ltcSample  = LTC-carrying channel (raw, undelayed)
      // sceneSample = non-LTC channel (scene audio if available)
      // internally: bandpass (HPF+LPF) applied to both channels
      // internally: noise-floor follower → activityGate update
    [LTC jump detection + d_ms_pending hysteresis]
    targetMs = d_ms + by_slider   (if masterValid)
           or  ab.estMs + by_slider  (if ab initialized)
    [rebuild check: |targetMs - activeDelayMs| > 2 frames → clear FIFOs]
    delay(write1/2, i, chnl1/2)  // delay engine uses targetMs

every ~200 ms (inside pushAudioAnalysisSample, at hop boundary):
    [if activityGate closed → return, alpha-beta coasts]
    estimateAudioFallbackOffset()
      // anchored mode if anchorUsable, else wide mode
      // K_eff corrects for SM read-staleness in slave mode
    fuseLtcAndAudioFallback()
    if (source == AudioFallback && large jump > 150 ms): ab.seed(deltaAudMs)
    ab.update(deltaAudMs, feedMeasured)            // alpha-beta step
    [anchor update: anchorMs = d_ms when ltcOk && !d_ms_recently_jumped]

every ~0.1 s (diagnostic block):
    copy fusion fields to public aud_* members
    write master novelty snapshot to shared memory (master mode)
    read master novelty snapshot + d_ms from shared memory (slave mode)
      → d_ms derived from sample-accurate decode positions:
        d_ms = (slave_decode_sample - master_decode_sample)/sr*1000
               - (tc_slave_ms - tc_master_ms)
```

`pushAudioAnalysisSample` must be called **before** `delay()` modifies the write pointers.
The fallback operates on raw, undelayed input samples - the K_eff anchor mechanism in
`estimateAudioFallbackOffset` accounts for the inter-track time offset mathematically
rather than by buffering delayed audio.

**Note:** `calc_delay()` (a per-frame timecode subtraction function) remains in the source
as a reference implementation but is no longer called in the current master-slave path.
The sample-accurate SM-based computation above supersedes it.

### 13.2 Timing Budget

| Operation | Frequency | Cost |
|---|---|---|
| Bandpass filtering + energy accumulation | Every sample | 4 multiplies + 4 adds |
| Hop boundary bookkeeping | Every 441 samples | ~10 operations |
| NCC computation (anchored) | Every 8820 samples (~200 ms) | ~7,320 MACs |
| NCC computation (wide) | Every 8820 samples (~200 ms) | ~160,400 MACs |
| Fusion policy | Every 8820 samples | ~10 comparisons |
| Diagnostic copy + SM read | Every 4410 samples (~100 ms) | ~20 field copies |

The dominant cost is the wide NCC, which runs at most 5 times per second. At a typical DAW
block size of 512 samples, it adds about 160,400/17 ≈ 9,435 MACs per block. Anchored NCC
(the steady-state case after LTC lock) is ~22× cheaper at ~430 MACs per block.

---

## 14. Parameters and Tuning

| Parameter | Location | Default | Effect |
|---|---|---|---|
| `hopMs` / `hopSamples` | `AudioFallbackState::init()` | 10 ms | Lag resolution and novelty time granularity. Smaller = finer lag grid but more NCC lags. |
| `windowFrames` | `AudioFallbackState::init()` | 2000 (20 s ring) | Full novelty ring; NCC sub-windows are ANCHORED_WIN=120 or WIDE_WIN=400. |
| `lagRange` | `AudioFallbackState::init()` | 200 (±2 s wide) | Wide-mode maximum detectable delay. Anchored mode uses NARROW_HALF=30 (±300 ms). |
| `NARROW_HALF` | compile-time | 30 hops (±300 ms) | Half-width of anchored NCC search. Must be < ANCHORED_WIN/2. |
| `ANCHORED_WIN` | compile-time | 120 frames (1.2 s) | NCC window in anchored mode. Controls how quickly the estimator reacts to a manual track shift. |
| `WIDE_WIN` | compile-time | 400 frames (4 s) | NCC window in wide mode. |
| `ANCHOR_MAX_AGE_MS` | compile-time | 30,000 ms | How long an anchor remains valid without LTC or NCC confirmation. Beyond this the estimator falls back to wide mode. |
| `refreshEvery` | `AudioFallbackState::init()` | 20 (200 ms) | NCC re-computation interval. Lower = faster tracking, higher CPU. |
| `ACTIVITY_GATE_DB` | compile-time | 8 dB | Signal must exceed tracked noise floor by this margin for the NCC to run. |
| `ALPHA` | `AlphaBetaState` | 0.20 | Position gain. Higher = faster response but more jitter. |
| `BETA` | `AlphaBetaState` | 0.02 | Velocity gain. Higher = faster drift tracking. |
| `MAX_VEL_MS_PER_S` | `AlphaBetaState` | 0.10 ms/s | Velocity cap; prevents tracker from following moving acoustic sources. |
| conf_AUD threshold | `fuseLtcAndAudioFallback()` | 0.4 wide / 0.25 anchored | Minimum confidence to switch from LTC to audio fallback. |
| `stableCount` gate | `estimateAudioFallbackOffset()` | ≥ 3 | Consecutive consistent estimates required before `valid = true` (600 ms minimum). |
| stability agreement | `estimateAudioFallbackOffset()` | ±2 hops | Agreement window for stability counting. Covers ±20 ms NCC grid noise. |
| rebuild threshold | `processBlock()` | 2 frames | `|targetMs - activeDelayMs|` must exceed this to trigger a delay engine rebuild. |
| `by_slider` range | UI / state | ±250 ms | Manual correction offset added to `targetMs` after the alpha-beta tracker. |

---

## 15. Diagnostic Outputs

The following public fields are written by the audio thread in the 0.1 s diagnostic block and
read by the GUI timer:

| Field | Type | Content |
|---|---|---|
| `aud_deltaMs` | `double` | `audFallback.deltaAudMs` - last NCC lag estimate in ms |
| `aud_conf` | `double` | `audFallback.confAud` - combined confidence [0, 1] |
| `aud_fusionSource` | `int` | `(int)fusion.source` - 0=None, 1=LTC, 2=AudioFallback |
| `aud_activeDelayMs` | `double` | `activeDelayMs` - offset currently in the delay engine |

The diagnostics card formats these as:

```
AUD: dt=Xms  conf=Y.YY  src=SRC  applied=Zms
```

Example readings:

```
AUD: dt=80ms   conf=0.85  src=LTC   applied=80ms    ← LTC healthy, audio confirms
AUD: dt=80ms   conf=0.78  src=AUD   applied=80ms    ← LTC failed, fallback active
AUD: dt=---    conf=0.00  src=NONE  applied=80ms    ← both failed, holding last value
AUD: dt=80ms   conf=0.12  src=NONE  applied=80ms    ← audio has peak but not stable yet
```

The `applied` field is the most operationally significant: it shows what the delay engine is
actually doing, independent of which source is currently providing evidence.

---

## 16. Fundamental Limitation: Signal Content Requirement

The audio fallback requires **shared acoustic content** to produce a reliable lag estimate.
The NCC of novelty curves finds a peak only if both channels respond to the same acoustic
events. Three scenarios affect this:

**LTC-only channels:** LTC has near-constant power; its novelty is near zero. If both channels
carry only LTC, both novelty curves are flat and σ₁ or σ₂ will be below the 1e-6 guard,
causing the estimator to exit without an estimate. The fallback will never produce `valid = true`
in this case.

**Mixed LTC + programme audio:** The novelty is driven by the programme audio component (the
LTC contribution cancels in the bandpass-filtered energy difference). The fallback will work if
the programme audio has sufficient dynamic variation (speech, music with transients). It will
not work on sustained tones, fade-outs, or silence.

**Programme audio only (LTC absent):** Best case for the fallback. The novelty is driven
entirely by the programme content, and the NCC will find the lag reliably given enough acoustic
events. This is the target operating condition for the fallback.

**Practical implication:** The recommended test design is `08_audio_ltc_fading.wav`: LTC
dominant for the first 30 seconds (audio fallback accumulates evidence in the background), sharp
LTC cut at 30 seconds (fallback takes over). A gradual LTC fade creates a "danger zone" where
LTC is too degraded to decode but the fallback hasn't yet seen 600 ms of consistent evidence -
both estimators fail simultaneously. A sharp cut ensures the fallback has a full, clean window
from the LTC-healthy period to work from immediately after the cut.

---

## 17. Test Track Design

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

The expected convergence time after the cut is approximately:
- `stableCount` requires 3 × 200 ms = 600 ms of consistent estimates
- First estimate runs 200 ms after the cut
- Therefore `valid = true` and `src=AUD` by approximately t = 30.8 s in the best case

In practice, the confidence may take 1–2 additional windows to cross the threshold if the
speech content in that window is sparse. The `applied` field should match `+80ms` throughout
the LTC period and remain at `+80ms` during the NONE phase due to hold-on-abstain, then be
confirmed by `src=AUD` once the fallback converges.
