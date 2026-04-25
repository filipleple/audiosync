/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <math.h>
#include <deque>
#include <array>
#include <climits>
#include "SharedGroupMemory.h"

//==============================================================================

class tc_data
{
public:
	int hrs = 0;
	int mnts = 0;
	int scnds = 0;
	int frms = 0;

	std::array<int, 80> buf{};

	const long double threshenv = std::pow(2.7182818284, (-1 / (0.1 * 44100)));

	long double threshold = 1.0;
	long double lastsign = 1.0;

	int sillen = 0;
	int bufpos = 0;
	int gotbit = -1;
	int syncpos = -1;
	int syncstate = 0;

	long double otm1 = 0.0;
	long double itm1 = 0.0;

	const long double minthresh = 0.0001;
	const int bufSize = 80;

	float pulsesize = 9.1875;

	// Absolute stream sample position at which the most recent LTC frame was
	// decoded.  Set by handleTimecode() on each successful sync-word match.
	// Used for sample-accurate cross-instance delay measurement (see processBlock).
	int64_t last_decode_sample = 0;

	int new_time = 0;
	int old_time = 0;

	int timecode_counter = 0;

	// delay
	std::deque<float> delay_buf;
	std::deque<float> const_buf;
	const int const_buf_size = 500000;
	bool active_delay = false;
	size_t delay_size = 0;

	// === Quality Scoring ===
	// Window accumulators (~0.5s at 44100 Hz)
	int w_sample_count = 0;
	const int W_SIZE = 22050;

	int w_frames_decoded = 0;
	int w_continuity_ok = 0;
	int w_prev_frms = -1;

	float w_pulse_dev_sum = 0.0f;
	float w_sillen_sum = 0.0f;
	int w_pulse_dev_count = 0;

	int w_trans_valid = 0;
	int w_trans_total = 0;
	int w_samples_since_trans = 0;

	float w_env_sum = 0.0f;
	int w_env_count = 0;

	int w_sync_hits = 0;

	// Computed metrics (updated at each window boundary)
	float q_lock_ratio = 0.0f;
	float q_continuity_score = 0.0f;
	float q_pulse_consistency = 0.0f;
	float q_transition_reliability = 0.0f;
	float q_signal_strength = 0.0f;
	float q_sync_word_rate = 0.0f;
	float q_fps_plausibility = 0.0f;
	float Q_LTC = 0.0f;
	float estimated_fps = 0.0f;

	enum class LTCState : uint8_t { FAIL = 0, SUSPECT = 1, VALID = 2 };
	LTCState ltc_state = LTCState::FAIL;

	// === Temporal-coherence gate ===
	// Once the decoder has seen LOCK_N consecutive frames that step forward by
	// the expected 1–3 frames, it enters a "locked" state.  While locked, any
	// new BCD-valid frame whose timecode is outside [-1, +3] frames relative
	// to the last accepted one is rejected without mutating hrs/mnts/scnds/
	// frms/last_decode_sample.  This catches speech transients that happen to
	// satisfy the sync-word + BCD range checks but decode to incoherent times.
	// After UNLOCK_M consecutive rejects the gate drops the lock so a genuine
	// re-acquisition after a real gap can still take hold.
	static constexpr int  LOCK_N    = 10;   // ~0.33 s of clean LTC at 30 fps
	static constexpr int  UNLOCK_M  = 20;   // ~0.67 s of sustained rejects
	bool    locked                     = false;
	int     consec_good_frames         = 0;
	int     consec_reject_since_lock   = 0;
	int64_t last_accepted_tc_ms        = -1;   // −1 = no previous accepted frame

