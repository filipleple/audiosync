
Yes. In this LTC-primary setup, the fallback should be designed as a **constrained residual tracker**, not as a free-running audio synchronizer. The best core estimator is still **GCC-PHAT**, which is a standard time-delay estimator, but here it should only search in a **small window around the last LTC-derived delay**. Also, because this is a VST, all heavy analysis should stay off the audio thread; Steinberg explicitly advises against blocking calls, filesystem/UI/network access, and memory allocation/deallocation in the real-time process function, and documents host latency handling through `getLatencySamples()` / `kLatencyChanged`. ([speechprocessingbook.aalto.fi][1])

The most important design caveat is this: your fallback must track **recorder drift/jitter**, not **instantaneous acoustic arrival differences** between mics. If the master mic and slave mic are physically separated, moving sources can create real acoustic delay changes that have nothing to do with recorder sync. So the fallback must be **slow, bounded, and conservative**. That is the main parameter-tuning consequence of your context. ([speechprocessingbook.aalto.fi][1])

## 1) What the fallback should output

Per slave, the fallback route should produce only three things every analysis hop:

* `delay_estimate_samples`
* `confidence`
* `max_safe_rate_of_change`

The audio thread should only consume a **smoothed target delay**. It should not run the estimator itself. That separation follows the VST real-time constraints. ([Steinberg Media][2])

## 2) Entry state when LTC drops

At switchover, seed the fallback with:

* `D0`: last trusted LTC delay, in samples
* `v0`: recent delay slope from LTC, in samples/sec
* `sigma0`: recent LTC jitter / short-term variance
* `t_drop`: time since dropout began

Use a robust slope estimate from the last 0.5–2.0 s of LTC-derived delays, preferably a median slope or trimmed linear fit.

Practical defaults:

* slope history: **1.0 s**
* jitter history: **0.5 s**
* if slope unavailable: `v0 = 0`

Then predict forward each hop:

```text
D_pred[k] = D_est[k-1] + v_est[k-1] * hop_sec
```

This predicted delay becomes the center of the audio search window.

## 3) Analysis signal path

Do not correlate full-band raw audio directly. Build a dedicated **analysis path** per master/slave pair:

1. downmix to mono
2. DC blocker / high-pass
3. band-limit
4. optional light pre-emphasis
5. optional decimation for analysis

Recommended defaults:

* **high-pass:** 80–120 Hz
* **speech/general scene band:** 150–3500 Hz
* **broader band option:** 120–5000 Hz
* **analysis sample rate:** 12 kHz or 16 kHz
* **decimation:** 4x from 48 kHz is a good default

For a bad camera mic, I would usually run **two analysis paths**:

* **Path A:** band-passed waveform for GCC-PHAT
* **Path B:** onset/energy envelope correlation

Path A is the main estimator. Path B is the sanity check when the waveform is too noisy or spectrally mangled.

## 4) The estimator

Use a **limited-lag GCC-PHAT** centered on `D_pred`, not a global search. GCC-PHAT is appropriate here because PHAT weighting reduces the influence of magnitude coloration and focuses on phase/time alignment. ([speechprocessingbook.aalto.fi][1])

Recommended worker-thread cycle:

* run every **10–20 ms**
* analyze a window of **80–160 ms**
* search only within a residual range around `D_pred`

Good defaults at 48 kHz:

* **hop:** 20 ms
* **window:** 120 ms
* **search half-range:** 8–15 ms
* **maximum search half-range during degraded mode:** 30–40 ms

If you already have stable LTC before dropout, start tight:

```text
R_nominal = max(3 ms, 3 * recent_LTC_jitter_ms)
```

Clamp it, for example:

```text
R_nominal in [5 ms, 15 ms]
R_max in [25 ms, 40 ms]
```

Then only expand the search if confidence stays poor for several hops.

## 5) Use a two-stage estimator

This is the design I would ship.

### Stage 1: coarse residual estimate

Run a cheap correlation on a decimated onset/envelope signal over a wider residual range.

Purpose:

* reject grossly wrong lags
* survive spectral damage
* provide a coarse candidate

