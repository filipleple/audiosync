/*
  ==============================================================================
    PluginEditor.cpp  —  UI redesign, layout phase
    All bounds set via Rectangle slicing; no hardcoded positions.
  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

// ============================================================
// BadgeComponent
// ============================================================

BadgeComponent::BadgeComponent(const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(label);
}

void BadgeComponent::setText(const juce::String& t)
{
    label.setText(t, juce::dontSendNotification);
}

void BadgeComponent::paint(juce::Graphics&)
{
    // Styling phase: draw rounded-rect pill background here
}

void BadgeComponent::resized()
{
    label.setBounds(getLocalBounds());
}

// ============================================================
// HeaderBar
// ============================================================

HeaderBar::HeaderBar()
{
    titleLabel.setText("LTC SYNC PANEL", juce::dontSendNotification);

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(modeBadge);
    addAndMakeVisible(groupLabel);
    addAndMakeVisible(syncBadge);
    addAndMakeVisible(slotBadge);
    addAndMakeVisible(sourceBadge);
    addAndMakeVisible(fpsBadge);
}

void HeaderBar::resized()
{
    auto area = getLocalBounds();
    const int w = area.getWidth();

    // §2: three zones  50% | 20% | 30%
    auto leftZone   = area.removeFromLeft(int(w * 0.50f));
    auto centerZone = area.removeFromLeft(int(w * 0.20f));
    auto rightZone  = area;   // remaining ~30%

    // --- LEFT: horizontal flow, gap = 10 ---
    // icon placeholder (24 × 24, vertically centered)
    leftZone.removeFromLeft(10);
    leftZone.removeFromLeft(24);   // icon reserved space
    leftZone.removeFromLeft(10);

    titleLabel.setBounds(leftZone.removeFromLeft(200));
    leftZone.removeFromLeft(10);

    modeBadge.setBounds(leftZone.removeFromLeft(80));
    leftZone.removeFromLeft(10);

    groupLabel.setBounds(leftZone);   // takes remainder

    // --- CENTER: sync badge, fully centered ---
    syncBadge.setBounds(centerZone.reduced(4, 4));

    // --- RIGHT: right-aligned badges, gap = 12 ---
    rightZone.removeFromRight(10);
    fpsBadge.setBounds(rightZone.removeFromRight(90));
    rightZone.removeFromRight(GAP_MED);
    sourceBadge.setBounds(rightZone.removeFromRight(80));
    rightZone.removeFromRight(GAP_MED);
    slotBadge.setBounds(rightZone.removeFromRight(90));
}

// ============================================================
// SignalCard
// ============================================================

SignalCard::SignalCard()
{
    inputLabel.setText("INPUT SIGNAL", juce::dontSendNotification);
    outputLabel.setText("OUTPUT SYNC",  juce::dontSendNotification);

    inputTimecode.setText("--:--:--:--",  juce::dontSendNotification);
    outputTimecode.setText("--:--:--:--", juce::dontSendNotification);
    inputMeta.setText("",  juce::dontSendNotification);

    fpsRowLabel.setText("FRAME RATE SELECTION", juce::dontSendNotification);

    fpsBox.addItem("30 FPS", 1);
    fpsBox.addItem("25 FPS", 2);

    addAndMakeVisible(inputLabel);
    addAndMakeVisible(inputTimecode);
    addAndMakeVisible(inputMeta);
    addAndMakeVisible(outputLabel);
    addAndMakeVisible(outputTimecode);
    addAndMakeVisible(fpsRowLabel);
    addAndMakeVisible(fpsBox);
}

void SignalCard::paint(juce::Graphics& g)
{
    g.setColour(juce::Colours::grey);

    // §4.1 vertical divider between INPUT and OUTPUT columns
    if (!vertDivider.isEmpty())
        g.fillRect(vertDivider);

    // §4.2 horizontal divider between timecode row and FPS row
    if (!horizDivider.isEmpty())
        g.fillRect(horizDivider);
}

void SignalCard::resized()
{
    auto inner = getLocalBounds().reduced(32);

    // §4.1  TimecodeRow: top ~45% of inner height
    const int tcHeight = int(inner.getHeight() * 0.45f);
    auto tcArea = inner.removeFromTop(tcHeight);

    // §4.2  Divider: GAP_LARGE margin above, 1 px line, GAP_LARGE below
    inner.removeFromTop(GAP_LARGE);
    horizDivider = inner.removeFromTop(1);
    inner.removeFromTop(GAP_LARGE);

    // §4.3  FPS row: remainder
    {
        auto fpsArea = inner;
        fpsRowLabel.setBounds(fpsArea.removeFromTop(20));
        fpsArea.removeFromTop(GAP_SMALL);
        fpsBox.setBounds(fpsArea.removeFromLeft(220).removeFromTop(28));
    }

    // --- TimecodeRow split 50 / 50 ---
    auto inputArea  = tcArea.removeFromLeft(tcArea.getWidth() / 2);
    auto outputArea = tcArea;

    // §4.1 store vertical divider geometry (1 px column at the boundary)
    vertDivider = juce::Rectangle<int>(inputArea.getRight(), inputArea.getY(),
                                       1, inputArea.getHeight());
    inputArea.removeFromRight(1);
    outputArea.removeFromLeft(1);

    // §4.1 INPUT side vertical layout:  label / gap / timecode(60%) / gap / meta
    {
        auto a = inputArea;
        inputLabel.setBounds(a.removeFromTop(20));
        a.removeFromTop(GAP_SMALL);
        const int tcH = int(a.getHeight() * 0.60f);
        inputTimecode.setBounds(a.removeFromTop(tcH));
        a.removeFromTop(4);
        inputMeta.setBounds(a.removeFromTop(20));
    }

    // §4.1 OUTPUT side vertical layout: label / gap / timecode(60%)
    {
        auto a = outputArea;
        outputLabel.setBounds(a.removeFromTop(20));
        a.removeFromTop(GAP_SMALL);
        const int tcH = int(a.getHeight() * 0.60f);
        outputTimecode.setBounds(a.removeFromTop(tcH));
    }
}

void SignalCard::setLtcActive(bool /*active*/)
{
    // §11 hook — styling phase: dim INPUT/OUTPUT timecodes when fallback active
}