	// Persistent counters (not reset by clear())
	int decoder_reset_count = 0;
	int rejected_frames_count = 0;
	bool fallback_requested = false;

public:
	void computeAndResetWindow(double srate, double current_fps)
	{
		float expected_frames = (float)(W_SIZE * current_fps / srate);

		q_lock_ratio = expected_frames > 0.0f
			? std::min(1.0f, (float)w_frames_decoded / expected_frames)
			: 0.0f;

		q_continuity_score = w_frames_decoded > 1
			? std::min(1.0f, (float)w_continuity_ok / (float)(w_frames_decoded - 1))
			: 0.0f;

		if (w_pulse_dev_count > 0)
		{
			q_pulse_consistency = std::max(0.0f, 1.0f - (w_pulse_dev_sum / (float)w_pulse_dev_count) * 3.0f);
			float empirical_half_pulse = (w_sillen_sum / (float)w_pulse_dev_count) / 2.0f;
			if (empirical_half_pulse > 0.0f)
			{
				estimated_fps = (float)(srate / ((double)empirical_half_pulse * 160.0));
				float diff = std::min(std::abs(estimated_fps - 25.0f), std::abs(estimated_fps - 30.0f));
				q_fps_plausibility = std::max(0.0f, 1.0f - diff / 5.0f);
			}
			else
			{
				q_fps_plausibility = 0.0f;
			}
		}
		else
		{
			q_pulse_consistency = 0.0f;
			q_fps_plausibility = 0.0f;
		}

		q_transition_reliability = w_trans_total > 0
			? (float)w_trans_valid / (float)w_trans_total
			: 0.0f;

		q_signal_strength = w_env_count > 0
			? std::min(1.0f, (w_env_sum / (float)w_env_count) / (float)(minthresh * 100.0L))
			: 0.0f;

		q_sync_word_rate = expected_frames > 0.0f
			? std::min(1.0f, (float)w_sync_hits / expected_frames)
			: 0.0f;

		// Weighted composite score
		Q_LTC = 0.25f * q_lock_ratio
		      + 0.20f * q_continuity_score
		      + 0.15f * q_pulse_consistency
		      + 0.15f * q_transition_reliability
		      + 0.10f * q_signal_strength
		      + 0.10f * q_sync_word_rate
		      + 0.05f * q_fps_plausibility;

		if (Q_LTC > 0.8f)
			ltc_state = LTCState::VALID;
		else if (Q_LTC > 0.5f)
			ltc_state = LTCState::SUSPECT;
		else
			ltc_state = LTCState::FAIL;

		fallback_requested = (ltc_state == LTCState::FAIL);

		// Reset window accumulators (w_prev_frms and w_samples_since_trans persist)
		w_sample_count = 0;
		w_frames_decoded = 0;
		w_continuity_ok = 0;
		w_pulse_dev_sum = 0.0f;
		w_sillen_sum = 0.0f;
		w_pulse_dev_count = 0;
		w_trans_valid = 0;
		w_trans_total = 0;
		w_env_sum = 0.0f;
		w_env_count = 0;
		w_sync_hits = 0;
	}

	inline void clear()
	{
		delay_buf.clear();
		active_delay = false;
		delay_size = 0;
		timecode_counter = 0;
		new_time = 0;
		old_time = 0;
		itm1 = 0.0;
		otm1 = 0.0;
		syncstate = 0;
		syncpos = -1;
		gotbit = -1;
		bufpos = 0;
		sillen = 0;
		lastsign = 1.0;
		threshold = 1.0;
		hrs = 0;
		mnts = 0;
		scnds = 0;
		frms = 0;

		// Reset diagnostic window
		w_sample_count = 0;
		w_frames_decoded = 0;
		w_continuity_ok = 0;
		w_prev_frms = -1;
		w_pulse_dev_sum = 0.0f;
		w_sillen_sum = 0.0f;
		w_pulse_dev_count = 0;
		w_trans_valid = 0;
		w_trans_total = 0;
		w_samples_since_trans = 0;
		w_env_sum = 0.0f;
		w_env_count = 0;
		w_sync_hits = 0;
		Q_LTC = 0.0f;
		ltc_state = LTCState::FAIL;

		// Coherence gate: force re-lock from scratch after any decoder reset
		// so the first frame after recovery is accepted without being checked
		// against the pre-reset timecode.
		locked                   = false;
		consec_good_frames       = 0;
		consec_reject_since_lock = 0;
		last_accepted_tc_ms      = -1;

		++decoder_reset_count;
	}
};

