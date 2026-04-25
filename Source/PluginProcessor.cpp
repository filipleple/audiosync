/*
  ==============================================================================

	This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <deque>
#include <memory>

//==============================================================================
static int delay_frames = 0;

inline void delay(float* writePtr, const int& index, tc_data& data)
{
	if (data.delay_size < data.const_buf.size())
	{
		writePtr[index] = data.const_buf[data.const_buf.size() - data.delay_size];
		return;
	}
	if (data.delay_buf.size() < data.delay_size)
	{
		float tmp = writePtr[index];
		data.delay_buf.push_back(tmp);
		writePtr[index] = 0;
	}
	else
	{
		float tmp = writePtr[index];
		data.delay_buf.push_back(tmp);
		float tmp2 = data.delay_buf[0];
		writePtr[index] = tmp2;
		data.delay_buf.pop_front();
	}
}

inline double calc_delay(tc_data& data1, tc_data& data2, int fps = 30)
{
	int h1 = data1.hrs;
	int h2 = data2.hrs;
	int m1 = data1.mnts;
	int m2 = data2.mnts;
	int s1 = data1.scnds;
	int s2 = data2.scnds;
	int f1 = data1.frms;
	int f2 = data2.frms;

	int time1_in_frames = fps * 3600 * h1 + fps * 60 * m1 + fps * s1 + f1;
	int time2_in_frames = fps * 3600 * h2 + fps * 60 * m2 + fps * s2 + f2;

	if (time1_in_frames == 0 || time2_in_frames == 0)
	{
		delay_frames = 0;
		return 0;
	}

	const double delay_ms = (time2_in_frames - time1_in_frames) * 1000 / fps;

	// Hard sanity cap: no realistic synchronisation scenario requires more than
	// 1 hour of offset.  BCD validation in handleTimecode should prevent garbage
	// frames reaching here, but this is a last-resort guard.
	static constexpr double MAX_SANE_DELAY_MS = 3'600'000.0;
	if (std::abs(delay_ms) > MAX_SANE_DELAY_MS)
	{
		++data2.rejected_frames_count;
		return 0;
	}

	// Original code had an operator-precedence bug:
	//   counter1>20 || counter2>20 && |d|>10000
	// was parsed as:
	//   counter1>20 || (counter2>20 && |d|>10000)
	// so counter1>20 alone bypassed the magnitude check entirely.
	if ((data1.timecode_counter > 20 || data2.timecode_counter > 20) && std::abs(delay_ms) > 10000)
	{
		delay_frames = time2_in_frames - time1_in_frames;
		return delay_ms;
	}

	if (delay_ms > 10000)
	{
		++data2.timecode_counter;
		++data2.rejected_frames_count;
		return 0;
	}

	if (delay_ms < -10000)
	{
		++data1.timecode_counter;
		++data1.rejected_frames_count;
		return 0;
	}

	data1.timecode_counter = 0;
	data2.timecode_counter = 0;

	delay_frames = time2_in_frames - time1_in_frames;
	return delay_ms;
}

inline void handleTimecode(const long double& sample, tc_data& data, const int& srate, const int& slider = 0, int64_t abs_pos = 0)
{
	static const double frates[] = {30, 24, 25, 30000.0 / 1001};

	data.pulsesize = srate / frates[slider] / 160;

	// Remove DC offset
	data.otm1 = 0.999 * data.otm1 + sample - data.itm1;
	data.itm1 = sample;
	const long double s = data.otm1;

	++data.sillen;
	++data.w_samples_since_trans;

	if (data.sillen > data.pulsesize * 2.2)
	{
		data.syncpos = -1;
		data.sillen = 0;
		data.gotbit = -1;
		data.syncstate = 1;
		data.w_samples_since_trans = 0;
		// Clear the bit buffer so that partial-frame bits from a cut LTC signal
		// cannot combine with subsequent speech transitions to form a spurious
		// sync-word match with invalid timecode values.
		data.buf.fill(0);
		data.bufpos = 0;
	}

	data.threshold = data.threshold * data.threshenv + std::abs(s) * (1 - data.threshenv);
	if (data.threshold < data.minthresh)
		data.threshold = data.minthresh;

	// Accumulate envelope for signal_strength
	data.w_env_sum += (float)data.threshold;
	++data.w_env_count;

	if ((s < -data.threshold * 0.8 && data.lastsign > 0) || (s > data.threshold * 0.8 && data.lastsign < 0))
	{
		data.lastsign *= -1;

		// Transition reliability: inter-transition interval
		++data.w_trans_total;
		float interval = (float)data.w_samples_since_trans;
		if (interval >= data.pulsesize * 0.4f && interval <= data.pulsesize * 2.5f)
			++data.w_trans_valid;
		data.w_samples_since_trans = 0;

		++data.gotbit;
		if (data.sillen > data.pulsesize * 1.8)
		{
			// Pulse consistency: sillen at bit commit should be ~2*pulsesize
			float dev = std::abs((float)data.sillen - 2.0f * data.pulsesize) / (2.0f * data.pulsesize);
			data.w_pulse_dev_sum += dev;
			data.w_sillen_sum += (float)data.sillen;
			++data.w_pulse_dev_count;

			data.gotbit = std::min(data.gotbit, 1);
			data.sillen = 0;

			data.buf[data.bufpos] = data.gotbit;
			if (++data.bufpos >= 80)
				data.bufpos = 0;

			if (data.syncpos >= 0)
				data.syncpos++;

			if (data.syncpos < 0 || data.syncpos >= 80)
			{
				data.syncpos = -1;

				if (
					data.buf[(data.bufpos + 64) % 80] == 0 &&
					data.buf[(data.bufpos + 65) % 80] == 0 &&
					data.buf[(data.bufpos + 66) % 80] == 1 &&
					data.buf[(data.bufpos + 67) % 80] == 1 &&
					data.buf[(data.bufpos + 68) % 80] == 1 &&
					data.buf[(data.bufpos + 69) % 80] == 1 &&
					data.buf[(data.bufpos + 70) % 80] == 1 &&
					data.buf[(data.bufpos + 71) % 80] == 1 &&
					data.buf[(data.bufpos + 72) % 80] == 1 &&
					data.buf[(data.bufpos + 73) % 80] == 1 &&
					data.buf[(data.bufpos + 74) % 80] == 1 &&
					data.buf[(data.bufpos + 75) % 80] == 1 &&
					data.buf[(data.bufpos + 76) % 80] == 1 &&
					data.buf[(data.bufpos + 77) % 80] == 1 &&
					data.buf[(data.bufpos + 78) % 80] == 0 &&
					data.buf[(data.bufpos + 79) % 80] == 1
				)
				{
					data.frms = data.buf[(data.bufpos + 0) % 80] +
						data.buf[(data.bufpos + 1) % 80] * 2 +
						data.buf[(data.bufpos + 2) % 80] * 4 +
						data.buf[(data.bufpos + 3) % 80] * 8 +
						10 * (
							data.buf[(data.bufpos + 8) % 80] +
							data.buf[(data.bufpos + 9) % 80] * 2
						);

					data.scnds = data.buf[(data.bufpos + 16) % 80] +
						data.buf[(data.bufpos + 17) % 80] * 2 +
						data.buf[(data.bufpos + 18) % 80] * 4 +
						data.buf[(data.bufpos + 19) % 80] * 8 +
						10 * (
							data.buf[(data.bufpos + 24) % 80] +
							data.buf[(data.bufpos + 25) % 80] * 2 +
							data.buf[(data.bufpos + 26) % 80] * 4
						);

					data.mnts = data.buf[(data.bufpos + 32) % 80] +
						data.buf[(data.bufpos + 33) % 80] * 2 +
						data.buf[(data.bufpos + 34) % 80] * 4 +
						data.buf[(data.bufpos + 35) % 80] * 8 +
						10 * (
							data.buf[(data.bufpos + 40) % 80] +
							data.buf[(data.bufpos + 41) % 80] * 2 +
							data.buf[(data.bufpos + 42) % 80] * 4
						);

					data.hrs = data.buf[(data.bufpos + 48) % 80] +
						data.buf[(data.bufpos + 49) % 80] * 2 +
						data.buf[(data.bufpos + 50) % 80] * 4 +
						data.buf[(data.bufpos + 51) % 80] * 8 +
						10 * (
							data.buf[(data.bufpos + 56) % 80] +
							data.buf[(data.bufpos + 57) % 80] * 2
						);

					// BCD range validation: speech or noise bits can satisfy the
					// sync-word pattern but decode to impossible SMPTE values.
					// Reject such frames outright; force re-lock so the decoder
					// does not propagate garbage into the delay engine.
					const int nomFps = (int)std::round(frates[slider]);
					const int parsed_hrs   = data.hrs;
					const int parsed_mnts  = data.mnts;
					const int parsed_scnds = data.scnds;
					const int parsed_frms  = data.frms;
					const double frameMsBcd = 1000.0 / (double)nomFps;
					auto rollbackDigits = [&]() {
						if (data.last_accepted_tc_ms >= 0)
						{
							const int64_t t = data.last_accepted_tc_ms;
							data.hrs   = (int)((t / 3600000LL) % 24);
							data.mnts  = (int)((t /   60000LL) % 60);
							data.scnds = (int)((t /    1000LL) % 60);
							data.frms  = (int)std::round(((double)(t % 1000LL)) / frameMsBcd);
						}
						else
						{
							data.hrs = data.mnts = data.scnds = data.frms = 0;
						}
					};
					if (parsed_hrs > 23 || parsed_mnts > 59 || parsed_scnds > 59 || parsed_frms >= nomFps)
					{
						++data.rejected_frames_count;
						data.syncpos = -1;
						// Restore last accepted values so a BCD-invalid frame
						// cannot leak its parsed digits into chnl1_in.hrs/...,
						// which processBlock reads when computing d_ms.
						rollbackDigits();
					}
					else
					{
						// Temporal-coherence gate.  Compute the candidate
						// timecode in milliseconds and require it to step
						// forward by 1–3 frames from the last accepted frame
						// when the decoder is locked.  Out-of-range deltas
						// indicate a speech-derived BCD-valid collision and
						// must not mutate any persistent decoder state.
						const int64_t candidate_ms =
							  (int64_t)parsed_hrs   * 3600000LL
							+ (int64_t)parsed_mnts  *   60000LL
							+ (int64_t)parsed_scnds *    1000LL
							+ (int64_t)((double)parsed_frms * frameMsBcd);

						bool accept_frame;
						if (data.locked && data.last_accepted_tc_ms >= 0)
						{
							const int64_t delta_ms = candidate_ms - data.last_accepted_tc_ms;
							const double minDelta  = -1.5 * frameMsBcd;   // 1 frame backward tolerance (jitter)
							const double maxDelta  =  3.5 * frameMsBcd;   // up to 3 dropped frames
							accept_frame = (double)delta_ms >= minDelta
							            && (double)delta_ms <= maxDelta;
						}
						else
						{
							accept_frame = true;
						}

						if (!accept_frame)
						{
							++data.rejected_frames_count;
							++data.consec_reject_since_lock;
							if (data.consec_reject_since_lock >= tc_data::UNLOCK_M)
							{
								data.locked                   = false;
								data.consec_good_frames       = 0;
								data.consec_reject_since_lock = 0;
								data.last_accepted_tc_ms      = -1;
							}
							// Roll parsed digits back to the last accepted
							// frame's values so garbage does not reach the
							// slave d_ms commit path via data.hrs/mnts/scnds/frms.
							rollbackDigits();
							data.syncpos = -1;
						}
						else
						{
							data.syncpos = 0;
							data.syncstate = 2;
							data.last_decode_sample = abs_pos;
							data.new_time = ((data.hrs * 60 + data.mnts) * 60 + data.scnds) * 100 + data.frms;

							// Coherence state update: count a legitimate step
							// (inside the normal +1..+3 band) toward re-lock.
							if (!data.locked)
							{
								if (data.last_accepted_tc_ms >= 0)
								{
									const int64_t delta_ms = candidate_ms - data.last_accepted_tc_ms;
									const bool stepForward =
									      (double)delta_ms >=  0.5 * frameMsBcd
									   && (double)delta_ms <=  3.5 * frameMsBcd;
									if (stepForward)
									{
										if (++data.consec_good_frames >= tc_data::LOCK_N)
										{
											data.locked                   = true;
											data.consec_reject_since_lock = 0;
										}
									}
									else
									{
										data.consec_good_frames = 1;
									}
								}
								else
								{
									data.consec_good_frames = 1;
								}
							}
							else
							{
								data.consec_reject_since_lock = 0;
							}
							data.last_accepted_tc_ms = candidate_ms;

							// Quality: lock and sync-word hit counting
							++data.w_sync_hits;
							++data.w_frames_decoded;

							// Continuity check: frame should follow prev by +1, or wrap at second boundary
							if (data.w_prev_frms >= 0)
							{
								bool step = (data.frms == data.w_prev_frms + 1);
								bool wrap = (data.frms == 0 && data.w_prev_frms >= 23);
								if (step || wrap)
									++data.w_continuity_ok;
							}
							data.w_prev_frms = data.frms;
						}
					} // end BCD-valid else
				}
				else
				{
					data.syncstate = 0;
				}
			}
			data.gotbit = -1;
		}
	}

	// Window boundary: compute metrics and reset accumulators
	++data.w_sample_count;
	if (data.w_sample_count >= data.W_SIZE)
		data.computeAndResetWindow((double)srate, frates[slider]);
}

inline void handle_const_delay(const float& sample, tc_data& data)
{
	data.const_buf.push_back(sample);
	if (data.const_buf.size() == (size_t)data.const_buf_size)
		data.const_buf.pop_front();
}


AutoSyncAudioProcessor::AutoSyncAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
	: AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
		.withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
		.withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
	)
#endif
{
	addParameter(myParameter = new juce::AudioParameterFloat("myParam", "Delay, ms", 0.0f, 4000.0f, 0.0f));

	auto logFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
	                   .getChildFile("MyPlugin_Debug.log");
	fileLogger.reset(new juce::FileLogger(logFile, "=== plugin start ===", 0));
	juce::Logger::setCurrentLogger(fileLogger.get());
	juce::Logger::writeToLog("Processor constructed");
}

AutoSyncAudioProcessor::~AutoSyncAudioProcessor()
{
	juce::Logger::writeToLog("Processor destroyed");
	juce::Logger::setCurrentLogger(nullptr);
}

//==============================================================================
const juce::String AutoSyncAudioProcessor::getName() const
{
	return JucePlugin_Name;
}

bool AutoSyncAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
	return true;
#else
	return false;
#endif
}

bool AutoSyncAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
	return true;
#else
	return false;
#endif
}

bool AutoSyncAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
	return true;
#else
	return false;
#endif
}

double AutoSyncAudioProcessor::getTailLengthSeconds() const
{
	return 0.0;
}

int AutoSyncAudioProcessor::getNumPrograms()
{
	return 1;
}

int AutoSyncAudioProcessor::getCurrentProgram()
{
	return 0;
}

void AutoSyncAudioProcessor::setCurrentProgram(int index)
{
}

const juce::String AutoSyncAudioProcessor::getProgramName(int index)
{
	return {};
}

void AutoSyncAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
}

//==============================================================================
void AutoSyncAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
	currentSampleRate = sampleRate;
	totalSamplesProcessed = 0;
	audFallback.init(sampleRate);
	ab.reset();
	prevFusionSource = FusionState::Source::None;

	// Open (or re-open) the shared memory region for this group.
	// SharedGroupMemory::open() calls close() internally, so re-entrant calls are safe.
	if (!shm.open(groupName.toStdString()))
		juce::Logger::writeToLog("AUTOSYNC: shm.open() failed for group \"" + groupName + "\"");
	else
		juce::Logger::writeToLog("AUTOSYNC: shm open OK  group=\"" + groupName
		    + "\"  mode=" + (pluginMode == PluginMode::Master ? "Master" : "Slave")
		    + "  slot=" + juce::String(slotId));
}

void AutoSyncAudioProcessor::releaseResources()
{
	shm.close();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool AutoSyncAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
	juce::ignoreUnused(layouts);
	return true;
#else
	if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
		&& layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
		return false;

#if ! JucePlugin_IsSynth
	if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
		return false;
#endif

	return true;
#endif
}


inline void AutoSyncAudioProcessor::processTimeCode(const float& sample, tc_data& channel, std::string& msg,
                                                      const int& index, const float& srate,
                                                      const int& slider, int64_t abs_pos)
{
	handleTimecode(sample, channel, srate, slider, abs_pos);

	if (channel.new_time != channel.old_time)
	{
		channel.old_time = channel.new_time;

		juce::Logger::writeToLog(
			"TC decode sr=" + juce::String(srate, 6) +
			" fpsIndex=" + juce::String(slider) +
			" syncstate=" + juce::String(channel.syncstate) +
			" hrs=" + juce::String(channel.hrs) +
			" min=" + juce::String(channel.mnts) +
			" sec=" + juce::String(channel.scnds) +
			" frm=" + juce::String(channel.frms) +
			" pulse=" + juce::String(channel.pulsesize, 6) +
			" threshold=" + juce::String((double)channel.threshold, 6));

		msg = std::to_string(channel.hrs / 10) + std::to_string(channel.hrs % 10) + ":"
			+ std::to_string(channel.mnts / 10) + std::to_string(channel.mnts % 10) + ":"
			+ std::to_string(channel.scnds / 10) + std::to_string(channel.scnds % 10) + ":"
			+ std::to_string(channel.frms / 10) + std::to_string(channel.frms % 10);
	}
}
#endif

void AutoSyncAudioProcessor::pushAudioAnalysisSample(float ch1, float ch2)
{
	if (audFallback.novelty1.empty())
		return;

	// Bandpass 100 Hz – 3500 Hz via cascaded 1-pole HPF + 1-pole LPF.
	// HPF removes DC / sub-100 Hz rumble that would otherwise dominate the
	// energy sum; LPF removes wind, HVAC, and codec artefacts above speech
	// band that add uncorrelated HF energy to the novelty envelope.
	const float h1 = audFallback.applyHPF(ch1, audFallback.hpfPrevX1, audFallback.hpfPrevY1);
	const float h2 = audFallback.applyHPF(ch2, audFallback.hpfPrevX2, audFallback.hpfPrevY2);
	const float b1 = audFallback.applyLPF(h1, audFallback.lpfPrev1);
	const float b2 = audFallback.applyLPF(h2, audFallback.lpfPrev2);
	audFallback.energyAcc1 += b1 * b1;
	audFallback.energyAcc2 += b2 * b2;

	if (++audFallback.hopSampleCounter < audFallback.hopSamples)
		return;

	// Hop boundary: compute novelty = positive energy increase
	float e1 = audFallback.energyAcc1 / (float)audFallback.hopSamples;
	float e2 = audFallback.energyAcc2 / (float)audFallback.hopSamples;

	// Asymmetric noise-floor follower + activity gate.
	// Fall is fast so the floor drops within ~150 ms after a loud signal (LTC) stops,
	// allowing quieter speech to open the gate quickly.
	// ch2 is only updated when it actually carries signal (non-zero after HPF).
	{
		const float tc1 = (e1 < audFallback.noiseFloor1)
		                ? AudioFallbackState::NOISE_FLOOR_TC_FALL
		                : AudioFallbackState::NOISE_FLOOR_TC_RISE;
		audFallback.noiseFloor1 = tc1 * audFallback.noiseFloor1 + (1.0f - tc1) * e1;
	}
	if (e2 > 1e-12f)
	{
		const float tc2 = (e2 < audFallback.noiseFloor2)
		                ? AudioFallbackState::NOISE_FLOOR_TC_FALL
		                : AudioFallbackState::NOISE_FLOOR_TC_RISE;
		audFallback.noiseFloor2 = tc2 * audFallback.noiseFloor2 + (1.0f - tc2) * e2;
	}

	const float dbThresh  = std::pow(10.0f, AudioFallbackState::ACTIVITY_GATE_DB / 10.0f);
	const bool  ch1Active = e1 > audFallback.noiseFloor1 * dbThresh;
	const bool  ch2Active = (e2 > 1e-12f) && (e2 > audFallback.noiseFloor2 * dbThresh);
	audFallback.activityGate = ch1Active || ch2Active;

	// Log-energy flux (scale-invariant).  Master and slave mics rarely have
	// matched gain; on linear flux a proportional level difference warps NCC
	// peaks because the same transient produces proportionally different
	// magnitudes on each side.  log-flux compares relative energy *changes*,
	// so the peak pattern aligns regardless of absolute gain.
	static constexpr float NOV_EPS = 1e-10f;
	float nov1 = std::max(0.0f, std::log(e1 + NOV_EPS) - std::log(audFallback.prevEnergy1 + NOV_EPS));
	float nov2 = std::max(0.0f, std::log(e2 + NOV_EPS) - std::log(audFallback.prevEnergy2 + NOV_EPS));

	// Stereo-LTC guard: when LTC is actively decoding on ch1, ch2 is likely also
	// carrying LTC in a stereo routing (both channels identical).  Scale nov2 to
	// zero as LTC quality rises so carrier energy doesn't poison the scene-audio
	// novelty buffer.  The gate opens automatically as LTC fades, letting speech
	// onsets accumulate cleanly before the NCC fallback activates.
	nov2 *= (1.0f - std::min(1.0f, chnl1_in.Q_LTC / 0.5f));

	audFallback.prevEnergy1      = e1;
	audFallback.prevEnergy2      = e2;
	audFallback.energyAcc1       = 0.0f;
	audFallback.energyAcc2       = 0.0f;
	audFallback.hopSampleCounter = 0;

	audFallback.novelty1[audFallback.writePos] = nov1;
	audFallback.novelty2[audFallback.writePos] = nov2;
	audFallback.writePos = (audFallback.writePos + 1) % audFallback.windowFrames;
	if (audFallback.framesFilled < audFallback.windowFrames)
		++audFallback.framesFilled;

	if (++audFallback.hopsSinceRefresh >= audFallback.refreshEvery)
	{
		audFallback.hopsSinceRefresh = 0;
		estimateAudioFallbackOffset();
		fuseLtcAndAudioFallback();
	}
}

void AutoSyncAudioProcessor::estimateAudioFallbackOffset()
{
	// Default to "not anchored" on every entry.  The flag is read by
	// fuseLtcAndAudioFallback to pick the NCC confidence threshold; letting
	// it carry stale state across early-returns (activity gate, ring not
	// yet filled) would cause the wrong threshold to be applied on the
	// next coast cycle.  Set true below only when a narrow-window NCC runs
	// or when the anchor-coast path below publishes deltaAudMs = anchorMs.
	audFallback.lastEstimateAnchored = false;

	const bool anchorAgeOk =
		audFallback.hasAnchor
		&& (juce::Time::currentTimeMillis() - anchorTimestampMs) < (int64_t)ANCHOR_MAX_AGE_MS;

	// Anchor-fresh coast during ring refill.
	//
	// After purgeNoveltyRing() fires on the LTC→FAIL edge, framesFilled drops
	// to 0 and takes ~20 s to reach windowFrames (MASTER_NOV_REF_SIZE).  The
	// main framesFilled guard below would otherwise block the estimator for
	// that whole period, leaving audFallback.valid=false and the fuser
	// outputting Source::None — while the anchor and α-β tracker still hold
	// the true offset.  Worse, by the time the ring refills the anchor can
	// age past ANCHOR_MAX_AGE_MS for long fades, forcing wide-mode NCC which
	// cannot reach offsets beyond ±lagRange·hopMs (±2 s) and locks onto a
	// spurious peak inside that range.
	//
	// The coast keeps deltaAudMs = anchorMs and valid=true so the fuser can
	// carry the pre-fade offset through the refill.  Marked anchored so the
	// relaxed confidence threshold is used if the confAud check ever matters.
	if (anchorAgeOk && audFallback.framesFilled < audFallback.windowFrames)
	{
		audFallback.deltaAudMs           = audFallback.anchorMs;
		audFallback.confAud              = 0.8;
		audFallback.peakCorr             = 0.0;
		audFallback.valid                = true;
		audFallback.lastEstimateAnchored = true;
		return;
	}

	if (audFallback.framesFilled < audFallback.windowFrames)
		return;

	// Activity gate: no informative audio → skip NCC, let alpha-beta coast.
	if (!audFallback.activityGate)
		return;

	const int N = audFallback.windowFrames;

	// ------------------------------------------------------------------
	// Search mode selection and SM staleness correction.
	//
	// Anchored: LTC recently confirmed the offset (anchor) and it fits in
	// the window.  Pre-shift masterNoveltyRef by K_eff so the NCC peak
	// lands near relLag=0; search only ±NARROW_HALF hops (±300 ms).
	//
	// Wide (fallback): no valid anchor — search ±lagRange hops (±2 s).
	//
	// Staleness: master writes novelty ~every 100 ms; slave reads it on
	// its own independent 100 ms timer → up to 200 ms offset.  Corrected
	// by K_eff = anchorHops − timeDeltaHops so master and slave frames
	// align to the same DAW timeline before the NCC runs.
	// ------------------------------------------------------------------
	// Compute staleness correction: master's novelty buffer may be up to
	// ~200 ms old when we read it (two async 0.1 s timers).  Correct by
	// shifting masterNoveltyRef forward by timeDeltaHops when building
	// linBuf2, so master and slave frames align to the same DAW time.
	//
	// Both sides must use a shared clock — the DAW playhead (currentDawSample)
	// — not each instance's self-maintained totalSamplesProcessed, which
	// starts at 0 on prepareToPlay and so differs by the load-time delta
	// between master and slave processes.  That delta is typically 10–15
	// hops and produces a systematic ±100–150 ms bias in the anchored NCC
	// lag (sign depending on which instance loaded first).
	// ------------------------------------------------------------------
	int timeDeltaHops = 0;
	if (masterNovAnchorSample > 0)
	{
		const int64_t deltaSamples = currentDawSample - masterNovAnchorSample;
		timeDeltaHops = (int)std::round((double)deltaSamples / (double)audFallback.hopSamples);
		// Clamp: staleness should be 0–200 ms; anything larger is noise.
		timeDeltaHops = std::max(0, std::min(timeDeltaHops, N / 8));
	}

	// Effective anchor (in hops), corrected for SM read staleness.
	// Positive K_eff -> slave is LATE w.r.t. master  -> look into master's past.
	// Negative K_eff -> slave is AHEAD w.r.t. master -> look into slave's past
	//                   (master can't see its own future, so we can't shift it forward).
	// Exactly one of masterShift / slaveShift is non-zero at a time.
	const int K_eff       = audFallback.anchorHops - timeDeltaHops;
	const int masterShift = (K_eff > 0) ?  K_eff : 0;
	const int slaveShift  = (K_eff < 0) ? -K_eff : 0;

	// anchorUsable: age ok, master ref received, AND each side has enough
	// real (non-zero) history to cover its respective shift plus the NCC
	// window.  The ring capacity already bounds shifts at ±N; the per-side
	// framesFilled check is the load-bearing constraint when rings aren't
	// full yet.
	const bool anchorUsable =
		anchorAgeOk
		&& audFallback.hasMasterRef
		&& std::abs(K_eff) < N - AudioFallbackState::NARROW_HALF - 1
		&& audFallback.masterFramesFilled >= AudioFallbackState::ANCHORED_WIN + masterShift
		&& audFallback.framesFilled       >= AudioFallbackState::ANCHORED_WIN + slaveShift;

	// Anchor is fresh but history too short for a safe anchored NCC: hold
	// the LTC value rather than feed the alpha-beta a bogus residual.
	if (anchorAgeOk && !anchorUsable)
	{
		audFallback.deltaAudMs           = audFallback.anchorMs;
		audFallback.confAud              = 0.8;
		audFallback.peakCorr             = 0.0;
		audFallback.valid                = true;
		audFallback.lastEstimateAnchored = true;
		return;
	}

	// Wide-mode guard: if we hold an anchor (possibly aged out) whose
	// magnitude exceeds wide-NCC reach, any peak found in ±lagRange is
	// guaranteed to be spurious and would seed the α-β tracker to a wildly
	// wrong value.  Hold α-β on its last velocity instead.
	if (!anchorUsable
	    && audFallback.hasAnchor
	    && std::abs(audFallback.anchorHops) > audFallback.lagRange)
	{
		audFallback.valid       = false;
		audFallback.stableCount = 0;
		return;
	}

	audFallback.lastEstimateAnchored = anchorUsable;

	// ------------------------------------------------------------------
	// Effective NCC window.
	//
	// Anchored mode: most-recent ANCHORED_WIN frames (1.2 s) so the
	// estimator reacts within ~1.2 s after a manual track shift.
	// Wide mode: most-recent WIDE_WIN frames (4 s) for cold-start acquisition.
	// The full N-frame novelty ring is held so that the anchored lookup can
	// reach into deep master history (up to ~20 s) without resizing per-cycle.
	//
	// startOff: how many frames from the circular-buffer "oldest" pointer
	// to skip, so that [startOff, startOff+nccN) are the most-recent nccN frames.
	// ------------------------------------------------------------------
	const int nccN    = anchorUsable ? AudioFallbackState::ANCHORED_WIN
	                                 : AudioFallbackState::WIDE_WIN;
	const int startOff = N - nccN;

	// ------------------------------------------------------------------
	// Linearise novelty1 (slave, circular).  Anchored-mode pre-shift of
	// slaveShift hops kicks in only when K_eff<0 (slave ahead): we can't
	// shift master forward in time, so instead we read slave's past to
	// line it up with master's present.
	// ------------------------------------------------------------------
	const int s1Shift = anchorUsable ? slaveShift : 0;
	for (int i = 0; i < nccN; ++i)
	{
		int src = (audFallback.writePos + startOff + i - s1Shift + N * 8) % N;
		audFallback.linBuf1[i] = audFallback.novelty1[src];
	}

	// ------------------------------------------------------------------
	// Build linBuf2 (master reference).
	//
	// Anchored: pre-shift masterNoveltyRef back by masterShift so the NCC
	// peak lands at relLag=0 when the true offset equals anchorMs.  Only
	// one of masterShift/slaveShift is non-zero, so their difference still
	// equals K_eff and the absoluteLag conversion below (bestRelLag −
	// anchorHops) is the same expression whichever side was shifted.
	//
	// Wide: use masterNoveltyRef unshifted (or novelty2 if SM not yet up),
	// corrected for SM staleness by timeDeltaHops.
	// ------------------------------------------------------------------
	if (anchorUsable)
	{
		for (int i = 0; i < nccN; ++i)
			audFallback.linBuf2[i] = audFallback.masterNoveltyRef[(startOff + i - masterShift + N * 8) % N];
	}
	else
	{
		for (int i = 0; i < nccN; ++i)
		{
			int src = (audFallback.writePos + startOff + i) % N;
			audFallback.linBuf2[i] = audFallback.hasMasterRef
			                       ? audFallback.masterNoveltyRef[(startOff + i + timeDeltaHops + N * 8) % N]
			                       : audFallback.novelty2[src];
		}
	}

	// ------------------------------------------------------------------
	// Means + std devs (over nccN frames)
	// ------------------------------------------------------------------
	float m1 = 0.0f, m2 = 0.0f;
	for (int i = 0; i < nccN; ++i) { m1 += audFallback.linBuf1[i]; m2 += audFallback.linBuf2[i]; }
	m1 /= (float)nccN;
	m2 /= (float)nccN;

	float s1 = 0.0f, s2 = 0.0f;
	for (int i = 0; i < nccN; ++i)
	{
		float d1 = audFallback.linBuf1[i] - m1;
		float d2 = audFallback.linBuf2[i] - m2;
		s1 += d1 * d1;
		s2 += d2 * d2;
	}
	s1 = std::sqrt(s1 / (float)nccN);
	s2 = std::sqrt(s2 / (float)nccN);

	if (s1 < 1e-6f || s2 < 1e-6f)
	{
		audFallback.valid       = false;
		audFallback.stableCount = 0;
		return;
	}

	// ------------------------------------------------------------------
	// NCC helper — computes correlation at one lag value.
	// linBuf1/linBuf2, m1/m2, s1/s2, nccN captured by reference.
	// ------------------------------------------------------------------
	auto nccAt = [&](int lag) -> double
	{
		double sum = 0.0;
		int    cnt = 0;
		for (int i = 0; i < nccN; ++i)
		{
			int j = i + lag;
			if (j >= 0 && j < nccN)
			{
				sum += (double)(audFallback.linBuf1[i] - m1)
				     * (double)(audFallback.linBuf2[j] - m2);
				++cnt;
			}
		}
		return cnt > 0 ? sum / ((double)cnt * (double)s1 * (double)s2) : 0.0;
	};

	// ------------------------------------------------------------------
	// NCC search over the chosen range.
	// In anchored mode the search is relative to the pre-shifted linBuf2,
	// so bestRelLag ≈ 0 when the actual offset equals anchorMs.
	// ------------------------------------------------------------------
	const int searchHalf = anchorUsable ? AudioFallbackState::NARROW_HALF
	                                    : audFallback.lagRange;

	// Cache every NCC evaluation so the parabola interpolator and the
	// distance-gated runner-up scan (below) can reuse them without
	// recomputing.  Stack-allocated; sized for the widest search (lagRange=200).
	static constexpr int CORR_VALS_CAP = 2 * 200 + 1;
	jassert(2 * searchHalf + 1 <= CORR_VALS_CAP);
	std::array<double, CORR_VALS_CAP> corrVals{};

	double bestCorr   = -2.0;
	int    bestRelLag = 0;

	for (int lag = -searchHalf; lag <= searchHalf; ++lag)
	{
		double corr = nccAt(lag);
		corrVals[lag + searchHalf] = corr;
		if (corr > bestCorr)
		{
			bestCorr   = corr;
			bestRelLag = lag;
		}
	}

	// Distance-gated runner-up.  The original single-pass tracked any second
	// peak, including lags 1–2 hops either side of the best — which is just
	// the curvature of the same peak and artificially deflates prominence.
	// Restrict the runner-up to lags ≥ MIN_PEAK_DIST hops from the best so
	// prominence measures competition from genuinely distinct peaks only.
	static constexpr int MIN_PEAK_DIST = 5;
	double secondBest = -2.0;
	for (int lag = -searchHalf; lag <= searchHalf; ++lag)
	{
		if (std::abs(lag - bestRelLag) < MIN_PEAK_DIST)
			continue;
		const double corr = corrVals[lag + searchHalf];
		if (corr > secondBest)
			secondBest = corr;
	}

	// ------------------------------------------------------------------
	// Sub-hop parabola interpolation — sharpens accuracy from ±10 ms to
	// roughly ±2 ms at no extra window cost.
	// Only applied when the peak is not at the search boundary.
	// ------------------------------------------------------------------
	double subHopOffset = 0.0;
	if (bestRelLag > -searchHalf && bestRelLag < searchHalf)
	{
		double y0    = corrVals[bestRelLag - 1 + searchHalf];
		double y1    = bestCorr;
		double y2    = corrVals[bestRelLag + 1 + searchHalf];
		double denom = y2 - 2.0 * y1 + y0;
		if (std::abs(denom) > 1e-9)
			subHopOffset = std::max(-0.5, std::min(0.5, -0.5 * (y2 - y0) / denom));
	}

	// ------------------------------------------------------------------
	// Convert relative lag → absolute lag → milliseconds.
	//
	// Anchored: whichever side was pre-shifted, the net pre-shift between
	// slave and master equals K_eff (= masterShift − slaveShift since at
	// most one is non-zero), so a residual relLag=0 still means "offset =
	// anchorHops exactly".  Conversion to absolute lag is the same in both
	// cases:
	//   absoluteLag = relLag − anchorHops
	//
	// Wide: no shift — relative == absolute.
	//
	// Sign convention: absoluteLag > 0 → slave is early → deltaAudMs < 0.
	//                  absoluteLag < 0 → slave is late  → deltaAudMs > 0.
	// ------------------------------------------------------------------
	const int absoluteBestLag = anchorUsable
		? (bestRelLag - audFallback.anchorHops)
		: bestRelLag;

	const double absoluteLagSub = anchorUsable
		? ((double)bestRelLag + subHopOffset - (double)audFallback.anchorHops)
		: ((double)bestRelLag + subHopOffset);

	// ------------------------------------------------------------------
	// Confidence + stability
	// ------------------------------------------------------------------
	double prominence = (bestCorr > 0.0)
		? std::min(1.0, (bestCorr - std::max(0.0, secondBest)) / (bestCorr + 1e-9))
		: 0.0;

	bool stable = (audFallback.prevBestLag != INT_MAX
	            && std::abs(absoluteBestLag - audFallback.prevBestLag) <= 2);
	if (stable) ++audFallback.stableCount;
	else         audFallback.stableCount = 0;

	// Anchored estimates converge faster: only 2 stable hits needed vs 3.
	const double stabDivisor = anchorUsable ? 2.0 : 3.0;
	double stability = std::min(1.0, audFallback.stableCount / stabDivisor);

	const int    stabThresh  = anchorUsable ? 2   : 3;
	const double confThresh  = anchorUsable ? 0.25 : 0.3;

	audFallback.confAud    = std::max(0.0, std::min(1.0, 0.6 * prominence + 0.4 * stability));
	audFallback.deltaAudMs = -absoluteLagSub * audFallback.hopMs;
	audFallback.bestLag    = absoluteBestLag;
	audFallback.peakCorr   = bestCorr;
	audFallback.secondPeak = secondBest;
	audFallback.prevBestLag = absoluteBestLag;
	audFallback.valid      = (audFallback.stableCount >= stabThresh
	                       && audFallback.confAud > confThresh);
}

void AutoSyncAudioProcessor::fuseLtcAndAudioFallback()
{
	// In slave mode only chnl1_in is decoded; chnl2_in is never updated and
	// would permanently report FAIL, so we use masterValid + chnl1_in state
	// instead of the old dual-channel check from the stereo-pair architecture.
	bool ltcOk;
	if (pluginMode == PluginMode::Master)
		ltcOk = true;  // master is the reference; fusion result is not used for delay
	else
		ltcOk = masterValid
		     && (chnl1_in.ltc_state == tc_data::LTCState::VALID)
		     && !drift_suspected;

	// Detect LTC→FAIL transition and arm the hold timer.  Only fires once per
	// fade event (edge-detect on prevChnl1InLtcState).
	if (chnl1_in.ltc_state == tc_data::LTCState::FAIL
	    && prevChnl1InLtcState != tc_data::LTCState::FAIL)
	{
		ltcFadeTimestampMs = juce::Time::currentTimeMillis();

		// Purge the novelty ring so the post-fade NCC window cannot include
		// LTC-carrier-tail transients from before the edge.  The 2.5 s hold
		// below gives the ring time to refill with clean scene-only data
		// before fallback is permitted to activate.
		audFallback.purgeNoveltyRing();
	}
	prevChnl1InLtcState = chnl1_in.ltc_state;

	if (ltcOk)
	{
		// Only refresh the anchor when d_ms is stable — no recent jump candidate.
		// A value that slipped through the confirm count must not become the NCC seed.
		if (!d_ms_recently_jumped)
		{
			audFallback.anchorMs   = d_ms;
			audFallback.anchorHops = (int)std::round(d_ms / audFallback.hopMs);
			audFallback.hasAnchor  = true;
			anchorTimestampMs      = juce::Time::currentTimeMillis();
		}

		fusion.source         = FusionState::Source::LTC;
		fusion.selectedMs     = d_ms;
		fusion.selectedConf   = (double)chnl1_in.Q_LTC;
		fusion.fallbackActive = false;
	}
	else
	{
		// Transition hold: suppress audio fallback for 2.5 s after LTC drops.
		// This gives the LTC carrier time to clear from the novelty buffer (important
		// for stereo-LTC routing where ch2 also carries LTC) and prevents the NCC
		// from running on a contaminated buffer during the fade-out window.
		const bool inTransitionHold = (ltcFadeTimestampMs != 0)
		    && (juce::Time::currentTimeMillis() - ltcFadeTimestampMs < 2500LL);

		// Use a tighter confidence threshold when the estimator ran in anchored
		// mode: the narrow search window produces fewer false peaks.
		const double confThresh = audFallback.lastEstimateAnchored ? 0.25 : 0.4;

		if (!inTransitionHold && audFallback.valid && audFallback.confAud > confThresh)
		{
			fusion.source         = FusionState::Source::AudioFallback;
			fusion.selectedMs     = audFallback.deltaAudMs;
			fusion.selectedConf   = audFallback.confAud;
			fusion.fallbackActive = true;

			// A coherent anchored NCC hit proves the anchor is still tracking
			// the true offset, so refresh its timestamp.  This lets us hold
			// onto a large LTC-captured offset indefinitely as long as audio
			// correlation stays healthy — the 30 s anchor age limit only runs
			// down during silence or broken audio, not during normal speech.
			if (audFallback.lastEstimateAnchored)
				anchorTimestampMs = juce::Time::currentTimeMillis();
		}
		else
		{
			fusion.source         = FusionState::Source::None;
			fusion.selectedMs     = 0.0;
			fusion.selectedConf   = 0.0;
			fusion.fallbackActive = false;
		}
	}

	// ------------------------------------------------------------------
	// Alpha-beta tracker — updated every NCC refresh cycle (~200 ms).
	//
	// While LTC is healthy the tracker is kept synchronised to d_ms so it
	// is ready to coast the moment LTC drops.  When the audio fallback is
	// active the tracker absorbs the NCC measurement; when both sources
	// abstain it extrapolates on the last velocity estimate.
	//
	// Large-jump fast path: if the NCC has found a new stable offset more
	// than 150 ms from the current estimate (e.g. after a manual track
	// shift in the DAW), seed directly rather than waiting for the alpha
	// term to slowly converge over many seconds.
	// ------------------------------------------------------------------
	if (fusion.source == FusionState::Source::AudioFallback
	    && audFallback.valid
	    && ab.initialized
	    && std::abs(audFallback.deltaAudMs - ab.estMs) > 150.0)
	{
		ab.seed(audFallback.deltaAudMs);
	}

	if (!ab.initialized && fusion.source != FusionState::Source::None)
		ab.seed(audFallback.anchorMs);  // prime from the last confirmed LTC value

	if (ab.initialized)
	{
		const bool feedMeasured = (fusion.source == FusionState::Source::AudioFallback)
		                       && audFallback.valid;
		ab.update(audFallback.deltaAudMs, feedMeasured);

		// Resync to LTC while it is driving so the first coast after dropout
		// starts from the freshest reliable value with zero initial velocity.
		if (fusion.source == FusionState::Source::LTC)
		{
			ab.estMs        = d_ms;
			ab.velMsPerS    = 0.0;
			ab.lastUpdateMs = juce::Time::currentTimeMillis();
		}
	}
	prevFusionSource = fusion.source;
}

void AutoSyncAudioProcessor::writeMasterSlot()
{
	MasterSlot& m = shm.get()->master;

	// Timecode to milliseconds; frms converted at the current FPS setting
	const int64_t tc_ms = (int64_t)chnl1_in.hrs   * 3600000LL
	                    + (int64_t)chnl1_in.mnts   * 60000LL
	                    + (int64_t)chnl1_in.scnds  * 1000LL
	                    + (int64_t)chnl1_in.frms   * 1000LL / std::max(1, fps);

	const int N = audFallback.windowFrames;   // MASTER_NOV_REF_SIZE at runtime (set by init())

	seqcount_write_begin(m.writeSeq);

	m.tc_ref_ms           = tc_ms;
	m.ref_decode_sample   = chnl1_in.last_decode_sample;
	m.nov_anchor_sample   = currentDawSample;  // shared DAW-timeline position
	m.Q_ref               = chnl1_in.Q_LTC;
	m.ltc_state           = (uint8_t)chnl1_in.ltc_state;
	m.locked              = chnl1_in.locked ? 1u : 0u;
	m.valid               = (audFallback.framesFilled >= N);
	m.nov_writePos        = audFallback.writePos;
	m.nov_framesFilled    = audFallback.framesFilled;

	// Copy the full novelty1 circular buffer verbatim; the slave uses
	// nov_writePos to know where the oldest frame sits.
	for (int i = 0; i < N; ++i)
		m.nov_ref[i] = audFallback.novelty1[i];

	seqcount_write_end(m.writeSeq);

	++shmWriteCount;
}

void AutoSyncAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	juce::ScopedNoDenormals noDenormals;

	// Absolute DAW timeline position of the first sample in this buffer.
	// Using the playhead ensures all instances share the same origin regardless
	// of when their prepareToPlay was called (e.g. slave added mid-session).
	// Falls back to the self-maintained counter when no playhead is available
	// (standalone mode, some hosts).
	int64_t bufferStartSample = totalSamplesProcessed;
	if (auto* ph = getPlayHead())
		if (auto pos = ph->getPosition())
			if (auto ts = pos->getTimeInSamples())
				bufferStartSample = *ts;
	totalSamplesProcessed += buffer.getNumSamples();

	static int counter = 0;
	if ((++counter % 200) == 0)
	{
		juce::Logger::writeToLog(
			"processBlock samples=" + juce::String(buffer.getNumSamples()) +
			" d_ms=" + juce::String(d_ms) +
			" fps=" + juce::String(fps));
	}

	auto totalNumInputChannels = getTotalNumInputChannels();
	auto totalNumOutputChannels = getTotalNumOutputChannels();

	for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
		buffer.clear(i, 0, buffer.getNumSamples());

	auto* write1 = buffer.getWritePointer(0);
	auto* write2 = buffer.getWritePointer(1);

	// =========================================================================
	// MASTER PATH
	// Decode LTC from the designated channel, extract novelty, write SM.
	// Audio passes through unchanged — master is the reference, no delay.
	// =========================================================================
	if (pluginMode == PluginMode::Master)
	{
		const int fpsIndex = (fps == 25) ? 2 : 0;

		for (int i = 0; i < buffer.getNumSamples(); ++i)
		{
			currentDawSample = bufferStartSample + i;

			// Read from the designated LTC channel; always decode into chnl1_in.
			float ltcSample = (ltcChannel == 0) ? write1[i] : write2[i];
			processTimeCode(ltcSample, chnl1_in, input_ch1, i, (float)currentSampleRate, fpsIndex, bufferStartSample + i);

			// Novelty extraction for the reference curve.
			// ch2 = non-LTC channel (scene audio when available, else near-silence).
			// The activity gate in pushAudioAnalysisSample will suppress NCC
			// updates automatically if ch2 carries no informative content.
			float sceneSample = (ltcChannel == 0) ? write2[i] : write1[i];
			pushAudioAnalysisSample(ltcSample, sceneSample);

			// Update diagnostics + write SM every ~0.1 s.
			// Counter is per-sample (matching the slave path) — do NOT move this outside
			// the loop; per-block counting would require thousands of blocks to fire.
			if (++dt_sample_counter >= (int)(currentSampleRate * 0.1))
			{
				dt_sample_counter = 0;
				ch1_Q_LTC         = chnl1_in.Q_LTC;
				ch1_ltc_state     = (int)chnl1_in.ltc_state;
				ch1_estimated_fps = chnl1_in.estimated_fps;
				input_ch1         = (chnl1_in.hrs  < 10 ? "0" : "") + std::to_string(chnl1_in.hrs)   + ":"
				                  + (chnl1_in.mnts  < 10 ? "0" : "") + std::to_string(chnl1_in.mnts)  + ":"
				                  + (chnl1_in.scnds < 10 ? "0" : "") + std::to_string(chnl1_in.scnds) + ":"
				                  + (chnl1_in.frms  < 10 ? "0" : "") + std::to_string(chnl1_in.frms);

				if (shm.isOpen())
					writeMasterSlot();
			}
		}

		return;   // skip all slave / stereo logic below
	}

	// =========================================================================
	// SLAVE PATH
	// Decode own LTC from ltcChannel, read master SM for reference offset,
	// apply delay.  Audio fallback NCC wired up in Step 6.
	// =========================================================================

	// MIDI: send (delay + slider) as 14-bit CC and pitch wheel
	int value = static_cast<int>(static_cast<float>(((std::abs(d_ms) + by_slider) / 10000) * 16383.0f));
	if (value < 0) value = 0;
	int valueMSB = (value >> 7) & 0x7F;
	int valueLSB = value & 0x7F;
	midiMessages.addEvent(juce::MidiMessage::controllerEvent(1, 6, valueMSB), 0);
	midiMessages.addEvent(juce::MidiMessage::controllerEvent(1, 38, valueLSB), 0);
	midiMessages.addEvent(juce::MidiMessage::pitchWheel(1, value), 0);

	const int fpsIndex = (fps == 25) ? 2 : 0;

	for (int i = 0; i < buffer.getNumSamples(); ++i)
	{
		currentDawSample = bufferStartSample + i;

		// Fill const_buf for BOTH channels so delay() can read from them immediately
		// (without this, the channel whose const_buf is empty outputs silence for
		// the first delay_size samples while its delay_buf fills up).
		handle_const_delay(write1[i], chnl1);
		handle_const_delay(write2[i], chnl2);

		// Decode own LTC from the configured channel only (pre-delay)
		float ltcSample = (ltcChannel == 0) ? write1[i] : write2[i];
		processTimeCode(ltcSample, chnl1_in, input_ch1, i, (float)currentSampleRate, fpsIndex, bufferStartSample + i);

		// Slave novelty1 tracks the same LTC channel as the master reference curve.
		// ch2 = non-LTC channel (scene audio when available).  The activity gate in
		// pushAudioAnalysisSample suppresses NCC updates during silence automatically.
		{
			float sceneSample = (ltcChannel == 0) ? write2[i] : write1[i];
			pushAudioAnalysisSample(ltcSample, sceneSample);
		}

		// Target offset: master-derived d_ms (updated every ~0.1s in timer block below).
		// If master SM is stale or not yet received, the alpha-beta tracker coasts on
		// its last velocity estimate rather than snapping to zero.
		double targetMs;
		if (masterValid)
			targetMs = d_ms + by_slider;
		else if (ab.initialized)
			targetMs = ab.estMs + by_slider;   // smoothed + rate-limited; coasts on velocity when abstaining
		else
			targetMs = 0.0;

		// Rebuild delay engine if master target has shifted by more than 2 frames
		const double rebuildThreshMs = 2000.0 / fps;
		if (active_delay && activeDelayMs != 0.0 && targetMs != 0.0 &&
		    std::abs(targetMs - activeDelayMs) > rebuildThreshMs)
		{
			chnl1.clear();
			chnl2.clear();
			activeDelayMs = 0.0;
		}

		delay_ms   = std::to_string((int)std::round(std::abs(d_ms)));
		o_delay_ms = std::to_string((int)std::round(std::abs(targetMs)));

		myParameter->setValueNotifyingHost(static_cast<float>((std::abs(targetMs) + std::floor(by_slider)) / 4000));

		if (active_delay)
		{
			// Slave mode: delay BOTH channels uniformly by |targetMs|.
			// The old sign-based single-channel logic was for the stereo PoC;
			// in master-slave mode the whole slave track shifts in time as one.
			if (!chnl1.active_delay && !chnl2.active_delay && targetMs != 0.0)
			{
				chnl1.active_delay = true;
				chnl2.active_delay = true;
			}

			if (chnl1.active_delay)
			{
				if (!chnl1.delay_size)
				{
					chnl1.delay_size = (size_t)std::floor(std::abs(targetMs) / 1000.0 * currentSampleRate);
					activeDelayMs = targetMs;
				}
				delay(write1, i, chnl1);
			}
			if (chnl2.active_delay)
			{
				if (!chnl2.delay_size)
					chnl2.delay_size = (size_t)std::floor(std::abs(targetMs) / 1000.0 * currentSampleRate);
				delay(write2, i, chnl2);
			}

			// Decode post-delay LTC from the configured channel for tc display
			float ltcOut = (ltcChannel == 0) ? write1[i] : write2[i];
			processTimeCode(ltcOut, chnl1, tc, i, (float)currentSampleRate, fpsIndex, bufferStartSample + i);
		}
		else
		{
			tc = input_ch1;
			chnl1.clear();
			chnl2.clear();
			activeDelayMs = 0.0;
		}

		// Update diagnostics + read master SM every ~0.1s
		if (++dt_sample_counter >= (int)(currentSampleRate * 0.1))
		{
			dt_sample_counter = 0;

			ch1_Q_LTC           = chnl1_in.Q_LTC;
			ch1_ltc_state       = (int)chnl1_in.ltc_state;
			ch1_estimated_fps   = chnl1_in.estimated_fps;
			ch1_decoder_resets  = chnl1_in.decoder_reset_count;
			ch1_rejected_frames = chnl1_in.rejected_frames_count;
			fallback_requested  = chnl1_in.fallback_requested;

			// Audio fallback snapshot
			aud_deltaMs       = audFallback.deltaAudMs;
			aud_conf          = audFallback.confAud;
			aud_fusionSource  = (int)fusion.source;
			aud_activeDelayMs = activeDelayMs;

			// Read master SM; compute dt_ltc_ms = tc_self_ms − tc_ref_ms
			if (shm.isOpen())
			{
				const MasterSlot& m = shm.get()->master;
				int64_t tc_ref_ms_local         = 0;
				int64_t ref_decode_sample_local  = 0;
				int64_t nov_anchor_sample_local  = 0;
				uint8_t master_ltc_state         = 0;
				uint8_t master_locked_local      = 0;
				bool    master_valid_local       = false;
				int     nov_writePos_local       = 0;
				int     nov_framesFilled_local   = 0;
				float   nov_ref_local[MASTER_NOV_REF_SIZE] = {};

				uint32_t seq1, seq2;
				do {
					seq1 = seqcount_read_begin(m.writeSeq);
					tc_ref_ms_local          = m.tc_ref_ms;
					ref_decode_sample_local  = m.ref_decode_sample;
					nov_anchor_sample_local  = m.nov_anchor_sample;
					master_ltc_state         = m.ltc_state;
					master_locked_local      = m.locked;
					master_valid_local       = m.valid;
					nov_writePos_local       = m.nov_writePos;
					nov_framesFilled_local   = m.nov_framesFilled;
					for (int ni = 0; ni < audFallback.windowFrames; ++ni)
						nov_ref_local[ni] = m.nov_ref[ni];
					seq2 = seqcount_read_end(m.writeSeq);
				} while (seq1 != seq2);

				// Store master's write timestamp so estimateAudioFallbackOffset()
				// can correct for the asynchronous SM read latency (~0–200 ms).
				if (nov_anchor_sample_local > 0)
					masterNovAnchorSample = nov_anchor_sample_local;

				// Linearise master's circular novelty buffer (oldest→newest) into
				// audFallback.masterNoveltyRef so that estimateAudioFallbackOffset()
				// can cross-correlate slave transients against master transients.
				// Gate at WIDE_WIN (4 s) rather than full windowFrames (20 s) so
				// wide-mode NCC can start shortly after both instances come up;
				// anchored mode additionally gates on masterFramesFilled covering
				// the specific anchor shift (see estimateAudioFallbackOffset).
				audFallback.masterFramesFilled = nov_framesFilled_local;
				if (nov_framesFilled_local >= AudioFallbackState::WIDE_WIN)
				{
					const int N = audFallback.windowFrames;
					for (int ni = 0; ni < N; ++ni)
						audFallback.masterNoveltyRef[ni] = nov_ref_local[(nov_writePos_local + ni) % N];
					audFallback.hasMasterRef = true;
				}

				const int64_t tc_self_ms_local =
					  (int64_t)chnl1_in.hrs   * 3600000LL
					+ (int64_t)chnl1_in.mnts  *   60000LL
					+ (int64_t)chnl1_in.scnds *    1000LL
					+ (int64_t)chnl1_in.frms  *    1000LL / std::max(1, fps);

				// Lock gate: both sides must have the temporal-coherence gate
				// engaged before we'll update d_ms from LTC.  When either decoder
				// is unlocked (re-acquiring after a sustained reject burst, or
				// having latched briefly onto speech-derived BCD-valid garbage
				// during an LTC fade), the tc_ref_ms / tc_self_ms pair can't be
				// trusted — hold the last committed d_ms instead.
				const bool bothLocked = (master_locked_local != 0) && chnl1_in.locked;
				if (master_valid_local && master_ltc_state >= 1 && bothLocked && tc_ref_ms_local != 0)
				{
					// Sample-accurate delay: measures how many samples apart the two
					// decoders' last frame-detect events were on the shared timeline,
					// then subtracts the LTC frame-number difference so that perfectly
					// aligned tracks always read 0ms regardless of which frames were
					// compared or when the 0.1s update ticks fired.
					//
					//   delay_ms = (A_slave - A_master)/sr*1000 - (TC_self - TC_ref)
					//
					// Falls back to the old frame-number comparison if either side
					// hasn't decoded a frame yet (positions still 0).
					double newDtMs;
					if (chnl1_in.last_decode_sample != 0 && ref_decode_sample_local != 0)
					{
						const double sampleDiff_ms =
							(double)(chnl1_in.last_decode_sample - ref_decode_sample_local)
							/ currentSampleRate * 1000.0;
						newDtMs = sampleDiff_ms - (double)(tc_self_ms_local - tc_ref_ms_local);
					}
					else
					{
						newDtMs = (double)(tc_ref_ms_local - tc_self_ms_local);
					}

					// Hysteresis: if newDtMs differs from d_ms by more than the effective
					// threshold, hold it in d_ms_pending and require D_MS_JUMP_CONFIRMS
					// consecutive consistent readings before committing.
					//
					// Quality gate: when the LTC signal is degrading (Q < 0.65), tighten
					// the threshold to 50 ms so that corrupted-but-plausible frames during
					// a fade cannot accumulate 3 confirmations and commit a garbage d_ms.
					const double effectiveJumpThresh = (chnl1_in.Q_LTC < 0.65f)
					    ? 50.0 : D_MS_JUMP_THRESH_MS;

					bool commitDtMs = true;
					if (d_ms != 0.0 && std::abs(newDtMs - d_ms) > effectiveJumpThresh)
					{
						d_ms_clean_count     = 0;
						d_ms_recently_jumped = true;
						if (std::abs(newDtMs - d_ms_pending) < effectiveJumpThresh / 2.0)
							++d_ms_pending_count;
						else
						{
							d_ms_pending       = newDtMs;
							d_ms_pending_count = 1;
						}
						commitDtMs = (d_ms_pending_count >= D_MS_JUMP_CONFIRMS);
					}
					else
					{
						d_ms_pending_count = 0;
						if (++d_ms_clean_count >= 5)
							d_ms_recently_jumped = false;
					}

					lastMasterWriteMs = juce::Time::currentTimeMillis();
					masterValid       = true;
					holding           = false;

					if (commitDtMs)
					{
						// Jump detection: reset delay engine if master offset jumps > 2 frames
						const double frameMs = 1000.0 / fps;
						if (activeDelayMs != 0.0 && std::abs(newDtMs - d_ms) > 2.0 * frameMs)
						{
							chnl1.clear();
							chnl2.clear();
							activeDelayMs = 0.0;
						}
						d_ms = newDtMs;
					}
				}
				else if (lastMasterWriteMs != 0 &&
				         juce::Time::currentTimeMillis() - lastMasterWriteMs > 2000)
				{
					// Master data stale: freeze delay at current activeDelayMs
					masterValid = false;
					holding     = true;
				}
			}
		}
	}
}

//==============================================================================
bool AutoSyncAudioProcessor::hasEditor() const
{
	return true;
}

juce::AudioProcessorEditor* AutoSyncAudioProcessor::createEditor()
{
	return new AutoSyncAudioProcessorEditor(*this);
}

//==============================================================================
void AutoSyncAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
	juce::XmlElement xml("AutosyncState");
	xml.setAttribute("mode",         (int)pluginMode);
	xml.setAttribute("group",        groupName);
	xml.setAttribute("slot",         slotId);
	xml.setAttribute("ltcch",        ltcChannel);
	xml.setAttribute("label",        slotLabel);
	xml.setAttribute("fps",          fps);
	xml.setAttribute("active_delay", active_delay ? 1 : 0);
	xml.setAttribute("by_slider",    by_slider);
	copyXmlToBinary(xml, destData);
}

void AutoSyncAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
	auto xml = getXmlFromBinary(data, sizeInBytes);
	if (xml == nullptr || !xml->hasTagName("AutosyncState"))
		return;

	pluginMode   = (PluginMode)xml->getIntAttribute("mode",  (int)PluginMode::Slave);
	groupName    = xml->getStringAttribute("group", "group1");
	slotId       = juce::jlimit(1, 8, xml->getIntAttribute("slot",  1));
	ltcChannel   = juce::jlimit(0, 1, xml->getIntAttribute("ltcch", 0));
	slotLabel    = xml->getStringAttribute("label", "");
	fps          = xml->getIntAttribute("fps", 30) == 25 ? 25 : 30;
	active_delay = xml->getIntAttribute("active_delay", 0) != 0;
	by_slider    = juce::jlimit(-250.0, 250.0, xml->getDoubleAttribute("by_slider", 0.0));
	// shm will be (re-)opened on the next prepareToPlay call.
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new AutoSyncAudioProcessor();
}
