/*
  ==============================================================================
    PluginEditor.h  —  UI redesign, layout phase
    Design reference: 1500 × 900 px (scales proportionally)
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

// ============================================================
// Spacing constants (logical px at reference size)
// ============================================================
constexpr int PAD         = 24;
constexpr int GAP_SMALL   =  6;
constexpr int GAP_MED     = 12;
constexpr int GAP_LARGE   = 16;
constexpr int GAP_SECTION = 20;
constexpr int GAP_H       = 24;   // horizontal card gap

// ============================================================
// BadgeComponent — pill label; background drawn in styling phase
// ============================================================
class BadgeComponent : public juce::Component
{
public:
    explicit BadgeComponent(const juce::String& text = {});
    void setText(const juce::String& t);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::Label label;
};

// ============================================================
// HeaderBar  (row 1 of the top-level stack)
// ============================================================
class HeaderBar : public juce::Component
{
public:
    HeaderBar();
    void resized() override;

    void setMode(const juce::String& m)       { modeBadge.setText(m); }
    void setGroup(const juce::String& g)      { groupLabel.setText("GROUP: " + g, juce::dontSendNotification); }
    void setSyncStatus(const juce::String& s) { syncBadge.setText(s); }
    void setSlot(const juce::String& s)       { slotBadge.setText(s); }
    void setSource(const juce::String& s)     { sourceBadge.setText(s); }
    void setFps(const juce::String& f)        { fpsBadge.setText(f); }

private:
    juce::Label    titleLabel;   // "LTC SYNC PANEL"
    BadgeComponent modeBadge;    // MASTER / SLAVE
    juce::Label    groupLabel;   // GROUP: …
    BadgeComponent syncBadge;    // centered sync status
    BadgeComponent slotBadge;    // right group
    BadgeComponent sourceBadge;
    BadgeComponent fpsBadge;
};

// ============================================================
// SignalCard  — left card: timecodes + FPS selector
// ============================================================
class SignalCard : public juce::Component
{
public:
    SignalCard();
    void paint(juce::Graphics& g) override;
    void resized() override;

    // §11 hook: dim timecodes when audio fallback is active (impl in styling phase)
    void setLtcActive(bool active);

    void setInputTimecode(const juce::String& tc) { inputTimecode.setText(tc, juce::dontSendNotification); }
    void setInputMeta(const juce::String& s)      { inputMeta.setText(s,  juce::dontSendNotification); }
    void setOutputTimecode(const juce::String& tc){ outputTimecode.setText(tc, juce::dontSendNotification); }

    juce::ComboBox fpsBox;

private:
    // INPUT side
    juce::Label inputLabel;
    juce::Label inputTimecode;
    juce::Label inputMeta;

    // OUTPUT side
    juce::Label outputLabel;
    juce::Label outputTimecode;

    // FPS row
    juce::Label fpsRowLabel;   // "FRAME RATE SELECTION"

    // Divider geometry stored in resized(), drawn in paint()
    juce::Rectangle<int> vertDivider;   // §4.1 vertical between INPUT / OUTPUT
    juce::Rectangle<int> horizDivider;  // §4.2 horizontal between timecode row / FPS row
};

// ============================================================
// DelaySyncCard  — right card: delay metrics + toggle + slider
// ============================================================
class DelaySyncCard : public juce::Component
{
public:
    DelaySyncCard();
    void resized() override;

    // §11 hook: disable card when mode is Master (impl in styling phase)
    void setMasterMode(bool isMaster);

    void setDelayMs(const juce::String& v)     { delayMsValue.setText(v,     juce::dontSendNotification); }
    void setDelayFrames(const juce::String& v) { delayFramesValue.setText(v, juce::dontSendNotification); }
    void setMidiMs(const juce::String& v)      { midiMsValue.setText(v,      juce::dontSendNotification); }
    void setToggleActive(bool active)          { delayToggle.setButtonText(active ? "ON" : "OFF"); }
    void setManualValue(const juce::String& s) { sliderValue.setText(s,      juce::dontSendNotification); }

    juce::TextButton delayToggle;
    juce::Slider     manualSlider;

private:
    juce::Label titleLabel;        // "DELAY & SYNC"

    // Metric rows
    juce::Label delayMsLabel,     delayMsValue;
    juce::Label delayFramesLabel, delayFramesValue;
    juce::Label midiMsLabel,      midiMsValue;

    // Toggle row
    juce::Label toggleLabel;       // "DELAY TOGGLE"

    // Slider row
    juce::Label sliderLabel;       // "MANUAL CORRECTION"
    juce::Label sliderValue;       // live value echo
};

// ============================================================
// DiagnosticsCard  — four-column diagnostics panel
// ============================================================
class DiagnosticsCard : public juce::Component
{
public:
    DiagnosticsCard();
    void resized() override;

    void setSummary(float q, float fps, double drift, bool fallback);
    void setCh1(const juce::String& t)       { ch1Content.setText(t, juce::dontSendNotification); }
    void setCh2(const juce::String& t)       { ch2Content.setText(t, juce::dontSendNotification); }
    void setSyncEngine(const juce::String& t){ syncContent.setText(t, juce::dontSendNotification); }
    void setAudioClock(const juce::String& t){ audioContent.setText(t, juce::dontSendNotification); }

private:
    // §6.1 summary row (4 cells)
    juce::Label summaryQ, summaryFps, summaryDrift, summaryFallback;

    // §6.2 column titles
    juce::Label ch1Title, ch2Title, syncTitle, audioTitle;

    // §6.2 column content
    juce::Label ch1Content, ch2Content, syncContent, audioContent;

    // §6.3 footer
    juce::Label footerLabel;
};

// ============================================================
// ConfigSection  — device configuration controls
// ============================================================
class ConfigSection : public juce::Component
{
public:
    ConfigSection();
    void resized() override;

    // All controls public — wired to processor in PluginEditor ctor
    juce::ComboBox   modeBox;
    juce::TextEditor groupEditor;
    juce::ComboBox   slotBox;
    juce::TextButton ltcChL, ltcChR;   // toggle group (radio, group id 1)
    juce::TextEditor labelEditor;

private:
    juce::Label titleLabel;   // "DEVICE CONFIGURATION"
    juce::Label modeLabel, groupLabel, slotLabel, ltcChLabel, labelLabel;
};

// ============================================================
// Main editor
// ============================================================
class NewProjectAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       public juce::Timer
{
public:
    explicit NewProjectAudioProcessorEditor(NewProjectAudioProcessor&);
    ~NewProjectAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    NewProjectAudioProcessor& audioProcessor;

    HeaderBar       header;
    SignalCard      signalCard;
    DelaySyncCard   delayCard;
    DiagnosticsCard diagCard;
    ConfigSection   configSection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NewProjectAudioProcessorEditor)
};
