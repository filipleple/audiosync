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
	addAndMakeVisible(qual_fallback_label);

	// -------------------------------------------------------------------------
	// Config panel — initialised from processor state (already restored by
	// setStateInformation before the editor is constructed).
	// -------------------------------------------------------------------------

	// Mode
	cfg_mode_label.setText("Mode", juce::dontSendNotification);
	addAndMakeVisible(cfg_mode_label);
	cfg_mode_box.addItem("Master", 1);   // id 1 → enum 0 (Master)
	cfg_mode_box.addItem("Slave",  2);   // id 2 → enum 1 (Slave)
	cfg_mode_box.setSelectedId((int)audioProcessor.pluginMode + 1, juce::dontSendNotification);
	cfg_mode_box.onChange = [&]()
	{
		audioProcessor.pluginMode = (PluginMode)(cfg_mode_box.getSelectedId() - 1);
	};
	addAndMakeVisible(cfg_mode_box);

	// Group name
	cfg_group_label.setText("Group", juce::dontSendNotification);
	addAndMakeVisible(cfg_group_label);
	cfg_group_editor.setText(audioProcessor.groupName, false);
	cfg_group_editor.setInputRestrictions(16,
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
	cfg_group_editor.onReturnKey = [&]()
	{
		juce::String g = cfg_group_editor.getText().trim();
		if (g.isEmpty()) { cfg_group_editor.setText(audioProcessor.groupName, false); return; }
		audioProcessor.groupName = g;
		audioProcessor.reopenShm();
	};
	cfg_group_editor.onFocusLost = [&]()
	{
		juce::String g = cfg_group_editor.getText().trim();
		if (g.isEmpty()) { cfg_group_editor.setText(audioProcessor.groupName, false); return; }
		if (g != audioProcessor.groupName)
		{
			audioProcessor.groupName = g;
			audioProcessor.reopenShm();
		}
	};
	addAndMakeVisible(cfg_group_editor);

	// Slot ID (1-8, slave only)
	cfg_slot_label.setText("Slot", juce::dontSendNotification);
	addAndMakeVisible(cfg_slot_label);
	for (int i = 1; i <= 8; ++i)
		cfg_slot_box.addItem(juce::String(i), i);
	cfg_slot_box.setSelectedId(audioProcessor.slotId, juce::dontSendNotification);
	cfg_slot_box.onChange = [&]()
	{
		audioProcessor.slotId = cfg_slot_box.getSelectedId();
	};
	addAndMakeVisible(cfg_slot_box);

	// LTC channel (L / R)
	cfg_ltcch_label.setText("LTC ch", juce::dontSendNotification);
	addAndMakeVisible(cfg_ltcch_label);
	cfg_ltcch_box.addItem("L", 1);
	cfg_ltcch_box.addItem("R", 2);
	cfg_ltcch_box.setSelectedId(audioProcessor.ltcChannel + 1, juce::dontSendNotification);
	cfg_ltcch_box.onChange = [&]()
	{
		audioProcessor.ltcChannel = cfg_ltcch_box.getSelectedId() - 1;
	};
	addAndMakeVisible(cfg_ltcch_box);

	// Slot label (free text, slave only)
	cfg_label_label.setText("Label", juce::dontSendNotification);
	addAndMakeVisible(cfg_label_label);
	cfg_label_editor.setText(audioProcessor.slotLabel, false);
	cfg_label_editor.setInputRestrictions(31);
	cfg_label_editor.onTextChange = [&]()
	{
		audioProcessor.slotLabel = cfg_label_editor.getText();
	};
	addAndMakeVisible(cfg_label_editor);

	setSize(400, 480);
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
	g.drawHorizontalLine(401, 4.0f, (float)getWidth() - 4.0f);
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

	qual_fallback_label.setSize(400, 16);
	qual_fallback_label.setTopLeftPosition(4, 379);
	qual_fallback_label.moved();

	// Config panel — two rows below the second separator (y=401)
	// Row 1: Mode | Group | Slot | LTC ch
	const int cfgY1label = 406;   // mini-label row
	const int cfgY1ctrl  = 420;   // control row
	const int cfgCtrlH   = 24;

	cfg_mode_label.setSize(40, 13);
	cfg_mode_label.setTopLeftPosition(4, cfgY1label);

	cfg_mode_box.setSize(65, cfgCtrlH);
	cfg_mode_box.setTopLeftPosition(4, cfgY1ctrl);

	cfg_group_label.setSize(45, 13);
	cfg_group_label.setTopLeftPosition(74, cfgY1label);

	cfg_group_editor.setSize(100, cfgCtrlH);
	cfg_group_editor.setTopLeftPosition(74, cfgY1ctrl);

	cfg_slot_label.setSize(30, 13);
	cfg_slot_label.setTopLeftPosition(180, cfgY1label);

	cfg_slot_box.setSize(42, cfgCtrlH);
	cfg_slot_box.setTopLeftPosition(180, cfgY1ctrl);

	cfg_ltcch_label.setSize(48, 13);
	cfg_ltcch_label.setTopLeftPosition(228, cfgY1label);

	cfg_ltcch_box.setSize(42, cfgCtrlH);
	cfg_ltcch_box.setTopLeftPosition(228, cfgY1ctrl);

	// Row 2: Label (full width)
	const int cfgY2label = 450;
	const int cfgY2ctrl  = 464;

	cfg_label_label.setSize(40, 13);
	cfg_label_label.setTopLeftPosition(4, cfgY2label);

	cfg_label_editor.setSize(388, cfgCtrlH);
	cfg_label_editor.setTopLeftPosition(4, cfgY2ctrl);
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

	{
		const char* srcNames[] = { "NONE", "LTC", "AUD" };
		int src = std::clamp(audioProcessor.aud_fusionSource, 0, 2);
		juce::String dtStr = audioProcessor.aud_conf > 0.01
			? juce::String(audioProcessor.aud_deltaMs, 0) + "ms"
			: "---";
		juce::String activeStr = audioProcessor.aud_activeDelayMs != 0.0
			? juce::String((int)std::round(audioProcessor.aud_activeDelayMs)) + "ms"
			: "off";
		qual_fallback_label.setText(
			juce::String("AUD: dt=") + dtStr +
			"  conf=" + juce::String(audioProcessor.aud_conf, 2) +
			"  src=" + srcNames[src] +
			"  applied=" + activeStr,
			juce::dontSendNotification);
	}

	repaint();
}
