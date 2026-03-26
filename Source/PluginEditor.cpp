/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
NewProjectAudioProcessorEditor::NewProjectAudioProcessorEditor(NewProjectAudioProcessor& p)
	: AudioProcessorEditor(&p), audioProcessor(p)
{
	// Make sure that before the constructor has finished, you've set the
	// editor's size to whatever you need it to be.
	addAndMakeVisible(timecode_box);
	addAndMakeVisible(timecode_box_chanel2);
	addAndMakeVisible(delay_box);
	addAndMakeVisible(timecode_input1);
	addAndMakeVisible(timecode_input2);
	addAndMakeVisible(input);
	addAndMakeVisible(output);
	addAndMakeVisible(delay);
	addAndMakeVisible(delay_button);
	addAndMakeVisible(delay_state_label);
	addAndMakeVisible(o_delay);
	addAndMakeVisible(o_delay_box);

	addAndMakeVisible(delay_slider);
	delay_slider.setNumDecimalPlacesToDisplay(0);
	delay_slider.setRange(-1500, 1500);
	delay_slider.setValue(0);
	delay_slider.setMouseDragSensitivity(1);
	delay_slider.setTextValueSuffix("ms");

	addAndMakeVisible(show_MIDI);
	addAndMakeVisible(value_MIDI);

	addAndMakeVisible(version);

	


	delay_slider.onValueChange = [&]()
		{
			this->delay_by_slider = delay_slider.getValue();
			audioProcessor.by_slider = this->delay_by_slider;
		};


	delay_button.onClick = [&]()
		{
			if(audioProcessor.active_delay == false)
			{
				audioProcessor.active_delay = true;
			}
			else
			{
				audioProcessor.active_delay = false;
			}



		};
	//fps label
	fps_label.setText("30FPS", juce::dontSendNotification);
	addAndMakeVisible(fps_label);


	//fps box
	fps_box.addItem("30FPS", 1);
	fps_box.addItem("25FPS", 2);
	fps_box.setSelectedId(1);

	fps_box.onChange = [&]()
		{
			const auto id = fps_box.getSelectedId();

			if(id == 1)
			{
				audioProcessor.fps = 30;
				fps_label.setText("30FPS", juce::dontSendNotification);
			}
			else if(id == 2)
			{
				audioProcessor.fps = 25;
				fps_label.setText("25FPS", juce::dontSendNotification);
			}
		};


	addAndMakeVisible(fps_box);

	// Diagnostic labels
	qual_title.setText("--- QUALITY DIAGNOSTICS ---", juce::dontSendNotification);
	qual_title.setJustificationType(juce::Justification::centred);
	addAndMakeVisible(qual_title);
	addAndMakeVisible(qual_ch1_label);
	addAndMakeVisible(qual_ch2_label);
	addAndMakeVisible(qual_sync_label);

	setSize(400, 380);
	startTimer(1);
}

NewProjectAudioProcessorEditor::~NewProjectAudioProcessorEditor()
{
}

//==============================================================================
void NewProjectAudioProcessorEditor::paint(juce::Graphics& g)
{
	g.setColour(juce::Colours::grey);
	g.drawHorizontalLine(302, 4.0f, (float)getWidth() - 4.0f);
}

