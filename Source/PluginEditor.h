/*
  ==============================================================================
    PluginEditor.h  -  UI redesign, styling phase
    Design reference: 900 × 620 px (scales proportionally)
  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

// ============================================================
// Spacing constants
// ============================================================
constexpr int PAD         = 12;
constexpr int GAP_SMALL   =  4;
constexpr int GAP_MED     =  8;
constexpr int GAP_LARGE   = 10;
constexpr int GAP_SECTION = 10;
constexpr int GAP_H       = 12;

// ============================================================
// Theme - colours and typography (defined in .cpp)
// ============================================================
namespace Theme
{
    // Colours - ARGB
    inline const juce::Colour bg          = juce::Colour(0xFF1C1C1E);  // window background
    inline const juce::Colour cardBg      = juce::Colour(0xFF2C2C2E);  // card fill
    inline const juce::Colour cardBorder  = juce::Colour(0xFF3A3A3C);  // card outline
    inline const juce::Colour divider     = juce::Colour(0xFF3A3A3C);  // inner dividers
    inline const juce::Colour valueBg     = juce::Colour(0xFF1C1C1E);  // value-box sunken bg
    inline const juce::Colour valueBorder = juce::Colour(0xFF48484A);

    inline const juce::Colour textPrimary = juce::Colour(0xFFFFFFFF);  // timecode, values
    inline const juce::Colour textTitle   = juce::Colour(0xFFEBEBF5);  // section/card titles
    inline const juce::Colour textLabel   = juce::Colour(0xFF8E8E93);  // field labels
    inline const juce::Colour textMeta    = juce::Colour(0xFF636366);  // sub-text, metadata

    inline const juce::Colour badgeBg     = juce::Colour(0xFF48484A);  // neutral badge
    inline const juce::Colour accentGreen = juce::Colour(0xFF30D158);  // SYNC / active
    inline const juce::Colour accentOrange= juce::Colour(0xFFFF9F0A);  // HOLD / warn
    inline const juce::Colour accentRed   = juce::Colour(0xFFFF453A);  // FAIL / error

    // Geometry
    constexpr float cardRadius  = 12.0f;
    constexpr float badgeRadius = 10.0f;   // true pill when badge height ≈ 20px

    // Font sizes
    constexpr float fontTimecode = 26.0f;  // dominant primary value
    constexpr float fontValue    = 13.0f;  // metric values in right card
    constexpr float fontTitle    = 13.0f;  // card / section titles
    constexpr float fontLabel    = 11.0f;  // field labels
    constexpr float fontMeta     = 11.0f;  // sub-text, diagnostics content
    constexpr float fontBadge    = 11.0f;  // badge labels
}

// ============================================================
// BadgeComponent - pill-shaped label
// ============================================================
class BadgeComponent : public juce::Component
{
public:
    explicit BadgeComponent(const juce::String& text = {});
    void setText(const juce::String& t);
    void setBadgeColour(juce::Colour c);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::Label    label;
    juce::Colour   badgeColour { Theme::badgeBg };
};

// ============================================================
// HeaderBar
// ============================================================
class HeaderBar : public juce::Component
{
public:
    HeaderBar();
    void resized() override;

    void setMode(const juce::String& m)   { modeBadge.setText(m); }
    void setGroup(const juce::String& g)  { groupLabel.setText("GROUP: " + g, juce::dontSendNotification); }
    void setSyncBadge(const juce::String& text, juce::Colour colour);
    void setSlot(const juce::String& s)   { slotBadge.setText(s); }
    void setSource(const juce::String& s) { sourceBadge.setText(s); }
    void setFps(const juce::String& f)    { fpsBadge.setText(f); }

private:
    juce::Label    titleLabel;
    BadgeComponent modeBadge;
    juce::Label    groupLabel;
    BadgeComponent syncBadge;
    BadgeComponent slotBadge;
    BadgeComponent sourceBadge;
    BadgeComponent fpsBadge;
};

// ============================================================
// SignalCard  - left card: timecodes + FPS selector
// ============================================================
class SignalCard : public juce::Component
{
public:
    SignalCard();
    void paint(juce::Graphics& g) override;
    void resized() override;

    void setLtcActive(bool active);

    void setInputTimecode(const juce::String& tc) { inputTimecode.setText(tc, juce::dontSendNotification); }
    void setInputMeta(const juce::String& s)      { inputMeta.setText(s,  juce::dontSendNotification); }
    void setOutputTimecode(const juce::String& tc){ outputTimecode.setText(tc, juce::dontSendNotification); }

    juce::ComboBox fpsBox;

private:
    juce::Label inputLabel, inputTimecode, inputMeta;
    juce::Label outputLabel, outputTimecode;
    juce::Label fpsRowLabel;

    juce::Rectangle<int> vertDivider;
    juce::Rectangle<int> horizDivider;
};

// ============================================================
// DelaySyncCard  - right card: delay metrics + toggle + slider
// ============================================================
class DelaySyncCard : public juce::Component
{
public:
    DelaySyncCard();
    void paint(juce::Graphics& g) override;
    void resized() override;

    void setMasterMode(bool isMaster);

    void setDelayMs(const juce::String& v)     { delayMsValue.setText(v,     juce::dontSendNotification); }
    void setDelayFrames(const juce::String& v) { delayFramesValue.setText(v, juce::dontSendNotification); }
    void setMidiMs(const juce::String& v)      { midiMsValue.setText(v,      juce::dontSendNotification); }
    void setToggleActive(bool active)          { delayToggle.setButtonText(active ? "ON" : "OFF"); }
    void setManualValue(const juce::String& s) { sliderValue.setText(s,      juce::dontSendNotification); }

    juce::TextButton delayToggle;
    juce::Slider     manualSlider;
    juce::TextButton resetButton;

private:
    juce::Label titleLabel;
    juce::Label delayMsLabel,     delayMsValue;
    juce::Label delayFramesLabel, delayFramesValue;
    juce::Label midiMsLabel,      midiMsValue;
    juce::Label toggleLabel;
    juce::Label sliderLabel;
    juce::Label sliderValue;
};

// ============================================================
// DiagnosticsCard
// ============================================================
class DiagnosticsCard : public juce::Component
{
public:
    DiagnosticsCard();
    void paint(juce::Graphics& g) override;
    void resized() override;

    void setSummary(float q, float fps, double drift, bool fallback);
    void setCh1(const juce::String& t)       { ch1Content.setText(t, juce::dontSendNotification); }
    void setCh2(const juce::String& t)       { ch2Content.setText(t, juce::dontSendNotification); }
    void setSyncEngine(const juce::String& t){ syncContent.setText(t, juce::dontSendNotification); }
    void setAudioClock(const juce::String& t){ audioContent.setText(t, juce::dontSendNotification); }

private:
    juce::Label summaryQ, summaryFps, summaryDrift, summaryFallback;
    juce::Label ch1Title, ch2Title, syncTitle, audioTitle;
    juce::Label ch1Content, ch2Content, syncContent, audioContent;
    juce::Label footerLabel;

    // Column separator geometry (set in resized, drawn in paint)
    std::array<int, 3> colSepX {};
    int colSepY1 = 0, colSepY2 = 0;
    juce::Rectangle<int> summaryDivider;
};

// ============================================================
// ConfigSection
// ============================================================
class ConfigSection : public juce::Component
{
public:
    ConfigSection();
    void resized() override;

    juce::ComboBox   modeBox;
    juce::TextEditor groupEditor;
    juce::ComboBox   slotBox;
    juce::TextButton ltcChL, ltcChR;
    juce::TextEditor labelEditor;

private:
    juce::Label titleLabel;
    juce::Label modeLabel, groupLabel, slotLabel, ltcChLabel, labelLabel;
};

// ============================================================
// Main editor
// ============================================================
class AutoSyncAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       public juce::Timer
{
public:
    explicit AutoSyncAudioProcessorEditor(AutoSyncAudioProcessor&);
    ~AutoSyncAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    AutoSyncAudioProcessor& audioProcessor;

    HeaderBar       header;
    SignalCard      signalCard;
    DelaySyncCard   delayCard;
    DiagnosticsCard diagCard;
    ConfigSection   configSection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoSyncAudioProcessorEditor)
};
