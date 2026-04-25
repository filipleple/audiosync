# Cross-reference: design.md vs. implementation

## What aligns well

**§2 - Entry state at LTC dropout**
The anchor-and-track work nails this. `anchorMs`, `anchorHops`, `ANCHOR_MAX_AGE_MS` are the
exact equivalent of seeding with `D0` at dropout. The anchor is kept continuously fresh while
LTC is healthy so the handoff is instantaneous.

**§4 - Constrained / limited-lag search**
The narrow `±NARROW_HALF` (±300 ms) anchored search does what the design recommends. The
anchored path with staleness correction (`K_eff`) is in the spirit of the "center on D_pred"
guidance.

**§5 - Two-stage estimator (partial)**
The "anchored vs. wide" dual path is conceptually the coarse+fine split. Wide = coarse rescue;
anchored = fine estimator.

**§6 - Sub-hop interpolation**
Done (parabola fit, ±10 ms → ~±2 ms). Matches §5's "quadratic interpolation."

---

## What is missing or diverges

### 1. Analysis signal preprocessing (§3) - ⚠️ PARTIAL

design.md says:
> downmix → DC blocker / high-pass → band-limit → optional decimation
> high-pass: 80–120 Hz, speech/general scene band: 150–3500 Hz

**Status (as of current HEAD):** HPF is now applied. A 1-pole IIR HPF at 100 Hz
(`hpfAlpha` computed from sample rate in `AudioFallbackState::init()`) filters each channel
before energy accumulation:

```cpp
const float h1 = audFallback.applyHPF(ch1, audFallback.hpfPrevX1, audFallback.hpfPrevY1);
const float h2 = audFallback.applyHPF(ch2, audFallback.hpfPrevX2, audFallback.hpfPrevY2);
```

Still missing compared to the full design prescription:
- **Band-pass / low-pass:** no upper-frequency limit; wind, HVAC, codec artefacts above
  3.5 kHz are included.
- **Decimation:** analysis runs at full sample rate (44.1 / 48 kHz), not the 12 kHz
  recommended by design.md. This increases NCC computational cost but has negligible
  impact in practice given the 10 ms hop granularity.
- **Scene audio channel separation:** both master and slave feed `ltcSample` as ch1 and
  the non-LTC channel (scene audio when available, near-silence otherwise) as ch2.
  When the track is LTC-only, ch2 carries near-silence and the activity gate suppresses
  NCC updates automatically.

### 2. Plain NCC on energy novelty instead of GCC-PHAT (§4) - MEDIUM IMPACT

design.md says:
> Use GCC-PHAT on the band-passed waveform as the primary estimator.
> Energy/onset envelope correlation is Path B - a sanity check, not the main estimator.

What's implemented: plain NCC on positive-energy-change novelty envelopes. This is entirely
Path B (§3 "Path B: onset/energy envelope correlation"). GCC-PHAT is not present.

In practice, the novelty approach is more robust to spectral coloration differences between
very different mic positions, which matters here. But GCC-PHAT gives better time resolution
when audio quality is good.

### 3. Alpha-beta tracker on fallback output (§7) - ✅ IMPLEMENTED

design.md says:
> Do not feed raw estimates to the delay line. Use an alpha-beta filter (α=0.20, β=0.02).

**Status (as of current HEAD):** Fully implemented. `AlphaBetaTracker` is declared in
`PluginProcessor.h` with `ALPHA = 0.20`, `BETA = 0.02`, and `MAX_VEL_MS_PER_S = 0.20`.
It is seeded from the last good LTC anchor at the LTC→fallback transition and updated
every NCC refresh cycle (~200 ms):

```cpp
// Alpha-beta tracker - updated every NCC refresh cycle (~200 ms).
if (fusion.source == FusionState::Source::AudioFallback && audFallback.valid
    && ab.initialized && std::abs(audFallback.deltaAudMs - ab.estMs) > 150.0)
    ab.seed(audFallback.deltaAudMs);   // large-jump fast path (> 150 ms)

if (ab.initialized)
{
    const bool feedMeasured = (fusion.source == FusionState::Source::AudioFallback)
                               && audFallback.valid;
    ab.update(audFallback.deltaAudMs, feedMeasured);
}
```

The `targetMs` derivation now uses `ab.estMs` (the smoothed tracker output) rather than the
raw NCC estimate.

### 4. Correction-rate limit (§8) - ✅ IMPLEMENTED

design.md says:
> This is the single most important tuning parameter for your use case.
> Limit to 0.05–0.20 ms/s; start at 0.10 ms/s.

**Status (as of current HEAD):** Implemented inside `AlphaBetaTracker::update()`.
The velocity term is clamped to ±`MAX_VEL_MS_PER_S` = 0.20 ms/s before being applied:

```cpp
velMsPerS = std::max(-MAX_VEL_MS_PER_S,
            std::min( MAX_VEL_MS_PER_S, velMsPerS));
```