// ============================================================================
// Audio-domain fallback estimator state
// ============================================================================

struct AudioFallbackState
{
	// Parameters - set by init(), derived from sample rate
	int    hopSamples   = 441;
	int    windowFrames = 200;
	int    lagRange     = 150;
	int    refreshEvery = 20;
	double hopMs        = 10.0;

	// Cascaded 1-pole bandpass (applied per-sample before energy accumulation).
	// HPF @ 100 Hz strips DC and carrier rumble so novelty responds to
	// transients/speech rather than low-frequency drift.
	// LPF @ 3500 Hz removes wind, HVAC, and codec artefacts that would
	// otherwise add uncorrelated HF energy to the novelty envelope.
	float hpfPrevX1 = 0.0f, hpfPrevY1 = 0.0f;
	float hpfPrevX2 = 0.0f, hpfPrevY2 = 0.0f;
	float hpfAlpha  = 0.9872f;  // overwritten in init() from sampleRate; 100 Hz @ 48 kHz

	float lpfPrev1  = 0.0f;
	float lpfPrev2  = 0.0f;
	float lpfAlpha  = 0.3142f;  // overwritten in init() from sampleRate; 3500 Hz @ 48 kHz

	float applyHPF(float x, float& prevX, float& prevY) const noexcept
	{
		float y = hpfAlpha * (prevY + x - prevX);
		prevX = x;
		prevY = y;
		return y;
	}

	float applyLPF(float x, float& prev) const noexcept
	{
		float y = prev + lpfAlpha * (x - prev);
		prev = y;
		return y;
	}

	// Activity gate: slow noise-floor follower + dB threshold.
	// NCC estimation is suppressed when both channels are below the gate so the
	// estimator doesn't lock onto noise during silence or steady-state content.
	float noiseFloor1  = 1e-8f;
	float noiseFloor2  = 1e-8f;
	bool  activityGate = false;
	// Asymmetric noise-floor follower.
	// RISE: slow (τ ≈ 2 s at 10 ms/hop) - prevents gate from fluttering open on
	//       transient spikes.
	// FALL: fast (τ ≈ 50 ms at 10 ms/hop) - floor drops quickly after a loud
	//       signal (e.g. LTC carrier) stops, so quieter speech can trip the gate
	//       within ~150-250 ms instead of waiting 5-15 s.
	static constexpr float NOISE_FLOOR_TC_RISE = 0.995f;
	static constexpr float NOISE_FLOOR_TC_FALL = 0.80f;
	static constexpr float ACTIVITY_GATE_DB    = 8.0f;   // dB above tracked noise floor

	// Per-hop energy accumulators
	float energyAcc1       = 0.0f;
	float energyAcc2       = 0.0f;
	float prevEnergy1      = 0.0f;
	float prevEnergy2      = 0.0f;
	int   hopSampleCounter = 0;
	int   hopsSinceRefresh = 0;

	// Novelty circular buffers + pre-allocated linearisation scratch
	std::vector<float> novelty1;
	std::vector<float> novelty2;
	std::vector<float> linBuf1;
	std::vector<float> linBuf2;
	int writePos     = 0;
	int framesFilled = 0;

	// Slave mode: linearised master reference novelty copied from SM every ~0.1s.
	// When hasMasterRef is true, estimateAudioFallbackOffset() uses this instead
	// of linearising novelty2, so the NCC compares slave transients vs master transients.
	// masterFramesFilled tracks how many of the newest-aligned frames are real
	// data (the oldest-aligned tail is zero when master's ring isn't full).
	std::vector<float> masterNoveltyRef;
	bool hasMasterRef       = false;
	int  masterFramesFilled = 0;

	// Anchor: last LTC-confirmed offset used to centre the narrow NCC search.
	// Set by fuseLtcAndAudioFallback() whenever ltcOk is true.
	bool   hasAnchor             = false;
	double anchorMs              = 0.0;
	int    anchorHops            = 0;     // round(anchorMs / hopMs), cached
	bool   lastEstimateAnchored  = false; // set by estimateAudioFallbackOffset, read by fuse