void NewProjectAudioProcessorEditor::resized()
{

	//delay slider
	delay_slider.setSize(150, 50);
	delay_slider.setTopLeftPosition(10, 120);
	delay_slider.moved();

	//delay label
	delay.setSize(100, 50);
	delay.setTopLeftPosition(160, 120);
	delay.moved();


	//delay box
	delay_box.setSize(100, 30);
	delay_box.setTopLeftPosition(160, 150);
	delay_box.moved();


	//input
	input.setSize(100, 50);
	input.setTopLeftPosition(10, 0);
	input.moved();

	timecode_input2.setSize(170, 75);
	timecode_input2.setTopLeftPosition(10, 30);
	timecode_input2.moved();

	timecode_input1.setSize(170, 75);
	timecode_input1.setTopLeftPosition(10, 10);
	timecode_input1.moved();


	//output
	output.setSize(100, 50);
	output.setTopRightPosition(390, 0);
	output.moved();

	timecode_box.setSize(170, 75);
	timecode_box.setTopRightPosition(450, 10);
	timecode_box.moved();

	timecode_box_chanel2.setSize(170, 75);
	timecode_box_chanel2.setTopRightPosition(450, 30);
	timecode_box_chanel2.moved();


	//delay
	delay_button.setButtonText("delay");
	delay_button.setSize(50, 30);
	delay_button.setTopRightPosition(220, 200);

	delay_state_label.setSize(100, 30);
	delay_state_label.setTopRightPosition(190, 200);
	delay_state_label.moved();

	o_delay.setText("IN DELAY, ms", juce::dontSendNotification);
	o_delay.setSize(100, 50);
	o_delay.setTopLeftPosition(160, 75);
	o_delay.moved();

	o_delay_box.setSize(100, 30);
	o_delay_box.setTopLeftPosition(160, 105);
	o_delay_box.moved();

	//delay to midi
	show_MIDI.setSize(100, 50);
	show_MIDI.setTopLeftPosition(290, 170);
	show_MIDI.moved();

	value_MIDI.setSize(170, 75);
	value_MIDI.setTopLeftPosition(290, 180);
	value_MIDI.moved();

	//version label
	version.setText("ver 1.4", juce::dontSendNotification);
	version.setSize(170, 75);
	version.setTopLeftPosition(290, 230);
	version.moved();


	//fps box and fps label
	fps_box.setSize(60, 30);
	fps_box.setTopLeftPosition(160, 250);
	fps_box.moved();

	fps_label.setSize(80, 50);
	fps_label.setTopLeftPosition(110, 240);
	fps_label.moved();

	qual_title.setSize(400, 16);
	qual_title.setTopLeftPosition(0, 306);
	qual_title.moved();

	qual_ch1_label.setSize(400, 16);
	qual_ch1_label.setTopLeftPosition(4, 325);
	qual_ch1_label.moved();

	qual_ch2_label.setSize(400, 16);
	qual_ch2_label.setTopLeftPosition(4, 343);
	qual_ch2_label.moved();

	qual_sync_label.setSize(400, 16);
	qual_sync_label.setTopLeftPosition(4, 361);
	qual_sync_label.moved();
}

void NewProjectAudioProcessorEditor::timerCallback()
{

	show_MIDI.setText("OUT MIDI, ms", juce::dontSendNotification);
	value_MIDI.setText(std::to_string(std::floor(audioProcessor.by_slider) + std::abs(audioProcessor.d_ms)), juce::dontSendNotification);

	input.setText("INPUT", juce::dontSendNotification);
	delay.setText("IN DELAY, frames", juce::dontSendNotification);
	output.setText("OUTPUT", juce::dontSendNotification);

	o_delay_box.setText(audioProcessor.o_delay_ms, juce::dontSendNotification);
	
	if(audioProcessor.active_delay)
	{
		delay_state_label.setText("ACTIVE", juce::dontSendNotification);
	}
	else
	{
		delay_state_label.setText("INACTIVE", juce::dontSendNotification);
	}

	timecode_box.setText(audioProcessor.tc, juce::dontSendNotification);
	timecode_box_chanel2.setText(audioProcessor.output_c2, juce::dontSendNotification);
	delay_box.setText(audioProcessor.delay_ms, juce::dontSendNotification);
	timecode_input1.setText(audioProcessor.input_ch1, juce::dontSendNotification);
	timecode_input2.setText(audioProcessor.input_ch2, juce::dontSendNotification);

	// Quality diagnostics
	const char* stateNames[] = { "FAIL", "SUSPECT", "VALID" };
	int s1 = std::clamp(audioProcessor.ch1_ltc_state, 0, 2);
	int s2 = std::clamp(audioProcessor.ch2_ltc_state, 0, 2);

	qual_ch1_label.setText(
		juce::String("CH1: ") + stateNames[s1] +
		"  Q=" + juce::String(audioProcessor.ch1_Q_LTC, 2) +
		"  fps=" + juce::String(audioProcessor.ch1_estimated_fps, 1) +
		"  Rst=" + juce::String(audioProcessor.ch1_decoder_resets) +
		"  Rej=" + juce::String(audioProcessor.ch1_rejected_frames),
		juce::dontSendNotification);

	qual_ch2_label.setText(
		juce::String("CH2: ") + stateNames[s2] +
		"  Q=" + juce::String(audioProcessor.ch2_Q_LTC, 2) +
		"  fps=" + juce::String(audioProcessor.ch2_estimated_fps, 1) +
		"  Rst=" + juce::String(audioProcessor.ch2_decoder_resets) +
		"  Rej=" + juce::String(audioProcessor.ch2_rejected_frames),
		juce::dontSendNotification);

	qual_sync_label.setText(
		juce::String("dt_dev=") + juce::String(audioProcessor.dt_deviation, 1) + "ms" +
		"  drift=" + juce::String(audioProcessor.drift_per_s, 2) + "ms/s" +
		"  Fallback=" + (audioProcessor.fallback_requested ? "YES" : "no"),
		juce::dontSendNotification);

	repaint();
}