This prevents the fallback from chasing acoustic TDOA changes caused by moving sources.

### 5. Activity gate (§10) - ✅ IMPLEMENTED

design.md says:
> Hold prediction, keep confidence low, wait for informative audio during silence/noise.
> Activity gate: short-term RMS ≥ 6–10 dB above tracked noise floor.

**Status (as of current HEAD):** Implemented. `AudioFallbackState` maintains an asymmetric
per-channel noise-floor follower and an `activityGate` flag:

```cpp
static constexpr float NOISE_FLOOR_TC_RISE = 0.995f;  // slow rise (~2 s)
static constexpr float NOISE_FLOOR_TC_FALL = 0.80f;   // fast fall (~150 ms)
static constexpr float ACTIVITY_GATE_DB    = 8.0f;    // dB above noise floor
bool  activityGate = false;
```

In `estimateAudioFallbackOffset()`, the NCC is skipped when the gate is closed:

```cpp
if (!audFallback.activityGate)
    return;
```

The fast fall time constant allows the gate to open quickly (~150–250 ms) after the LTC
carrier stops, so quieter programme audio can trip it without waiting for the slow rise
time constant to decay.

### 6. No velocity / slope tracking (§2, §7) - MEDIUM IMPACT

design.md says:
> Predict forward: D_pred[k] = D_est[k-1] + v_est[k-1] * dt
> Estimate v0 from robust slope of last 1.0 s of LTC delays.

What's implemented: during fallback the estimate holds at `anchorMs` (D0) only. No slope
extrapolation. Fine for dropouts of a few seconds; accumulates error for longer ones.

### 7. Delay line not continuously variable (§9b) - LOW IMPACT (for now)

design.md says:
> Use a fractional delay line for small moves, crossfade for larger corrections.
> If change < 0.25 sample/block: continuous fractional movement.
> If jump > ~2–8 samples: switch taps with a 5–10 ms crossfade.

What's implemented: the delay line commits the full new value in one block when activated
(hard commit). The `rebuildThreshMs` guard prevents spurious large jumps from NCC noise but
there is no gradual ramping or crossfade on correction.

### 8. No multi-band robustness (§11) - LOW PRIORITY

design.md says:
> For a 360-camera mic: run a 3-band estimator (150–500 Hz, 500–1500 Hz, 1500–4000 Hz),
> compute confidence per band, take weighted median lag.

What's implemented: single broadband path only.

---

## Summary table

| design.md §   | What it says                                       | Status                               |
|---------------|----------------------------------------------------|--------------------------------------|
| §2 D0 seeding | Seed fallback with last LTC delay at dropout       | ✅ done (anchor system)               |
| §3 Signal path | HPF + bandpass + optional decimation              | ⚠️ partial - HPF @ 100 Hz added; no bandpass or decimation |
| §4 GCC-PHAT   | GCC-PHAT on band-passed waveform as primary        | ❌ NCC on energy novelty used instead |
| §4 Limited search | Center on D_pred, narrow range                | ✅ done (anchored mode, K_eff staleness correction) |
| §5 Two-stage  | Coarse envelope + fine GCC-PHAT                    | ⚠️ partial (wide/narrow dual path)   |
| §6 Sub-sample | Quadratic peak fit                                 | ✅ done (parabola)                    |
| §7 Alpha-beta | Don't feed raw estimates to delay line             | ✅ done (α=0.20, β=0.02; 150 ms fast-seed path) |
| §8 Rate limit | Cap correction at 0.05–0.20 ms/s                  | ✅ done (MAX_VEL_MS_PER_S = 0.20)    |
| §9b Variable delay | Crossfade / ramp on correction               | ❌ missing - hard commit              |
| §10 Activity gate | Hold on silence / below noise floor          | ✅ done (8 dB gate, asymmetric noise follower) |
| §11 Multi-band | 3-band weighted median                            | ❌ not started                        |

---

## Remaining work (bang-for-buck order)

Items 1–3 and the HPF portion of item 4 from the original list have been implemented.
Remaining:

1. **Bandpass filter on analysis input** (§3) - add a low-pass (≤ 3.5 kHz) after the
   existing HPF @ 100 Hz to exclude wind, HVAC, and codec artefacts. Requires deciding
   whether to route a separate scene audio channel rather than the LTC track.
2. **GCC-PHAT** (§4) - add as a fine-stage estimator once the analysis path is correct.
3. **Velocity / slope tracking** (§2/§7) - slope history from last 1 s of LTC delays for
   long-dropout extrapolation beyond the current anchor-hold behaviour.
4. **Crossfade on delay correction** (§9b) - audio quality improvement for large corrections;
   replaces the current hard-commit rebuild.
5. **Multi-band** (§11) - 3-band weighted-median robustness upgrade; lowest priority.