### Stage 2: fine residual estimate

Run GCC-PHAT on the band-passed waveform in a narrow range around Stage 1 or around `D_pred`.

Then fit the peak with quadratic interpolation to get sub-sample precision.

This two-stage design is usually more stable than a single raw GCC path on ugly camera audio.

## 6) Confidence scoring

Even if your mode switch already exists, the fallback route still needs its own internal confidence to decide whether to **update**, **coast**, or **freeze**.

Use at least these components:

* **activity gate**: both channels contain usable signal
* **peak prominence**: top correlation peak clearly beats alternatives
* **prediction consistency**: measured residual is close to predicted residual
* **temporal stability**: estimate agrees with recent accepted estimates

A practical score:

```text
conf = w1*activity + w2*peak_prominence + w3*prediction_consistency + w4*stability
```

Recommended acceptance rules:

* `activity == true`
* peak ratio `p1/p2 >= 1.25–1.5`
* `|D_meas - D_pred| <= R`
* median-consistency over last 3–5 accepted hops

Good starting values:

* **peak ratio threshold:** 1.35
* **consistency threshold:** 1.5–2.0 ms
* **accepted history length:** 5 hops

When confidence fails, **do not chase the estimate**. Coast using prediction only.

## 7) The controller: do not feed raw estimates to the delay line

The estimator should feed a small tracker. Use either:

* an **alpha-beta filter**, or
* a 1D Kalman filter

An alpha-beta filter is enough here.

Recommended update:

```text
predict:
    D_pred = D_est + v_est * dt

if conf_good:
    r = clamp(D_meas - D_pred, -R, R)
    D_est = D_pred + alpha * r
    v_est = v_est + beta * r / dt
else:
    D_est = D_pred
    v_est = v_est
```

Good defaults:

* **alpha:** 0.15–0.30
* **beta:** 0.01–0.05
* start with **alpha = 0.20**, **beta = 0.02**

Higher alpha follows audio faster. Lower alpha is safer when mic spacing causes acoustic-TDOA contamination.

## 8) Hard-limit the correction rate

This is the single most important tuning parameter for your use case.

Because the microphones are not co-located, the audio estimator may “see” source-position changes and try to move the delay. That is not recorder drift. So the fallback output must be rate-limited to what the recorder timing can plausibly do.

Define:

```text
max_correction_rate_ms_per_s
```

Start with:

* **0.05–0.20 ms/s** for normal independent recorder clock drift
* **up to 0.50 ms/s** only if your camera really shows larger wander

At 48 kHz:

* 0.10 ms/s ≈ 4.8 samples/s
* 0.50 ms/s ≈ 24 samples/s

A good adaptive default is:

```text
rate_limit = clamp(3 * RMS(recent_LTC_slope_variation), 0.05, 0.50) ms/s
```

This makes the fallback inherit its aggressiveness from how the LTC-tracked system was behaving before dropout.

## 9) The actual delay application

Because your plugin already applies delay to slaves, the fallback should continue to drive that same delay engine, but with two requirements:

### a) You need safety headroom

A delay-only engine can only move within available buffered time. So every slave needs a **base safety latency** large enough to absorb fallback corrections around the last LTC value.

Recommended base safety margin:

* **10–20 ms** for short dropouts and disciplined clocks
* **20–50 ms** if the camera timing is ugly

Conceptually:

```text
applied_delay = base_margin + fallback_delay
```

That gives the fallback room to move both ways without needing “negative delay.”

If that margin changes in a way visible to the host, handle latency reporting through the standard VST latency mechanism. ([Steinberg Media][2])

### b) The delay line must be continuously variable

Do not jump the read head sample-discontinuously.

Use:

* a **fractional delay line** for small moves
* a **short crossfade** for larger corrections

Practical rule:

* if change < **0.25 sample per block**: continuous fractional movement
* if cumulative correction is small but sustained: ramp over **20–100 ms**
* if a correction jump exceeds about **2–8 samples**: switch read taps with a **5–10 ms crossfade**

That prevents clicks while still letting the fallback correct drift.

## 10) Silence and low-information handling