	// Half-width of the narrow lag search in hops (±300 ms at 10 ms/hop).
	// Must satisfy: NARROW_HALF < ANCHORED_WIN/2 to keep meaningful overlap.
	static constexpr int NARROW_HALF = 30;

	// Effective NCC window in anchored mode (frames).
	// Using only the most recent ANCHORED_WIN frames (1.2 s) means the estimator
	// reacts within ~1.2 s after a manual track shift.
	// Must satisfy: ANCHORED_WIN > 2 * NARROW_HALF (= 60) to maintain adequate
	// overlap at ±NARROW_HALF lag.
	static constexpr int ANCHORED_WIN = 120;

	// Effective NCC window in wide (no-anchor) mode (frames).
	// Kept at 4 s so cold-start acquisition still reacts in a few seconds,
	// independent of how long the full novelty ring is.
	static constexpr int WIDE_WIN = 400;

	// Estimation results
	double deltaAudMs  = 0.0;
	double confAud     = 0.0;
	double peakCorr    = 0.0;
	double secondPeak  = 0.0;
	int    bestLag     = 0;
	int    prevBestLag = INT_MAX;
	int    stableCount = 0;
	bool   valid       = false;

	void init(double sampleRate)
	{
		hopMs        = 10.0;
		hopSamples   = (int)std::round(sampleRate * 0.010);
		// 20 s novelty ring so the slave can hold enough master history for
		// anchored NCC to reach offsets up to ±(N − NARROW_HALF − 1) ≈ ±19.69 s.
		// Keep in lockstep with SharedGroupMemory.h:MASTER_NOV_REF_SIZE.
		windowFrames = MASTER_NOV_REF_SIZE;
		lagRange     = 200;   // ±2 s wide search

		// HPF: 1-pole IIR, cutoff 100 Hz.  alpha = 1 / (1 + 2π·fc/sr)
		hpfAlpha = (float)(1.0 / (1.0 + 2.0 * 3.14159265358979323846 * 100.0 / sampleRate));
		// LPF: 1-pole IIR, cutoff 3500 Hz.  alpha = 2π·fc / (sr + 2π·fc)
		lpfAlpha = (float)(2.0 * 3.14159265358979323846 * 3500.0
		                 / (sampleRate + 2.0 * 3.14159265358979323846 * 3500.0));
		refreshEvery = 20;
		novelty1.assign(windowFrames, 0.0f);
		novelty2.assign(windowFrames, 0.0f);
		linBuf1.assign(windowFrames, 0.0f);
		linBuf2.assign(windowFrames, 0.0f);
		masterNoveltyRef.assign(windowFrames, 0.0f);
		reset();
	}

	void reset()
	{
		energyAcc1 = energyAcc2 = prevEnergy1 = prevEnergy2 = 0.0f;
		hopSampleCounter = hopsSinceRefresh = writePos = framesFilled = 0;
		deltaAudMs = confAud = peakCorr = secondPeak = 0.0;
		bestLag = 0;
		prevBestLag = INT_MAX;
		stableCount = 0;
		valid = false;
		hasMasterRef       = false;
		masterFramesFilled = 0;
		hasAnchor = false;
		anchorMs  = 0.0;
		anchorHops = 0;
		lastEstimateAnchored = false;
		hpfPrevX1 = hpfPrevY1 = hpfPrevX2 = hpfPrevY2 = 0.0f;
		lpfPrev1 = lpfPrev2 = 0.0f;
		noiseFloor1 = noiseFloor2 = 1e-8f;
		activityGate = false;
	}

