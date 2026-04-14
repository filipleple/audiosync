# Cross-reference: design.md vs. implementation

## What aligns well

**§2 — Entry state at LTC dropout**
The anchor-and-track work nails this. `anchorMs`, `anchorHops`, `ANCHOR_MAX_AGE_MS` are the
exact equivalent of seeding with `D0` at dropout. The anchor is kept continuously fresh while
LTC is healthy so the handoff is instantaneous.

**§4 — Constrained / limited-lag search**
The narrow `±NARROW_HALF` (±300 ms) anchored search does what the design recommends. The
anchored path with staleness correction (`K_eff`) is in the spirit of the "center on D_pred"
guidance.

**§5 — Two-stage estimator (partial)**
The "anchored vs. wide" dual path is conceptually the coarse+fine split. Wide = coarse rescue;
anchored = fine estimator.

**§6 — Sub-hop interpolation**
Done (parabola fit, ±10 ms → ~±2 ms). Matches §5's "quadratic interpolation."

---

## What is missing or diverges

### 1. Analysis signal preprocessing (§3) — HIGH IMPACT

design.md says:
> downmix → DC blocker / high-pass → band-limit → optional decimation
> high-pass: 80–120 Hz, speech/general scene band: 150–3500 Hz

What's implemented: raw `ltcSample` (the LTC track, full-band 48 kHz) is fed directly to
`pushAudioAnalysisSample` in both master and slave mode. This means:
- The novelty curve is dominated by LTC biphase tone structure (~2.4 kHz burst patterns),
  not scene transients.
- Wind, HVAC, codec artifacts all included unfiltered.
- No HPF.

Whether this is a bug or an acceptable shortcut depends on whether you have separate scene
audio to feed in. If not, document it explicitly as a constraint.

### 2. Plain NCC on energy novelty instead of GCC-PHAT (§4) — MEDIUM IMPACT

design.md says:
> Use GCC-PHAT on the band-passed waveform as the primary estimator.
> Energy/onset envelope correlation is Path B — a sanity check, not the main estimator.

What's implemented: plain NCC on positive-energy-change novelty envelopes. This is entirely
Path B (§3 "Path B: onset/energy envelope correlation"). GCC-PHAT is not present.

In practice, the novelty approach is more robust to spectral coloration differences between
very different mic positions, which matters here. But GCC-PHAT gives better time resolution
when audio quality is good.

### 3. No alpha-beta tracker on fallback output (§7) — HIGH IMPACT

design.md says:
> Do not feed raw estimates to the delay line. Use an alpha-beta filter (α=0.20, β=0.02).

What's implemented: `fusion.selectedMs` (raw NCC output) is fed directly to `targetMs` in the
same `processBlock` call. No smoothing, no velocity tracking. In `PluginProcessor.cpp`:

```cpp
else if (fusion.fallbackActive && fusion.selectedMs != 0.0)
    targetMs = fusion.selectedMs;  // raw NCC estimate, no smoothing
```

A single bad NCC estimate causes an immediate hard delay jump.

### 4. No correction-rate limit (§8) — HIGH IMPACT

design.md says:
> This is the single most important tuning parameter for your use case.
> Limit to 0.05–0.20 ms/s; start at 0.10 ms/s.

What's implemented: nothing. The fallback can move `targetMs` by an arbitrary amount
per estimation cycle. Moving acoustic sources will cause the NCC to chase acoustic TDOA
changes that are not recorder drift, destabilizing the applied delay.

### 5. No activity gate (§10) — MEDIUM IMPACT

design.md says:
> Hold prediction, keep confidence low, wait for informative audio during silence/noise.
> Activity gate: short-term RMS ≥ 6–10 dB above tracked noise floor.

What's implemented: NCC runs unconditionally every `refreshEvery=20` hops. The `s1 < 1e-6f`
early-exit catches complete silence but not low-energy or steady-state content (HVAC, wind,
the LTC carrier itself in quiet passages).

### 6. No velocity / slope tracking (§2, §7) — MEDIUM IMPACT

design.md says:
> Predict forward: D_pred[k] = D_est[k-1] + v_est[k-1] * dt
> Estimate v0 from robust slope of last 1.0 s of LTC delays.

What's implemented: during fallback the estimate holds at `anchorMs` (D0) only. No slope
extrapolation. Fine for dropouts of a few seconds; accumulates error for longer ones.

### 7. Delay line not continuously variable (§9b) — LOW IMPACT (for now)

design.md says:
> Use a fractional delay line for small moves, crossfade for larger corrections.
> If change < 0.25 sample/block: continuous fractional movement.
> If jump > ~2–8 samples: switch taps with a 5–10 ms crossfade.

What's implemented: the delay line commits the full new value in one block when activated
(hard commit). The `rebuildThreshMs` guard prevents spurious large jumps from NCC noise but
there is no gradual ramping or crossfade on correction.

### 8. No multi-band robustness (§11) — LOW PRIORITY

design.md says:
> For a 360-camera mic: run a 3-band estimator (150–500 Hz, 500–1500 Hz, 1500–4000 Hz),
> compute confidence per band, take weighted median lag.

What's implemented: single broadband path only.

---

## Summary table

| design.md §   | What it says                                       | Status                               |
|---------------|----------------------------------------------------|--------------------------------------|
| §2 D0 seeding | Seed fallback with last LTC delay at dropout       | ✅ done (anchor system)               |
| §3 Signal path | HPF + bandpass + optional decimation              | ❌ missing — raw LTC channel used     |
| §4 GCC-PHAT   | GCC-PHAT on band-passed waveform as primary        | ❌ NCC on energy novelty used instead |
| §4 Limited search | Center on D_pred, narrow range                | ✅ done (anchored mode)               |
| §5 Two-stage  | Coarse envelope + fine GCC-PHAT                    | ⚠️ partial (wide/narrow dual path)   |
| §6 Sub-sample | Quadratic peak fit                                 | ✅ done (parabola)                    |
| §7 Alpha-beta | Don't feed raw estimates to delay line             | ❌ missing — raw fusion output used   |
| §8 Rate limit | Cap correction at 0.05–0.20 ms/s                  | ❌ missing entirely                   |
| §9b Variable delay | Crossfade / ramp on correction               | ❌ missing — hard commit              |
| §10 Activity gate | Hold on silence / below noise floor          | ❌ missing                            |
| §11 Multi-band | 3-band weighted median                            | ❌ not started                        |

---

## Recommended implementation order (bang-for-buck)

1. **Alpha-beta tracker** (§7) — small state, big stability gain; prevents one bad NCC hop
   from slamming the delay line.
2. **Correction rate limiter** (§8) — five lines of code; critical for the moving-source
   use case; design.md calls it the single most important tuning parameter.
3. **Activity gate** (§10) — tracked noise floor per channel; gate NCC updates when
   signal is too weak to produce a reliable peak.
4. **Analysis preprocessing** (§3) — HPF + bandpass on scene audio fed to novelty extraction;
   changes the *meaning* of the fallback (requires deciding whether to use LTC track or a
   separate scene audio channel).
5. **GCC-PHAT** (§4) — add as a fine-stage estimator once the analysis path is correct.
6. **Velocity tracking** (§2/§7) — slope history for long-dropout extrapolation.
7. **Crossfade on delay correction** (§9b) — audio quality improvement for large corrections.
8. **Multi-band** (§11) — robustness upgrade for bad camera mics; last.