// ============================================================
// DelaySyncCard
// ============================================================

DelaySyncCard::DelaySyncCard()
{
    titleLabel.setText("DELAY & SYNC", juce::dontSendNotification);

    delayMsLabel.setText("IN DELAY (MS)",     juce::dontSendNotification);
    delayFramesLabel.setText("IN DELAY (FRAMES)", juce::dontSendNotification);
    midiMsLabel.setText("OUT MIDI (MS)",      juce::dontSendNotification);

    delayMsValue.setText("--",   juce::dontSendNotification);
    delayFramesValue.setText("--", juce::dontSendNotification);
    midiMsValue.setText("--",    juce::dontSendNotification);

    toggleLabel.setText("DELAY TOGGLE", juce::dontSendNotification);
    delayToggle.setButtonText("OFF");

    sliderLabel.setText("MANUAL CORRECTION", juce::dontSendNotification);
    sliderValue.setText("0 ms", juce::dontSendNotification);

    manualSlider.setRange(-1500.0, 1500.0);
    manualSlider.setNumDecimalPlacesToDisplay(0);
    manualSlider.setTextValueSuffix(" ms");

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(delayMsLabel);
    addAndMakeVisible(delayMsValue);
    addAndMakeVisible(delayFramesLabel);
    addAndMakeVisible(delayFramesValue);
    addAndMakeVisible(midiMsLabel);
    addAndMakeVisible(midiMsValue);
    addAndMakeVisible(toggleLabel);
    addAndMakeVisible(delayToggle);
    addAndMakeVisible(sliderLabel);
    addAndMakeVisible(sliderValue);
    addAndMakeVisible(manualSlider);
}

void DelaySyncCard::resized()
{
    // §5: inner padding 24 px
    auto inner = getLocalBounds().reduced(24);

    // §5.1 Title
    titleLabel.setBounds(inner.removeFromTop(28));
    inner.removeFromTop(GAP_LARGE);

    // §5.2 Three metric rows: label left / value right, 32 px tall, 12 px gap
    constexpr int ROW_H  = 32;
    constexpr int VALUE_W = 100;

    auto layoutMetricRow = [&](juce::Label& lbl, juce::Label& val)
    {
        auto row = inner.removeFromTop(ROW_H);
        val.setBounds(row.removeFromRight(VALUE_W));
        lbl.setBounds(row);
        inner.removeFromTop(GAP_MED);
    };

    layoutMetricRow(delayMsLabel,     delayMsValue);
    layoutMetricRow(delayFramesLabel, delayFramesValue);
    layoutMetricRow(midiMsLabel,      midiMsValue);

    inner.removeFromTop(GAP_LARGE);

    // §5.3 Toggle row: label left / button right, 28 px tall
    {
        auto row = inner.removeFromTop(28);
        delayToggle.setBounds(row.removeFromRight(80));
        toggleLabel.setBounds(row);
    }
    inner.removeFromTop(GAP_LARGE);

    // §5.4 Slider row: label+value line, then full-width slider 24 px
    {
        auto labelRow = inner.removeFromTop(20);
        sliderValue.setBounds(labelRow.removeFromRight(80));
        sliderLabel.setBounds(labelRow);
        inner.removeFromTop(GAP_SMALL);
        manualSlider.setBounds(inner.removeFromTop(24));
    }
}