	// Clears the novelty ring and all filter state so the next window rebuilds
	// from scratch.  Called on the LTC→FAIL edge: the 2.5 s transition hold in
	// fuseLtcAndAudioFallback previously suppressed *activation* of fallback
	// but the ring kept ingesting carrier-tail transients the whole time, so
	// when fallback finally armed the most-recent ANCHORED_WIN frames still
	// contained hybrid carrier+scene novelty that biased the NCC peak.  Purging
	// at the edge forces the post-fade window to be scene-only.
	void purgeNoveltyRing()
	{
		std::fill(novelty1.begin(), novelty1.end(), 0.0f);
		std::fill(novelty2.begin(), novelty2.end(), 0.0f);
		energyAcc1 = energyAcc2 = 0.0f;
		prevEnergy1 = prevEnergy2 = 0.0f;
		hopSampleCounter = 0;
		hopsSinceRefresh = 0;
		writePos         = 0;
		framesFilled     = 0;
		hpfPrevX1 = hpfPrevY1 = hpfPrevX2 = hpfPrevY2 = 0.0f;
		lpfPrev1 = lpfPrev2 = 0.0f;
		noiseFloor1 = noiseFloor2 = 1e-8f;
		activityGate = false;
		stableCount  = 0;
		prevBestLag  = INT_MAX;
		valid        = false;
		// Invalidate cached master reference: the stale linearised buffer
		// held pre-purge master novelty that no longer aligns with the
		// freshly-zeroed slave ring.  It will be repopulated once master's
		// own post-purge ring passes WIDE_WIN.
		hasMasterRef       = false;
		masterFramesFilled = 0;
	}
};

struct FusionState
{
	enum class Source { None, LTC, AudioFallback };
	Source source         = Source::None;
	double selectedMs     = 0.0;
	double selectedConf   = 0.0;
	bool   fallbackActive = false;
};

// ============================================================================
// Alpha-beta tracker: smooths raw NCC estimates before they reach the delay
// line and enforces a velocity cap so the fallback can't chase moving sources.
// ============================================================================

struct AlphaBetaState
{
	double  estMs        = 0.0;   // current delay estimate (ms)
	double  velMsPerS    = 0.0;   // velocity / drift estimate (ms/s)
	bool    initialized  = false;
	int64_t lastUpdateMs = 0;     // juce::Time::currentTimeMillis() at last update

	static constexpr double ALPHA            = 0.20;
	static constexpr double BETA             = 0.02;
	// §8 rate limit.  Design recommends 0.05–0.20 ms/s with 0.10 as start;
	// 0.10 rejects acoustic-TDOA contamination from moving sources more
	// aggressively while still covering realistic recorder-clock drift.
	static constexpr double MAX_VEL_MS_PER_S = 0.10;

	// Call once at LTC→fallback transition to prime from the last good LTC value.
	void seed(double d0Ms)
	{
		estMs        = d0Ms;
		velMsPerS    = 0.0;
		initialized  = true;
		lastUpdateMs = juce::Time::currentTimeMillis();
	}

	// Called every NCC refresh cycle (~200 ms).
	// measuredMs    - NCC/fusion output (ignored when !measuredValid).
	// measuredValid - false means coast: predict forward on current velocity.
	void update(double measuredMs, bool measuredValid)
	{
		const int64_t now = juce::Time::currentTimeMillis();
		const double  dt  = std::max(0.001, (double)(now - lastUpdateMs) * 0.001);
		lastUpdateMs = now;

		const double predicted = estMs + velMsPerS * dt;

		if (measuredValid)
		{
			const double r = measuredMs - predicted;
			estMs     = predicted + ALPHA * r;
			velMsPerS = velMsPerS + BETA * r / dt;
		}
		else
		{
			estMs = predicted;
			// velocity unchanged - coast
		}

		// §8: clamp velocity so the fallback can't track acoustic source movement
		velMsPerS = std::max(-MAX_VEL_MS_PER_S,
		            std::min( MAX_VEL_MS_PER_S, velMsPerS));
	}

	void reset()
	{
		estMs = velMsPerS = 0.0;
		initialized  = false;
		lastUpdateMs = 0;
	}
};

// ============================================================================

enum class PluginMode { Master = 0, Slave = 1 };

