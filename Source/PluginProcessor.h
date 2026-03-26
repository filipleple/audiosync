/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <math.h>
#include <deque>

//==============================================================================

class tc_data
{
public:
	int hrs = 0;
	int mnts = 0;
	int scnds = 0;
	int frms = 0;

	int* buf = new int[80]{0};

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
		++decoder_reset_count;
	}
};

class NewProjectAudioProcessor : public juce::AudioProcessor
#if JucePlugin_Enable_ARA
                             , public juce::AudioProcessorARAExtension
#endif
{
public:
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

public:
	//==============================================================================
	NewProjectAudioProcessor();
	~NewProjectAudioProcessor() override;

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
		const int& index, const float& sr, const int& sl);

private:
	tc_data chnl1;
	tc_data chnl2;
	tc_data chnl1_in;
	tc_data chnl2_in;

	// Δt rolling window for drift / channel_agreement detection
	std::deque<double> dt_history;
	int dt_sample_counter = 0;

	//==============================================================================
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NewProjectAudioProcessor)
	std::unique_ptr<juce::FileLogger> fileLogger;
};