void DelaySyncCard::setMasterMode(bool /*isMaster*/)
{
    // §11 hook — styling phase: disable/dim entire card when Master mode
}

// ============================================================
// DiagnosticsCard
// ============================================================

DiagnosticsCard::DiagnosticsCard()
{
    summaryQ.setText("Q: --",           juce::dontSendNotification);
    summaryFps.setText("FPS: --",       juce::dontSendNotification);
    summaryDrift.setText("DRIFT: --",   juce::dontSendNotification);
    summaryFallback.setText("FALLBACK: --", juce::dontSendNotification);

    ch1Title.setText("CHANNEL 1",   juce::dontSendNotification);
    ch2Title.setText("CHANNEL 2",   juce::dontSendNotification);
    syncTitle.setText("SYNC ENGINE", juce::dontSendNotification);
    audioTitle.setText("AUDIO CLOCK", juce::dontSendNotification);

    footerLabel.setText("VIEW RAW DIAGNOSTICS LOG", juce::dontSendNotification);

    addAndMakeVisible(summaryQ);
    addAndMakeVisible(summaryFps);
    addAndMakeVisible(summaryDrift);
    addAndMakeVisible(summaryFallback);
    addAndMakeVisible(ch1Title);
    addAndMakeVisible(ch2Title);
    addAndMakeVisible(syncTitle);
    addAndMakeVisible(audioTitle);
    addAndMakeVisible(ch1Content);
    addAndMakeVisible(ch2Content);
    addAndMakeVisible(syncContent);
    addAndMakeVisible(audioContent);
    addAndMakeVisible(footerLabel);
}

void DiagnosticsCard::resized()
{
    // §6: inner padding 16 px
    auto inner = getLocalBounds().reduced(16);

    // §6.1 Summary row: 28 px, 4 equal columns
    {
        auto row = inner.removeFromTop(28);
        inner.removeFromTop(GAP_MED);
        const int colW = row.getWidth() / 4;
        summaryQ.setBounds(row.removeFromLeft(colW));
        summaryFps.setBounds(row.removeFromLeft(colW));
        summaryDrift.setBounds(row.removeFromLeft(colW));
        summaryFallback.setBounds(row);
    }

    // §6.3 Footer: 24 px from bottom
    footerLabel.setBounds(inner.removeFromBottom(24));
    inner.removeFromBottom(GAP_MED);

    // §6.2 Main diagnostics row: 4 equal columns
    const int colW = inner.getWidth() / 4;

    auto layoutCol = [&](juce::Label& title, juce::Label& content)
    {
        auto col = inner.removeFromLeft(colW);
        title.setBounds(col.removeFromTop(20));
        col.removeFromTop(GAP_SMALL);
        content.setBounds(col);
    };

    layoutCol(ch1Title,   ch1Content);
    layoutCol(ch2Title,   ch2Content);
    layoutCol(syncTitle,  syncContent);
    layoutCol(audioTitle, audioContent);
}

void DiagnosticsCard::setSummary(float q, float fps, double drift, bool fallback)
{
    summaryQ.setText("Q: " + juce::String(q, 2), juce::dontSendNotification);
    summaryFps.setText("FPS: " + juce::String(fps, 1), juce::dontSendNotification);
    summaryDrift.setText("DRIFT: " + juce::String(drift, 2) + " ms/s", juce::dontSendNotification);
    summaryFallback.setText(juce::String("FALLBACK: ") + (fallback ? "YES" : "NO"), juce::dontSendNotification);
}

// ============================================================
// ConfigSection
// ============================================================