class AutoSyncAudioProcessor : public juce::AudioProcessor
#if JucePlugin_Enable_ARA
                             , public juce::AudioProcessorARAExtension
#endif
{
public:
	// =========================================================================
	// Session configuration (persisted via getStateInformation)
	// =========================================================================
	PluginMode   pluginMode  = PluginMode::Slave;
	juce::String groupName   = "group1";  // shared across master + all slaves
	int          slotId      = 1;         // slave slot index 1-8 (ignored by master)
	int          ltcChannel  = 0;         // 0 = Left, 1 = Right
	juce::String slotLabel   = "";        // free-text label shown in master dashboard

	// Slave runtime state (written by audio thread, read by GUI timer)
	bool    masterValid       = false;    // true when master SM data is fresh
	bool    holding           = false;    // true when delay is frozen (stale master)
	int64_t lastMasterWriteMs = 0;        // juce::Time::currentTimeMillis() of last SM read

	// Called from the editor when the group name is changed at runtime.
	// Closes the current segment and opens (or creates) the new one.
	// Safe to call from the message thread between playback sessions;
	// do NOT call while processBlock is running.
	void reopenShm()
	{
		shm.close();
		if (!shm.open(groupName.toStdString()))
			juce::Logger::writeToLog("AUTOSYNC: reopenShm() failed for group \"" + groupName + "\"");
	}

	double currentSampleRate = 44100.0;
	std::string tc = "--:--:--:--";
	std::string output_c2 = "--:--:--:--";
	std::string delay_ms = "--";
	std::string input_ch1 = "--:--:--:--";
	std::string input_ch2 = "--:--:--:--";
	std::string o_delay_ms = "--";

	double prev_ms = 0;
	int prev_frames = 0;

	bool active_delay = false;
	double by_slider = 0;

	juce::AudioParameterFloat* myParameter;

	double d_ms = 0;

	int fps = 30;

	// === Diagnostic outputs (written by audio thread, read by GUI timer) ===
	float ch1_Q_LTC = 0.0f;
	float ch2_Q_LTC = 0.0f;
	int ch1_ltc_state = 0;  // 0=FAIL, 1=SUSPECT, 2=VALID
	int ch2_ltc_state = 0;
	float ch1_estimated_fps = 0.0f;
	float ch2_estimated_fps = 0.0f;
	int ch1_decoder_resets = 0;
	int ch2_decoder_resets = 0;
	int ch1_rejected_frames = 0;
	int ch2_rejected_frames = 0;
	double dt_deviation = 0.0;
	double drift_per_s = 0.0;
	bool drift_suspected = false;
	bool fallback_requested = false;

	// Audio fallback + fusion diagnostic outputs
	double aud_deltaMs        = 0.0;
	double aud_conf           = 0.0;
	int    aud_fusionSource   = 0;    // 0=None, 1=LTC, 2=AudioFallback
	double aud_activeDelayMs  = 0.0;  // offset actually programmed into delay engine

	// Shared-memory diagnostics (master mode)
	uint32_t shmWriteCount = 0;       // incremented on every SM write; read by GUI timer

public:
	//==============================================================================
	AutoSyncAudioProcessor();
	~AutoSyncAudioProcessor() override;

	//==============================================================================
	void prepareToPlay(double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;

#ifndef JucePlugin_PreferredChannelConfigurations
	bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

	void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

	//==============================================================================
	juce::AudioProcessorEditor* createEditor() override;
	bool hasEditor() const override;

	//==============================================================================
	const juce::String getName() const override;

	bool acceptsMidi() const override;
	bool producesMidi() const override;
	bool isMidiEffect() const override;
	double getTailLengthSeconds() const override;

	//==============================================================================
	int getNumPrograms() override;
	int getCurrentProgram() override;
	void setCurrentProgram(int index) override;
	const juce::String getProgramName(int index) override;
	void changeProgramName(int index, const juce::String& newName) override;

	//==============================================================================
	void getStateInformation(juce::MemoryBlock& destData) override;
	void setStateInformation(const void* data, int sizeInBytes) override;

private:
	inline void processTimeCode(const float& sample, tc_data& channel, std::string& msg,
		const int& index, const float& sr, const int& sl, int64_t abs_pos = 0);

	void pushAudioAnalysisSample(float ch1, float ch2);
	void estimateAudioFallbackOffset();
	void fuseLtcAndAudioFallback();
	void writeMasterSlot();   // master mode only: snapshot LTC state + novelty → shm

private:
	SharedGroupMemory shm;   // opened in prepareToPlay, closed in releaseResources

	tc_data chnl1;
	tc_data chnl2;
	tc_data chnl1_in;
	tc_data chnl2_in;

	// Δt rolling window for drift / channel_agreement detection
	std::deque<double> dt_history;
	int dt_sample_counter = 0;
	int drift_confirm_count = 0;   // consecutive windows where |drift_per_s| > threshold

	AudioFallbackState audFallback;
	FusionState        fusion;
	AlphaBetaState     ab;
	FusionState::Source prevFusionSource = FusionState::Source::None;
	double  activeDelayMs     = 0.0;   // offset currently programmed into the delay engine
	int64_t anchorTimestampMs = 0;     // juce::Time::currentTimeMillis() when anchor last set
	int64_t masterNovAnchorSample = 0; // abs sample pos when master last wrote novelty to SM
	// Max time the anchor may stand without either a fresh LTC confirmation
	// or a valid anchored NCC hit.  A coherent audio fallback refreshes the
	// timestamp each cycle (see fuseLtcAndAudioFallback), so this only fires
	// when both LTC and NCC have been dark for this long - i.e. the scene
	// genuinely went silent.  30 s buys a slow source to come back without
	// losing alignment; beyond that, we fall back to wide re-acquisition.
	static constexpr double ANCHOR_MAX_AGE_MS = 30000.0;

	// Hysteresis state for d_ms updates in slave SM read.
	// Large jumps in the computed delay require JUMP_CONFIRMS_NEEDED consecutive
	// consistent readings before being applied, so a single garbage LTC frame
	// (e.g. from an abrupt cut) cannot immediately derail the delay engine.
	double  d_ms_pending         = 0.0;
	int     d_ms_pending_count   = 0;
	static constexpr double D_MS_JUMP_THRESH_MS = 500.0;  // jump magnitude that triggers doubt
	static constexpr int    D_MS_JUMP_CONFIRMS  = 3;      // consecutive readings needed to accept

	// Set when any jump candidate is detected; cleared after 5 consecutive clean
	// reads.  While true, the NCC anchor is not refreshed from d_ms so that a
	// corrupted value that slipped through the confirm count cannot become the
	// alpha-beta seed at the LTC→fallback transition.
	bool d_ms_recently_jumped = false;
	int  d_ms_clean_count     = 0;

	// Timestamp (juce::Time::currentTimeMillis) at which chnl1_in last transitioned
	// to FAIL.  Used by fuseLtcAndAudioFallback() to enforce a hold period before
	// activating audio fallback, giving the LTC carrier time to clear from the
	// novelty buffer.
	int64_t ltcFadeTimestampMs      = 0;
	tc_data::LTCState prevChnl1InLtcState = tc_data::LTCState::FAIL;

	// Monotonically increasing sample counter, reset on prepareToPlay.
	// Used as the absolute sample position passed to handleTimecode so that
	// master and slave decode-event positions can be compared sample-accurately.
	int64_t totalSamplesProcessed = 0;

	// Shared DAW-timeline sample position at the current per-sample step
	// (bufferStartSample + i inside processBlock's loop).  Used where the
	// master and slave must agree on a common clock - specifically the SM
	// staleness correction in estimateAudioFallbackOffset and the master
	// write of m.nov_anchor_sample.  Falls back to totalSamplesProcessed
	// when no playhead is available, same as bufferStartSample.
	int64_t currentDawSample = 0;

	//==============================================================================
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoSyncAudioProcessor)
	std::unique_ptr<juce::FileLogger> fileLogger;
};
