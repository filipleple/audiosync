/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <math.h>

//==============================================================================
/**
*/



class tc_data
{
public:
	int hrs = 0;
	int mnts = 0;
	int scnds = 0;
	int frms = 0;

	int* buf = new int[80]{0}; //buffer

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

	//delay

	std::deque<float> delay_buf;


	//delay if in range of const buffer
	std::deque<float> const_buf;

	const int const_buf_size = 500000;
	//

	bool active_delay = false;

	size_t delay_size = 0;
public:
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

	}

};

class NewProjectAudioProcessor : public juce::AudioProcessor
#if JucePlugin_Enable_ARA
                             , public juce::AudioProcessorARAExtension
#endif
{
public:
	
	std::string tc = "--:--:--:--";
	std::string output_c2 = "--:--:--:--";
	std::string delay_ms = "--";
	std::string input_ch1 = "--:--:--:--";
	std::string input_ch2 = "--:--:--:--";
	std::string o_delay_ms = "--";


	double prev_ms = 0;
	int prev_frames = 0;

	bool active_delay = false;//state of the button from gui

	double by_slider = 0;//delay that we recieve by slider in gui


	//parametr decalred for midi modulation link
	juce::AudioParameterFloat* myParameter;

	double d_ms = 0;//input delayý

	//fps
	int fps = 30;

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
	//==============================================================================
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NewProjectAudioProcessor)
};