ConfigSection::ConfigSection()
{
    titleLabel.setText("DEVICE CONFIGURATION", juce::dontSendNotification);
    modeLabel.setText("Mode",        juce::dontSendNotification);
    groupLabel.setText("Group",      juce::dontSendNotification);
    slotLabel.setText("Slot",        juce::dontSendNotification);
    ltcChLabel.setText("LTC Channel", juce::dontSendNotification);
    labelLabel.setText("Label",      juce::dontSendNotification);

    modeBox.addItem("Master", 1);
    modeBox.addItem("Slave",  2);

    for (int i = 1; i <= 8; ++i)
        slotBox.addItem(juce::String(i), i);

    // LTC channel — radio group so JUCE handles mutual exclusion
    ltcChL.setButtonText("L");
    ltcChL.setClickingTogglesState(true);
    ltcChL.setRadioGroupId(1);

    ltcChR.setButtonText("R");
    ltcChR.setClickingTogglesState(true);
    ltcChR.setRadioGroupId(1);

    groupEditor.setInputRestrictions(16,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
    labelEditor.setInputRestrictions(31);

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(modeLabel);    addAndMakeVisible(modeBox);
    addAndMakeVisible(groupLabel);   addAndMakeVisible(groupEditor);
    addAndMakeVisible(slotLabel);    addAndMakeVisible(slotBox);
    addAndMakeVisible(ltcChLabel);   addAndMakeVisible(ltcChL); addAndMakeVisible(ltcChR);
    addAndMakeVisible(labelLabel);   addAndMakeVisible(labelEditor);
}

void ConfigSection::resized()
{
    auto area = getLocalBounds();

    // Title row
    titleLabel.setBounds(area.removeFromTop(28));
    area.removeFromTop(GAP_MED);

    // §7.1 Row 1: Mode (300 px fixed) | gap | Group (flex)
    {
        auto row = area.removeFromTop(18 + GAP_SMALL + 24);   // label + gap + control
        auto modeArea  = row.removeFromLeft(300);
        row.removeFromLeft(GAP_H);
        auto groupArea = row;

        modeLabel.setBounds(modeArea.removeFromTop(18));
        modeArea.removeFromTop(GAP_SMALL);
        modeBox.setBounds(modeArea.removeFromTop(24));

        groupLabel.setBounds(groupArea.removeFromTop(18));
        groupArea.removeFromTop(GAP_SMALL);
        groupEditor.setBounds(groupArea.removeFromTop(24));
    }
    area.removeFromTop(GAP_MED);

    // §7.2 Row 2: Slot(20%) | gap | LTC ch(25%) | gap | Label(55%)
    {
        auto row = area.removeFromTop(18 + GAP_SMALL + 24);
        const int usableW = row.getWidth() - 2 * GAP_H;
        const int slotW   = int(usableW * 0.20f);
        const int ltcW    = int(usableW * 0.25f);

        auto slotArea  = row.removeFromLeft(slotW);
        row.removeFromLeft(GAP_H);
        auto ltcArea   = row.removeFromLeft(ltcW);
        row.removeFromLeft(GAP_H);
        auto labelArea = row;

        // Slot
        slotLabel.setBounds(slotArea.removeFromTop(18));
        slotArea.removeFromTop(GAP_SMALL);
        slotBox.setBounds(slotArea.removeFromTop(24));

        // LTC channel: two equal-width toggle buttons side by side
        ltcChLabel.setBounds(ltcArea.removeFromTop(18));
        ltcArea.removeFromTop(GAP_SMALL);
        {
            auto ctrl = ltcArea.removeFromTop(24);
            const int half = (ctrl.getWidth() - GAP_SMALL) / 2;
            ltcChL.setBounds(ctrl.removeFromLeft(half));
            ctrl.removeFromLeft(GAP_SMALL);
            ltcChR.setBounds(ctrl);
        }

        // Label
        labelLabel.setBounds(labelArea.removeFromTop(18));
        labelArea.removeFromTop(GAP_SMALL);
        labelEditor.setBounds(labelArea.removeFromTop(24));
    }
}

// ============================================================
// NewProjectAudioProcessorEditor
// ============================================================

NewProjectAudioProcessorEditor::NewProjectAudioProcessorEditor(NewProjectAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    addAndMakeVisible(header);
    addAndMakeVisible(signalCard);
    addAndMakeVisible(delayCard);
    addAndMakeVisible(diagCard);
    addAndMakeVisible(configSection);

    // --- FPS selector ---
    signalCard.fpsBox.setSelectedId(audioProcessor.fps == 25 ? 2 : 1, juce::dontSendNotification);
    signalCard.fpsBox.onChange = [&]()
    {
        audioProcessor.fps = (signalCard.fpsBox.getSelectedId() == 2) ? 25 : 30;
    };

    // --- Delay toggle ---
    delayCard.delayToggle.onClick = [&]()
    {
        audioProcessor.active_delay = !audioProcessor.active_delay;
    };

    // --- Manual correction slider ---
    delayCard.manualSlider.setValue(audioProcessor.by_slider, juce::dontSendNotification);
    delayCard.manualSlider.onValueChange = [&]()
    {
        const double v = delayCard.manualSlider.getValue();
        audioProcessor.by_slider = v;
        delayCard.setManualValue(juce::String((int)v) + " ms");
    };

    // --- Config: Mode ---
    configSection.modeBox.setSelectedId((int)audioProcessor.pluginMode + 1, juce::dontSendNotification);
    configSection.modeBox.onChange = [&]()
    {
        audioProcessor.pluginMode = (PluginMode)(configSection.modeBox.getSelectedId() - 1);
    };

    // --- Config: Group ---
    configSection.groupEditor.setText(audioProcessor.groupName, false);
    auto applyGroup = [&]()
    {
        juce::String g = configSection.groupEditor.getText().trim();
        if (g.isEmpty()) { configSection.groupEditor.setText(audioProcessor.groupName, false); return; }
        if (g != audioProcessor.groupName) { audioProcessor.groupName = g; audioProcessor.reopenShm(); }
    };
    configSection.groupEditor.onReturnKey  = applyGroup;
    configSection.groupEditor.onFocusLost  = applyGroup;

    // --- Config: Slot ---
    configSection.slotBox.setSelectedId(audioProcessor.slotId, juce::dontSendNotification);
    configSection.slotBox.onChange = [&]()
    {
        audioProcessor.slotId = configSection.slotBox.getSelectedId();
    };

    // --- Config: LTC channel ---
    configSection.ltcChL.setToggleState(audioProcessor.ltcChannel == 0, juce::dontSendNotification);
    configSection.ltcChR.setToggleState(audioProcessor.ltcChannel == 1, juce::dontSendNotification);
    configSection.ltcChL.onClick = [&]() { audioProcessor.ltcChannel = 0; };
    configSection.ltcChR.onClick = [&]() { audioProcessor.ltcChannel = 1; };

    // --- Config: Label ---
    configSection.labelEditor.setText(audioProcessor.slotLabel, false);
    configSection.labelEditor.onTextChange = [&]()
    {
        audioProcessor.slotLabel = configSection.labelEditor.getText();
    };

    setSize(1500, 900);
    startTimer(1);
}

NewProjectAudioProcessorEditor::~NewProjectAudioProcessorEditor() {}

// ============================================================
// paint  — background fill; card backgrounds added in styling phase
// ============================================================

void NewProjectAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

// ============================================================
// resized  — top-level Rectangle slicing (§1)
// ============================================================

void NewProjectAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(PAD);
    const int totalH = getHeight();

    // §1  HeaderBar  44 px
    header.setBounds(area.removeFromTop(44));
    area.removeFromTop(GAP_SECTION);

    // §1  MainRow  ~42% of total height
    const int mainH = int(totalH * 0.42f);
    auto mainRow = area.removeFromTop(mainH);
    area.removeFromTop(GAP_SECTION);

    // §1  DiagnosticsCard  ~18% of total height
    const int diagH = int(totalH * 0.18f);
    diagCard.setBounds(area.removeFromTop(diagH));
    area.removeFromTop(GAP_SECTION);

    // §1  ConfigSection  fills remainder
    configSection.setBounds(area);

    // §3  MainRow split  68% SignalCard | gap | 32% DelaySyncCard
    const int leftW = int(mainRow.getWidth() * 0.68f);
    signalCard.setBounds(mainRow.removeFromLeft(leftW));
    mainRow.removeFromLeft(GAP_H);
    delayCard.setBounds(mainRow);
}

