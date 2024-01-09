/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/**
*/
class NewProjectAudioProcessorEditor : public juce::AudioProcessorEditor, public juce::Timer
{
public:
	NewProjectAudioProcessorEditor(NewProjectAudioProcessor&);
	~NewProjectAudioProcessorEditor() override;

	//==============================================================================
	void paint(juce::Graphics&) override;
	void resized() override;
	void timerCallback() override;

private:
	// This reference is provided as a quick way for your editor to
	// access the processor object that created it.
	NewProjectAudioProcessor& audioProcessor;

	juce::Label timecode_box;
	juce::Label timecode_box_chanel2;
	juce::Label delay_box;
	juce::Label timecode_input1;
	juce::Label timecode_input2;

	//in out in_delay labels
	juce::Label input;
	juce::Label output;
	juce::Label delay;


	//delay button and state label
	juce::TextButton delay_button;
	juce::Label delay_state_label;

	//output delay box
	juce::Label o_delay_box;
	juce::Label o_delay;

	//slider
	juce::Slider delay_slider;

	//MIDI out
	juce::Label show_MIDI;
	juce::Label value_MIDI;

	//version label
	juce::Label version;

	//fps buttons
	juce::ComboBox fps_box;
	juce::Label fps_label;



	double delay_by_slider = 0;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NewProjectAudioProcessorEditor)
};
