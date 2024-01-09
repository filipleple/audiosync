/*
  ==============================================================================

	This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <deque>

//==============================================================================
static int delay_frames = 0;

inline void delay(float* writePtr, const int& index, tc_data& data)
{

	if(data.delay_size < data.const_buf.size())
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
		return 0;
	}

	if (delay_ms < -10000)
	{
		++data1.timecode_counter;
		return 0;
	}

	data1.timecode_counter = 0;
	data2.timecode_counter = 0;

	delay_frames = time2_in_frames - time1_in_frames;
	return delay_ms;
}

inline void handleTimecode(const long double& sample, tc_data& data, const int& srate, const int& slider = 0)
{
	static const double frates[] = {30, 24, 25, 30000.0 / 1001};


	//pulsesize is depending of srate and fps
	data.pulsesize = srate / frates[slider] / 160;


	// remove DC offset
	data.otm1 = 0.999 * data.otm1 + sample - data.itm1;
	data.itm1 = sample;

	const long double s = data.otm1;

	++data.sillen;

	if (data.sillen > data.pulsesize * 2.2)
	{
		data.syncpos = -1;
		data.sillen = 0;
		data.gotbit = -1;
		data.syncstate = 1;
	}

	data.threshold = data.threshold * data.threshenv + std::abs(s) * (1 - data.threshenv);
	if (data.threshold < data.minthresh)
	{
		data.threshold = data.minthresh;
	}

	if ((s < -data.threshold * 0.8 && data.lastsign > 0) || (s > data.threshold * 0.8 && data.lastsign < 0))
	{
		data.lastsign *= -1;
		++data.gotbit;
		if (data.sillen > data.pulsesize * 1.8)
		{
			data.gotbit = std::min(data.gotbit, 1);
			data.sillen = 0;

			data.buf[data.bufpos] = data.gotbit;

			if (++data.bufpos >= 80)
			{
				data.bufpos = 0;
			}

			if (data.syncpos >= 0)
			{
				data.syncpos++;
			}

			if (data.syncpos < 0 || data.syncpos >= 80)
			{
				data.syncpos = -1;

				// try to get sync word
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

					data.new_time = ((data.hrs * 80 + data.mnts) * 80 + data.scnds) * 100 + data.frms;
				}
				else
				{
					data.syncstate = 0;
				}
			}
			data.gotbit = -1;
		}
	}
}

inline void handle_const_delay(const float& sample, tc_data& data)
{
	data.const_buf.push_back(sample);
	if (data.const_buf.size() == data.const_buf_size)
	{
		data.const_buf.pop_front();
	}
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

	//parameter initialized in constructor with these params
	addParameter(myParameter = new juce::AudioParameterFloat("myParam", "Delay, ms", 0.0f, 4000.0f, 0.0f));
}

NewProjectAudioProcessor::~NewProjectAudioProcessor()
{
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


	//return true;
	

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
	return 1; // NB: some hosts don't cope very well if you tell them there are 0 programs,
	// so this should be at least 1, even if you're not really implementing programs.
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
	// Use this method as the place to do any pre-playback
	// initialisation that you need..
}

void NewProjectAudioProcessor::releaseResources()
{
	// When playback stops, you can use this as an opportunity to free up any
	// spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool NewProjectAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
	juce::ignoreUnused(layouts);
	return true;
#else
	// This is the place where you check if the layout is supported.
	// In this template code we only support mono or stereo.
	// Some plugin hosts, such as certain GarageBand versions, will only
	// load plugins that support stereo bus layouts.
	if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
		&& layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
		return false;

	// This checks if the input layout matches the output layout
#if ! JucePlugin_IsSynth
	if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
		return false;
#endif

	return true;
#endif
}


inline void NewProjectAudioProcessor::processTimeCode(const float& sample, tc_data& channel, std::string& msg,
                                                      const int& index, const float& srate = 44100,
                                                      const int& slider = 0)
{
	handleTimecode(sample, channel, srate, slider);

	if (channel.new_time != channel.old_time)
	{
		channel.old_time = channel.new_time;


		msg = std::to_string(channel.hrs / 10) + std::to_string(channel.hrs % 10) + ":"
			+ std::to_string(channel.mnts / 10) + std::to_string(channel.mnts % 10) + ":"
			+ std::to_string(channel.scnds / 10) + std::to_string(channel.scnds % 10) + ":"
			+ std::to_string(channel.frms / 10) + std::to_string(channel.frms % 10);
	}
}
#endif

void NewProjectAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	juce::ScopedNoDenormals noDenormals;
	auto totalNumInputChannels = getTotalNumInputChannels();
	auto totalNumOutputChannels = getTotalNumOutputChannels();

	
	




	for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
	{
		buffer.clear(i, 0, buffer.getNumSamples());
	}

	
	auto* write1 = buffer.getWritePointer(0);
	auto* write2 = buffer.getWritePointer(1);

	
	
	//midilink


	//sending out (delay + slider value) as 2 7 bits values via midi link 
	
	int value = static_cast<int>(static_cast<float>(((std::abs(d_ms) + by_slider) / 10000) * 16383.0f ));
	if(value < 0)
	{
		value = 0;
	}
	int valueMSB = (value >> 7) & 0x7F; // Most Significant 7 bits
	int valueLSB = value & 0x7F; // Least Significant 7 bits

	//cc 14 bit 6/38
	midiMessages.addEvent(juce::MidiMessage::controllerEvent(1
		, 6, valueMSB), 0);
	midiMessages.addEvent(juce::MidiMessage::controllerEvent(1
		, 38, valueLSB), 0);


	//pitch
	midiMessages.addEvent(juce::MidiMessage::pitchWheel(1, value), 0);

	for (int i = 0; i < buffer.getNumSamples(); ++i)
	{
		handle_const_delay(write1[i], chnl1);


		if (fps == 30) {
			processTimeCode(write1[i], chnl1_in, input_ch1, i); //for input channel 1
			processTimeCode(write2[i], chnl2_in, input_ch2, i); // for input channel 2
		}
		else if(fps == 25)
		{
			processTimeCode(write1[i], chnl1_in, input_ch1, i, 44100, 2); //for input channel 1
			processTimeCode(write2[i], chnl2_in, input_ch2, i, 44100, 2); // for input channel 2


		}
		d_ms = calc_delay(chnl1_in, chnl2_in, fps);

		if (std::abs(prev_frames - delay_frames) > 1)
		{
			
			chnl1.clear();
			chnl2.clear();
		}
		else if(std::abs(prev_frames - delay_frames) == 1)
		{
				if (d_ms > prev_ms)
				{
					d_ms = prev_ms;
					delay_frames = prev_frames;
				}
		}


		delay_ms = std::to_string(std::abs(delay_frames));
		o_delay_ms = std::to_string(std::abs(d_ms));


		//we sent info via midi modulation(we sent delay + slider value)
		myParameter->setValueNotifyingHost(static_cast<float>((std::abs(d_ms) + std::floor(by_slider)) / 4000));


		if (active_delay)
		{
			

			


			if (d_ms > 0 && !chnl1.active_delay && !chnl2.active_delay) // if channel 2 is faster than channel 1
			{
				chnl2.active_delay = true;
				chnl1.active_delay = false;
			}
			if (d_ms < 0 && !chnl1.active_delay && !chnl2.active_delay) // if channel 1 is faster than channel 2
			{
				chnl2.active_delay = false;
				chnl1.active_delay = true;
			}


			if (chnl2.active_delay) //delay for channel 2
			{
				if (chnl2.delay_size == 0)
				{
					chnl2.delay_size = std::floor(d_ms / 1000 * 44100);
				}

				delay(write2, i, chnl2);
			}
			if (chnl1.active_delay) //delay for channel 1
			{
				if (!chnl1.delay_size)
				{
					chnl1.delay_size = std::floor(std::abs(d_ms) / 1000 * 44100);
				}
				
				
				delay(write1, i, chnl1);
				
				
			}
			
			if (fps == 30) {
				processTimeCode(write1[i], chnl1, tc, i); //for output channel1 
				processTimeCode(write2[i], chnl2, output_c2, i); //for output channel 2
			}
			else if(fps == 25)
			{
				processTimeCode(write1[i], chnl1, tc, i, 44100, 2); //for output channel1 
				processTimeCode(write2[i], chnl2, output_c2, i, 44100, 2); //for output channel 2
			}
		}
		else
		{
			tc = input_ch1;
			output_c2 = input_ch2;

			chnl1.clear();
			chnl2.clear();
		}
		prev_ms = d_ms;
		prev_frames = delay_frames;
	}
}

//==============================================================================
bool NewProjectAudioProcessor::hasEditor() const
{
	return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* NewProjectAudioProcessor::createEditor()
{
	return new NewProjectAudioProcessorEditor(*this);
}

//==============================================================================
void NewProjectAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
	// You should use this method to store your parameters in the memory block.
	// You could do that either as raw data, or use the XML or ValueTree classes
	// as intermediaries to make it easy to save and load complex data.
}

void NewProjectAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
	// You should use this method to restore your parameters from this memory block,
	// whose contents will have been created by the getStateInformation() call.
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new NewProjectAudioProcessor();
}
