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

	if (data1.timecode_counter > 20 || data2.timecode_counter > 20 && std::abs(delay_ms) > 10000)
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

					data.syncpos = 0;
					data.syncstate = 2;
					data.last_decode_sample = abs_pos;
					data.new_time = ((data.hrs * 60 + data.mnts) * 60 + data.scnds) * 100 + data.frms;

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


NewProjectAudioProcessor::NewProjectAudioProcessor()
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

NewProjectAudioProcessor::~NewProjectAudioProcessor()
{
	juce::Logger::writeToLog("Processor destroyed");
	juce::Logger::setCurrentLogger(nullptr);
}

//==============================================================================
const juce::String NewProjectAudioProcessor::getName() const
{
	return JucePlugin_Name;
}

bool NewProjectAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
	return true;
#else
	return false;
#endif
}

bool NewProjectAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
	return true;
#else
	return false;
#endif
}

bool NewProjectAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
	return true;
#else
	return false;
#endif
}

double NewProjectAudioProcessor::getTailLengthSeconds() const
{
	return 0.0;
}

int NewProjectAudioProcessor::getNumPrograms()
{
	return 1;
}

int NewProjectAudioProcessor::getCurrentProgram()
{
	return 0;
}

void NewProjectAudioProcessor::setCurrentProgram(int index)
{
}

const juce::String NewProjectAudioProcessor::getProgramName(int index)
{
	return {};
}

void NewProjectAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
}

//==============================================================================
void NewProjectAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
	currentSampleRate = sampleRate;
	totalSamplesProcessed = 0;
	audFallback.init(sampleRate);

	// Open (or re-open) the shared memory region for this group.
	// SharedGroupMemory::open() calls close() internally, so re-entrant calls are safe.
	if (!shm.open(groupName.toStdString()))
		juce::Logger::writeToLog("AUTOSYNC: shm.open() failed for group \"" + groupName + "\"");
	else
		juce::Logger::writeToLog("AUTOSYNC: shm open OK  group=\"" + groupName
		    + "\"  mode=" + (pluginMode == PluginMode::Master ? "Master" : "Slave")
		    + "  slot=" + juce::String(slotId));
}