// ============================================================
// timerCallback  — push processor state to all sub-components
// ============================================================

void NewProjectAudioProcessorEditor::timerCallback()
{
    // --- HeaderBar ---
    header.setMode(audioProcessor.pluginMode == PluginMode::Master ? "MASTER" : "SLAVE");
    header.setGroup(audioProcessor.groupName);
    header.setFps(juce::String(audioProcessor.fps) + " FPS");

    {
        const char* srcNames[] = { "NONE", "LTC", "AUD" };
        int src = std::clamp(audioProcessor.aud_fusionSource, 0, 2);
        header.setSource(srcNames[src]);
    }

    if (audioProcessor.pluginMode == PluginMode::Master)
    {
        header.setSyncStatus("SM:" + juce::String(audioProcessor.shmWriteCount));
        header.setSlot("MASTER");
    }
    else
    {
        header.setSlot("SLOT " + juce::String(audioProcessor.slotId));
        if (audioProcessor.holding)
            header.setSyncStatus("HOLD");
        else if (audioProcessor.masterValid)
            header.setSyncStatus("SYNC");
        else
            header.setSyncStatus("WAIT");
    }

    // --- SignalCard ---
    signalCard.setInputTimecode(audioProcessor.input_ch1);
    signalCard.setInputMeta(
        "Q=" + juce::String(audioProcessor.ch1_Q_LTC, 2) +
        "  fps=" + juce::String(audioProcessor.ch1_estimated_fps, 1));
    signalCard.setOutputTimecode(audioProcessor.tc);
    signalCard.setLtcActive(audioProcessor.aud_fusionSource != 2);

    // --- DelaySyncCard ---
    delayCard.setDelayMs(audioProcessor.o_delay_ms);
    delayCard.setDelayFrames(audioProcessor.delay_ms);
    delayCard.setMidiMs(
        juce::String((int)std::round(std::abs(audioProcessor.d_ms) + audioProcessor.by_slider)) + " ms");
    delayCard.setToggleActive(audioProcessor.active_delay);
    delayCard.setMasterMode(audioProcessor.pluginMode == PluginMode::Master);

    // --- DiagnosticsCard ---
    const char* stateNames[] = { "FAIL", "SUSPECT", "VALID" };
    const int s1 = std::clamp(audioProcessor.ch1_ltc_state, 0, 2);
    const int s2 = std::clamp(audioProcessor.ch2_ltc_state, 0, 2);

    diagCard.setSummary(
        audioProcessor.ch1_Q_LTC,
        audioProcessor.ch1_estimated_fps,
        audioProcessor.drift_per_s,
        audioProcessor.fallback_requested);

    diagCard.setCh1(
        juce::String(stateNames[s1]) +
        "  Q=" + juce::String(audioProcessor.ch1_Q_LTC, 2) +
        "  fps=" + juce::String(audioProcessor.ch1_estimated_fps, 1) +
        "  Rst=" + juce::String(audioProcessor.ch1_decoder_resets) +
        "  Rej=" + juce::String(audioProcessor.ch1_rejected_frames));

    diagCard.setCh2(
        juce::String(stateNames[s2]) +
        "  Q=" + juce::String(audioProcessor.ch2_Q_LTC, 2) +
        "  fps=" + juce::String(audioProcessor.ch2_estimated_fps, 1) +
        "  Rst=" + juce::String(audioProcessor.ch2_decoder_resets) +
        "  Rej=" + juce::String(audioProcessor.ch2_rejected_frames));

    diagCard.setSyncEngine(
        "dt_dev=" + juce::String(audioProcessor.dt_deviation, 1) + "ms" +
        "  drift=" + juce::String(audioProcessor.drift_per_s, 2) + "ms/s" +
        "  Fallback=" + (audioProcessor.fallback_requested ? "YES" : "no"));

    {
        const char* srcNm[] = { "NONE", "LTC", "AUD" };
        const int src = std::clamp(audioProcessor.aud_fusionSource, 0, 2);
        const juce::String dtStr = audioProcessor.aud_conf > 0.01
            ? juce::String(audioProcessor.aud_deltaMs, 0) + "ms"
            : "---";
        const juce::String activeStr = audioProcessor.aud_activeDelayMs != 0.0
            ? juce::String((int)std::round(audioProcessor.aud_activeDelayMs)) + "ms"
            : "off";
        diagCard.setAudioClock(
            "dt=" + dtStr +
            "  conf=" + juce::String(audioProcessor.aud_conf, 2) +
            "  src=" + srcNm[src] +
            "  applied=" + activeStr);
    }

    repaint();
}