The fallback must be allowed to do nothing.

If the scene contains silence, steady HVAC, wind, or unrelated dominant content, the correct action is usually:

* hold prediction
* keep confidence low
* wait for informative audio

Use an activity gate based on:

* short-term RMS above noise floor
* spectral flux / onset activity
* optionally speech-band energy overlap

Recommended defaults:

* noise-floor tracking time constant: **1–3 s**
* update only when signal is at least **6–10 dB** above the tracked floor

This keeps the fallback from “locking” to noise.

## 11) Multi-band robustness for the bad camera mic

For a 360-camera mic, a single broadband correlation can get fooled by wind, codec trash, or stitching artifacts. The strongest upgrade is a **3-band estimator**:

* 150–500 Hz
* 500–1500 Hz
* 1500–4000 Hz

Run the same residual estimator per band, compute a confidence per band, and take the **weighted median lag**.

That is much more robust than trusting one full-band peak.

## 12) Parameter set I would start with

For dialogue / general scene audio:

* analysis sample rate: **12 kHz**
* hop: **20 ms**
* window: **120 ms**
* overlap: implicit via hop/window
* band-pass: **150–3500 Hz**
* nominal search half-range: **10 ms**
* max search half-range: **30 ms**
* alpha: **0.20**
* beta: **0.02**
* peak ratio threshold: **1.35**
* prediction-consistency threshold: **1.5 ms**
* activity gate: **8 dB above tracked noise floor**
* max correction rate: **0.10 ms/s**
* base safety latency: **20 ms**
* large-correction crossfade: **7 ms**

For noisier, more unstable camera audio:

* keep hop at **20 ms**
* increase window to **150–200 ms**
* keep search nominally tight, **12–15 ms**
* lower alpha to **0.12–0.18**
* keep beta small, **0.01–0.02**
* use multi-band weighted median
* raise base safety latency to **30–40 ms**

## 13) What not to do

Do not:

* run a full unrestricted cross-correlation search every hop
* update from every transient
* let the fallback change delay faster than plausible recorder drift
* chain slave-to-slave; always estimate each slave against the master
* do heavy FFT/correlation or memory churn in the audio callback, per Steinberg’s real-time guidance ([Steinberg Media][2])

## 14) Minimal implementation sketch

```text
on LTC dropout:
    D_est = last_LTC_delay
    v_est = robust_slope(last_1s_LTC_delay_history)
    mode = FALLBACK

every audio block:
    push preprocessed mono master/slave samples to lock-free ring
    audio thread reads atomic D_target and applies variable delay

worker every 20 ms:
    D_pred = D_est + v_est * dt

    coarse = envelope_corr(master, slave, center=D_pred, range=R)
    fine   = gcc_phat(master, slave, center=coarse_or_D_pred, range=R_fine)

    conf = score(activity, peak_ratio, |fine-D_pred|, stability)

    if conf good:
        r = clamp(fine - D_pred, -R, R)
        D_est = D_pred + alpha * r
        v_est = v_est + beta * r / dt
    else:
        D_est = D_pred

    D_est = rate_limit(D_est, max_correction_rate)
    D_target = smooth_and_crossfade_safe(D_est)
    publish_atomic(D_target, conf)
```

## 15) The main tuning loop

Tune in this order:

1. **Set base safety latency** so the delay line has room to move.
2. **Set search range** from real LTC behavior before dropout.
3. **Set correction-rate limit** from plausible recorder drift, not from what the correlation wants.
4. **Only then** tune alpha/beta.
5. Add **multi-band voting** if the camera mic is still unstable.
6. Increase window length only when false updates remain a problem.

If you want, I can turn this into C++-level pseudocode for the worker estimator and the variable-delay engine, including the ring-buffer layout and the exact GCC-PHAT peak-picking steps.

[1]: https://speechprocessingbook.aalto.fi/Enhancement/tdoa.html "11.8.3. Time-Delay of Arrival (TDoA) and Direction of Arrival (DoA) Estimation - Introduction to Speech Processing"
[2]: https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Processing.html "Processing - VST 3 Developer Portal"