void NewProjectAudioProcessor::releaseResources()
{
	shm.close();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool NewProjectAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
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


inline void NewProjectAudioProcessor::processTimeCode(const float& sample, tc_data& channel, std::string& msg,
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

void NewProjectAudioProcessor::pushAudioAnalysisSample(float ch1, float ch2)
{
	if (audFallback.novelty1.empty())
		return;

	audFallback.energyAcc1 += ch1 * ch1;
	audFallback.energyAcc2 += ch2 * ch2;

	if (++audFallback.hopSampleCounter < audFallback.hopSamples)
		return;

	// Hop boundary: compute novelty = positive energy increase
	float e1 = audFallback.energyAcc1 / (float)audFallback.hopSamples;
	float e2 = audFallback.energyAcc2 / (float)audFallback.hopSamples;

	float nov1 = std::max(0.0f, e1 - audFallback.prevEnergy1);
	float nov2 = std::max(0.0f, e2 - audFallback.prevEnergy2);

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

void NewProjectAudioProcessor::estimateAudioFallbackOffset()
{
	if (audFallback.framesFilled < audFallback.windowFrames)
		return;

	const int N = audFallback.windowFrames;

	// ------------------------------------------------------------------
	// Choose search mode.
	//
	// Anchored: LTC confirmed the offset recently and the anchor fits
	// inside the window.  Pre-shift masterNoveltyRef by anchorHops so the
	// correlation peak lands near relative lag 0; search only ±NARROW_HALF
	// hops (±300 ms).  Every lag in that range has ≥170 overlapping frames
	// vs. as few as 50 in the old wide search.
	//
	// Wide (fallback): no anchor or anchor too large for the window.
	// Behaviour identical to the previous implementation.
	// ------------------------------------------------------------------
	const bool anchorUsable =
		audFallback.hasMasterRef
		&& audFallback.hasAnchor
		&& (juce::Time::currentTimeMillis() - anchorTimestampMs) < (int64_t)ANCHOR_MAX_AGE_MS
		&& std::abs(audFallback.anchorHops) < N - AudioFallbackState::NARROW_HALF - 1;

	audFallback.lastEstimateAnchored = anchorUsable;

	// ------------------------------------------------------------------
	// Linearise novelty1 (slave, circular, always the same).
	// ------------------------------------------------------------------
	for (int i = 0; i < N; ++i)
	{
		int src = (audFallback.writePos + i) % N;
		audFallback.linBuf1[i] = audFallback.novelty1[src];
	}

	// ------------------------------------------------------------------
	// Linearise novelty2 / build reference for linBuf2.
	//
	// Anchored: pre-shift masterNoveltyRef right by anchorHops.
	//   linBuf2[i] = masterNoveltyRef[(i − K + 4N) % N]
	// This rotation aligns master and slave so that a residual lag of 0
	// means "offset equals anchorMs exactly".
	//
	// Wide: use masterNoveltyRef unshifted (or novelty2 if SM not yet up).
	// ------------------------------------------------------------------
	if (anchorUsable)
	{
		const int K = audFallback.anchorHops;
		for (int i = 0; i < N; ++i)
			audFallback.linBuf2[i] = audFallback.masterNoveltyRef[(i - K + N * 4) % N];
	}
	else
	{
		for (int i = 0; i < N; ++i)
		{
			int src = (audFallback.writePos + i) % N;
			audFallback.linBuf2[i] = audFallback.hasMasterRef
			                       ? audFallback.masterNoveltyRef[i]
			                       : audFallback.novelty2[src];
		}
	}

	// ------------------------------------------------------------------
	// Means + std devs
	// ------------------------------------------------------------------
	float m1 = 0.0f, m2 = 0.0f;
	for (int i = 0; i < N; ++i) { m1 += audFallback.linBuf1[i]; m2 += audFallback.linBuf2[i]; }
	m1 /= (float)N;
	m2 /= (float)N;

	float s1 = 0.0f, s2 = 0.0f;
	for (int i = 0; i < N; ++i)
	{
		float d1 = audFallback.linBuf1[i] - m1;
		float d2 = audFallback.linBuf2[i] - m2;
		s1 += d1 * d1;
		s2 += d2 * d2;
	}
	s1 = std::sqrt(s1 / (float)N);
	s2 = std::sqrt(s2 / (float)N);

	if (s1 < 1e-6f || s2 < 1e-6f)
	{
		audFallback.valid       = false;
		audFallback.stableCount = 0;
		return;
	}

	// ------------------------------------------------------------------
	// NCC helper — computes correlation at one lag value.
	// linBuf1/linBuf2, m1/m2, s1/s2, N captured by reference.
	// ------------------------------------------------------------------
	auto nccAt = [&](int lag) -> double
	{
		double sum = 0.0;
		int    cnt = 0;
		for (int i = 0; i < N; ++i)
		{
			int j = i + lag;
			if (j >= 0 && j < N)
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

	double bestCorr   = -2.0;
	double secondBest = -2.0;
	int    bestRelLag = 0;

	for (int lag = -searchHalf; lag <= searchHalf; ++lag)
	{
		double corr = nccAt(lag);
		if (corr > bestCorr)
		{
			secondBest = bestCorr;
			bestCorr   = corr;
			bestRelLag = lag;
		}
		else if (corr > secondBest)
		{
			secondBest = corr;
		}
	}

	// ------------------------------------------------------------------
	// Sub-hop parabola interpolation — sharpens accuracy from ±10 ms to
	// roughly ±2 ms at no extra window cost.
	// Only applied when the peak is not at the search boundary.
	// ------------------------------------------------------------------
	double subHopOffset = 0.0;
	if (bestRelLag > -searchHalf && bestRelLag < searchHalf)
	{
		double y0    = nccAt(bestRelLag - 1);
		double y1    = bestCorr;
		double y2    = nccAt(bestRelLag + 1);
		double denom = y2 - 2.0 * y1 + y0;
		if (std::abs(denom) > 1e-9)
			subHopOffset = std::max(-0.5, std::min(0.5, -0.5 * (y2 - y0) / denom));
	}

	// ------------------------------------------------------------------
	// Convert relative lag → absolute lag → milliseconds.
	//
	// Anchored: relative lag 0 corresponds to anchorHops (the confirmed
	// LTC offset), so absolute = relLag − anchorHops.
	//
	// Wide: relative == absolute (no shift was applied).
	//
	// Sign convention (preserved from original):
	//   bestLag < 0  →  master leads slave  →  deltaAudMs > 0  (slave is delayed)
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

void NewProjectAudioProcessor::fuseLtcAndAudioFallback()
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

	if (ltcOk)
	{
		// Keep the anchor up to date while LTC is healthy so it is ready
		// the moment LTC degrades.
		audFallback.anchorMs   = d_ms;
		audFallback.anchorHops = (int)std::round(d_ms / audFallback.hopMs);
		audFallback.hasAnchor  = true;
		anchorTimestampMs      = juce::Time::currentTimeMillis();

		fusion.source         = FusionState::Source::LTC;
		fusion.selectedMs     = d_ms;
		fusion.selectedConf   = (double)chnl1_in.Q_LTC;
		fusion.fallbackActive = false;
	}
	else
	{
		// Use a tighter confidence threshold when the estimator ran in anchored
		// mode: the narrow search window produces fewer false peaks.
		const double confThresh = audFallback.lastEstimateAnchored ? 0.25 : 0.4;

		if (audFallback.valid && audFallback.confAud > confThresh)
		{
			fusion.source         = FusionState::Source::AudioFallback;
			fusion.selectedMs     = audFallback.deltaAudMs;
			fusion.selectedConf   = audFallback.confAud;
			fusion.fallbackActive = true;
		}
		else
		{
			fusion.source         = FusionState::Source::None;
			fusion.selectedMs     = 0.0;
			fusion.selectedConf   = 0.0;
			fusion.fallbackActive = false;
		}
	}
}

void NewProjectAudioProcessor::writeMasterSlot()
{
	MasterSlot& m = shm.get()->master;

	// Timecode to milliseconds; frms converted at the current FPS setting
	const int64_t tc_ms = (int64_t)chnl1_in.hrs   * 3600000LL
	                    + (int64_t)chnl1_in.mnts   * 60000LL
	                    + (int64_t)chnl1_in.scnds  * 1000LL
	                    + (int64_t)chnl1_in.frms   * 1000LL / std::max(1, fps);

	const int N = audFallback.windowFrames;   // 200

	seqcount_write_begin(m.writeSeq);

	m.tc_ref_ms           = tc_ms;
	m.ref_decode_sample   = chnl1_in.last_decode_sample;
	m.Q_ref               = chnl1_in.Q_LTC;
	m.ltc_state        = (uint8_t)chnl1_in.ltc_state;
	m.valid            = (audFallback.framesFilled >= N);
	m.nov_writePos     = audFallback.writePos;
	m.nov_framesFilled = audFallback.framesFilled;

	// Copy the full novelty1 circular buffer verbatim; the slave uses
	// nov_writePos to know where the oldest frame sits.
	for (int i = 0; i < N; ++i)
		m.nov_ref[i] = audFallback.novelty1[i];

	seqcount_write_end(m.writeSeq);

	++shmWriteCount;
}

void NewProjectAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
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
			// Read from the designated LTC channel; always decode into chnl1_in.
			float ltcSample = (ltcChannel == 0) ? write1[i] : write2[i];
			processTimeCode(ltcSample, chnl1_in, input_ch1, i, (float)currentSampleRate, fpsIndex, bufferStartSample + i);

			// Novelty extraction for the reference curve.
			// ch2 = 0.0f — novelty2 is unused in master mode (removed in Step 11).
			pushAudioAnalysisSample(ltcSample, 0.0f);

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
		// Fill const_buf for BOTH channels so delay() can read from them immediately
		// (without this, the channel whose const_buf is empty outputs silence for
		// the first delay_size samples while its delay_buf fills up).
		handle_const_delay(write1[i], chnl1);
		handle_const_delay(write2[i], chnl2);

		// Decode own LTC from the configured channel only (pre-delay)
		float ltcSample = (ltcChannel == 0) ? write1[i] : write2[i];
		processTimeCode(ltcSample, chnl1_in, input_ch1, i, (float)currentSampleRate, fpsIndex, bufferStartSample + i);

		// Slave novelty1 tracks the same LTC channel as the master reference curve.
		// Novelty2 is not accumulated here — it is overwritten from master SM
		// (linearised nov_ref[]) in the 0.1s diagnostic block below.
		pushAudioAnalysisSample(ltcSample, 0.0f);

		// Target offset: master-derived d_ms (updated every ~0.1s in timer block below).
		// If master SM is stale (holding == true) or not yet received, freeze at last
		// programmed delay rather than snapping to zero.
		double targetMs;
		if (masterValid)
			targetMs = d_ms;
		else if (fusion.fallbackActive && fusion.selectedMs != 0.0)
			targetMs = fusion.selectedMs;  // audio fallback NCC estimate
		else if (activeDelayMs != 0.0)
			targetMs = activeDelayMs;  // hold-last-delay until master comes back
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
				int64_t tc_ref_ms_local        = 0;
				int64_t ref_decode_sample_local = 0;
				uint8_t master_ltc_state       = 0;
				bool    master_valid_local      = false;
				int     nov_writePos_local      = 0;
				int     nov_framesFilled_local  = 0;
				float   nov_ref_local[200]      = {};

				uint32_t seq1, seq2;
				do {
					seq1 = seqcount_read_begin(m.writeSeq);
					tc_ref_ms_local         = m.tc_ref_ms;
					ref_decode_sample_local = m.ref_decode_sample;
					master_ltc_state        = m.ltc_state;
					master_valid_local      = m.valid;
					nov_writePos_local      = m.nov_writePos;
					nov_framesFilled_local  = m.nov_framesFilled;
					for (int ni = 0; ni < audFallback.windowFrames; ++ni)
						nov_ref_local[ni] = m.nov_ref[ni];
					seq2 = seqcount_read_end(m.writeSeq);
				} while (seq1 != seq2);

				// Linearise master's circular novelty buffer (oldest→newest) into
				// audFallback.masterNoveltyRef so that estimateAudioFallbackOffset()
				// can cross-correlate slave transients against master transients.
				if (nov_framesFilled_local >= audFallback.windowFrames)
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

				if (master_valid_local && master_ltc_state >= 1 && tc_ref_ms_local != 0)
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
						newDtMs = (double)(tc_self_ms_local - tc_ref_ms_local);
					}

					// Jump detection: reset delay engine if master offset jumps > 2 frames
					const double frameMs = 1000.0 / fps;
					if (activeDelayMs != 0.0 && std::abs(newDtMs - d_ms) > 2.0 * frameMs)
					{
						chnl1.clear();
						chnl2.clear();
						activeDelayMs = 0.0;
					}

					d_ms              = newDtMs;
					lastMasterWriteMs = juce::Time::currentTimeMillis();
					masterValid       = true;
					holding           = false;
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
bool NewProjectAudioProcessor::hasEditor() const
{
	return true;
}

juce::AudioProcessorEditor* NewProjectAudioProcessor::createEditor()
{
	return new NewProjectAudioProcessorEditor(*this);
}

//==============================================================================
void NewProjectAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
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

void NewProjectAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
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
	by_slider    = xml->getDoubleAttribute("by_slider", 0.0);
	// shm will be (re-)opened on the next prepareToPlay call.
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new NewProjectAudioProcessor();
}
