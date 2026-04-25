/*
  ==============================================================================
    PluginEditor.cpp  —  UI redesign, styling phase
  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

// ============================================================
// Internal helpers
// ============================================================

namespace {

// Apply the shared card paint pattern (background + 1px border).
static void paintCard(juce::Graphics& g, juce::Rectangle<int> bounds)
{
    const auto r = bounds.toFloat();
    g.setColour(Theme::cardBg);
    g.fillRoundedRectangle(r, Theme::cardRadius);
    g.setColour(Theme::cardBorder);
    g.drawRoundedRectangle(r.reduced(0.5f), Theme::cardRadius, 1.0f);
}

// Style a label as a section/card title.
static void styleTitle(juce::Label& l)
{
    l.setFont(juce::Font(Theme::fontTitle, juce::Font::bold));
    l.setColour(juce::Label::textColourId, Theme::textTitle);
    l.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
}

// Style a label as a field label (small, dim).
static void styleFieldLabel(juce::Label& l)
{
    l.setFont(juce::Font(Theme::fontLabel));
    l.setColour(juce::Label::textColourId, Theme::textLabel);
    l.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
}

// Style a label as metadata/diagnostic content.
static void styleMeta(juce::Label& l)
{
    l.setFont(juce::Font(Theme::fontMeta));
    l.setColour(juce::Label::textColourId, Theme::textMeta);
    l.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
}

// Style a label as a right-aligned metric value (transparent bg — box drawn in paint).
static void styleValue(juce::Label& l)
{
    l.setFont(juce::Font(Theme::fontValue, juce::Font::bold));
    l.setColour(juce::Label::textColourId, Theme::textPrimary);
    l.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l.setJustificationType(juce::Justification::centred);
}

} // namespace

// ============================================================
// BadgeComponent
// ============================================================

BadgeComponent::BadgeComponent(const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setFont(juce::Font(Theme::fontBadge, juce::Font::bold));
    label.setColour(juce::Label::textColourId, Theme::textPrimary);
    label.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(label);
}

void BadgeComponent::setText(const juce::String& t)
{
    label.setText(t, juce::dontSendNotification);
}

void BadgeComponent::setBadgeColour(juce::Colour c)
{
    badgeColour = c;
    repaint();
}

void BadgeComponent::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(badgeColour);
    g.fillRoundedRectangle(r, r.getHeight() * 0.5f);   // true pill radius
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
    titleLabel.setFont(juce::Font(Theme::fontTitle + 2.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId, Theme::textPrimary);
    titleLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    groupLabel.setFont(juce::Font(Theme::fontLabel));
    groupLabel.setColour(juce::Label::textColourId, Theme::textLabel);
    groupLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(modeBadge);
    addAndMakeVisible(groupLabel);
    addAndMakeVisible(syncBadge);
    addAndMakeVisible(slotBadge);
    addAndMakeVisible(sourceBadge);
    addAndMakeVisible(fpsBadge);
}

void HeaderBar::setSyncBadge(const juce::String& text, juce::Colour colour)
{
    syncBadge.setText(text);
    syncBadge.setBadgeColour(colour);
}

void HeaderBar::resized()
{
    auto area = getLocalBounds();
    const int w = area.getWidth();

    auto leftZone   = area.removeFromLeft(int(w * 0.50f));
    auto centerZone = area.removeFromLeft(int(w * 0.20f));
    auto rightZone  = area;

    // Left: icon gap | title | modeBadge | groupLabel
    leftZone.removeFromLeft(10);
    leftZone.removeFromLeft(24);   // icon reserved
    leftZone.removeFromLeft(10);
    titleLabel.setBounds(leftZone.removeFromLeft(200));
    leftZone.removeFromLeft(10);
    modeBadge.setBounds(leftZone.removeFromLeft(80));
    leftZone.removeFromLeft(10);
    groupLabel.setBounds(leftZone);

    // Center: sync badge
    syncBadge.setBounds(centerZone.reduced(4, 4));

    // Right: right-aligned badges
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
    // Section labels
    inputLabel.setText("INPUT SIGNAL", juce::dontSendNotification);
    outputLabel.setText("OUTPUT SYNC",  juce::dontSendNotification);
    styleFieldLabel(inputLabel);
    styleFieldLabel(outputLabel);

    // Timecode displays — dominant element
    inputTimecode.setText("--:--:--:--",  juce::dontSendNotification);
    outputTimecode.setText("--:--:--:--", juce::dontSendNotification);
    {
        const juce::Font tcFont(juce::Font::getDefaultMonospacedFontName(),
                                Theme::fontTimecode, juce::Font::bold);
        inputTimecode.setFont(tcFont);
        outputTimecode.setFont(tcFont);
    }
    inputTimecode.setColour(juce::Label::textColourId, Theme::textPrimary);
    inputTimecode.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    inputTimecode.setJustificationType(juce::Justification::centred);
    outputTimecode.setColour(juce::Label::textColourId, Theme::textPrimary);
    outputTimecode.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    outputTimecode.setJustificationType(juce::Justification::centred);

    // Metadata sub-label
    inputMeta.setText("", juce::dontSendNotification);
    styleMeta(inputMeta);
    inputMeta.setJustificationType(juce::Justification::centred);

    // FPS row
    fpsRowLabel.setText("FRAME RATE SELECTION", juce::dontSendNotification);
    styleFieldLabel(fpsRowLabel);

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
    // Card background
    paintCard(g, getLocalBounds());

    g.setColour(Theme::divider);

    // Vertical divider between INPUT and OUTPUT (§4.1)
    if (!vertDivider.isEmpty())
        g.fillRect(vertDivider);

    // Horizontal divider between timecode row and FPS row (§4.2)
    if (!horizDivider.isEmpty())
        g.fillRect(horizDivider);
}

void SignalCard::resized()
{
    auto inner = getLocalBounds().reduced(14);

    const int tcHeight = int(inner.getHeight() * 0.50f);
    auto tcArea = inner.removeFromTop(tcHeight);

    inner.removeFromTop(GAP_LARGE);
    horizDivider = inner.removeFromTop(1);
    inner.removeFromTop(GAP_LARGE);

    // FPS row
    {
        auto fpsArea = inner;
        fpsRowLabel.setBounds(fpsArea.removeFromTop(20));
        fpsArea.removeFromTop(GAP_SMALL);
        fpsBox.setBounds(fpsArea.removeFromLeft(160).removeFromTop(26));
    }

    // TimecodeRow split 50/50
    auto inputArea  = tcArea.removeFromLeft(tcArea.getWidth() / 2);
    auto outputArea = tcArea;

    vertDivider = juce::Rectangle<int>(inputArea.getRight(), inputArea.getY(),
                                       1, inputArea.getHeight());
    inputArea.removeFromRight(1);
    outputArea.removeFromLeft(1);

    // INPUT side
    {
        auto a = inputArea;
        inputLabel.setBounds(a.removeFromTop(20));
        a.removeFromTop(GAP_SMALL);
        inputTimecode.setBounds(a.removeFromTop(int(a.getHeight() * 0.60f)));
        a.removeFromTop(4);
        inputMeta.setBounds(a.removeFromTop(20));
    }

    // OUTPUT side
    {
        auto a = outputArea;
        outputLabel.setBounds(a.removeFromTop(20));
        a.removeFromTop(GAP_SMALL);
        outputTimecode.setBounds(a.removeFromTop(int(a.getHeight() * 0.60f)));
    }
}

void SignalCard::setLtcActive(bool active)
{
    const juce::Colour tcColour = active ? Theme::textPrimary : Theme::textMeta;
    inputTimecode.setColour(juce::Label::textColourId, tcColour);
    outputTimecode.setColour(juce::Label::textColourId, tcColour);
    inputMeta.setColour(juce::Label::textColourId, active ? Theme::textMeta : Theme::textMeta.darker(0.4f));
}

// ============================================================
// DelaySyncCard
// ============================================================

DelaySyncCard::DelaySyncCard()
{
    // Card title
    titleLabel.setText("DELAY & SYNC", juce::dontSendNotification);
    styleTitle(titleLabel);

    // Field labels
    delayMsLabel.setText("IN DELAY (MS)",      juce::dontSendNotification);
    delayFramesLabel.setText("IN DELAY (FRAMES)", juce::dontSendNotification);
    midiMsLabel.setText("OUT MIDI (MS)",       juce::dontSendNotification);
    toggleLabel.setText("DELAY TOGGLE",        juce::dontSendNotification);
    sliderLabel.setText("MANUAL CORRECTION",   juce::dontSendNotification);
    styleFieldLabel(delayMsLabel);
    styleFieldLabel(delayFramesLabel);
    styleFieldLabel(midiMsLabel);
    styleFieldLabel(toggleLabel);
    styleFieldLabel(sliderLabel);

    // Value labels — right-aligned, transparent bg (box drawn in paint)
    delayMsValue.setText("--",   juce::dontSendNotification);
    delayFramesValue.setText("--", juce::dontSendNotification);
    midiMsValue.setText("--",    juce::dontSendNotification);
    styleValue(delayMsValue);
    styleValue(delayFramesValue);
    styleValue(midiMsValue);

    // Slider value echo
    sliderValue.setText("0 ms", juce::dontSendNotification);
    styleMeta(sliderValue);
    sliderValue.setJustificationType(juce::Justification::centredRight);

    delayToggle.setButtonText("OFF");

    manualSlider.setRange(-250.0, 250.0);
    manualSlider.setNumDecimalPlacesToDisplay(0);
    manualSlider.setTextValueSuffix(" ms");

    resetButton.setButtonText("0");
    resetButton.setTooltip("Reset manual correction to zero");

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
    addAndMakeVisible(resetButton);
}

void DelaySyncCard::paint(juce::Graphics& g)
{
    paintCard(g, getLocalBounds());

    // Value-box sunken backgrounds (rounded insets behind value labels)
    g.setColour(Theme::valueBg);
    for (const auto* lbl : { &delayMsValue, &delayFramesValue, &midiMsValue })
        g.fillRoundedRectangle(lbl->getBounds().toFloat(), 4.0f);

    g.setColour(Theme::valueBorder);
    for (const auto* lbl : { &delayMsValue, &delayFramesValue, &midiMsValue })
        g.drawRoundedRectangle(lbl->getBounds().toFloat().reduced(0.5f), 4.0f, 1.0f);
}

void DelaySyncCard::resized()
{
    auto inner = getLocalBounds().reduced(24);

    titleLabel.setBounds(inner.removeFromTop(24));
    inner.removeFromTop(GAP_LARGE);

    constexpr int ROW_H  = 24;
    constexpr int VALUE_W = 100;

    auto layoutMetricRow = [&](juce::Label& lbl, juce::Label& val)
    {
        auto row = inner.removeFromTop(ROW_H);
        val.setBounds(row.removeFromRight(VALUE_W));
        lbl.setBounds(row);
        inner.removeFromTop(GAP_SMALL);
    };

    layoutMetricRow(delayMsLabel,     delayMsValue);
    layoutMetricRow(delayFramesLabel, delayFramesValue);
    layoutMetricRow(midiMsLabel,      midiMsValue);

    inner.removeFromTop(GAP_LARGE);

    // Toggle row
    {
        auto row = inner.removeFromTop(22);
        delayToggle.setBounds(row.removeFromRight(70));
        toggleLabel.setBounds(row);
    }
    inner.removeFromTop(GAP_LARGE);

    // Slider row
    {
        auto labelRow = inner.removeFromTop(16);
        sliderValue.setBounds(labelRow.removeFromRight(70));
        sliderLabel.setBounds(labelRow);
        inner.removeFromTop(GAP_SMALL);
        auto sliderRow = inner.removeFromTop(20);
        resetButton.setBounds(sliderRow.removeFromRight(24));
        sliderRow.removeFromRight(4);
        manualSlider.setBounds(sliderRow);
    }
}

void DelaySyncCard::setMasterMode(bool isMaster)
{
    setAlpha(isMaster ? 0.35f : 1.0f);
}

// ============================================================
// DiagnosticsCard
// ============================================================

DiagnosticsCard::DiagnosticsCard()
{
    // Summary row
    for (auto* l : { &summaryQ, &summaryFps, &summaryDrift, &summaryFallback })
    {
        l->setFont(juce::Font(Theme::fontLabel, juce::Font::bold));
        l->setColour(juce::Label::textColourId, Theme::textTitle);
        l->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    }
    summaryQ.setText("Q: --",              juce::dontSendNotification);
    summaryFps.setText("FPS: --",          juce::dontSendNotification);
    summaryDrift.setText("DRIFT: --",      juce::dontSendNotification);
    summaryFallback.setText("FALLBACK: --", juce::dontSendNotification);

    // Column titles
    for (auto* l : { &ch1Title, &ch2Title, &syncTitle, &audioTitle })
        styleTitle(*l);
    ch1Title.setText("CHANNEL 1",    juce::dontSendNotification);
    ch2Title.setText("CHANNEL 2",    juce::dontSendNotification);
    syncTitle.setText("SYNC ENGINE", juce::dontSendNotification);
    audioTitle.setText("AUDIO CLOCK", juce::dontSendNotification);

    // Column content
    for (auto* l : { &ch1Content, &ch2Content, &syncContent, &audioContent })
        styleMeta(*l);

    // Footer
    footerLabel.setText("VIEW RAW DIAGNOSTICS LOG", juce::dontSendNotification);
    footerLabel.setFont(juce::Font(Theme::fontMeta));
    footerLabel.setColour(juce::Label::textColourId, Theme::textMeta);
    footerLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

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

void DiagnosticsCard::paint(juce::Graphics& g)
{
    paintCard(g, getLocalBounds());

    g.setColour(Theme::divider);

    // Horizontal separator below summary row
    if (!summaryDivider.isEmpty())
        g.fillRect(summaryDivider);

    // Vertical column separators
    for (int x : colSepX)
        if (x > 0)
            g.fillRect(juce::Rectangle<int>(x, colSepY1, 1, colSepY2 - colSepY1));
}

void DiagnosticsCard::resized()
{
    auto inner = getLocalBounds().reduced(10);

    // Summary row
    {
        auto row = inner.removeFromTop(22);
        inner.removeFromTop(GAP_SMALL);
        summaryDivider = inner.removeFromTop(1);
        inner.removeFromTop(GAP_MED);

        const int colW = row.getWidth() / 4;
        summaryQ.setBounds(row.removeFromLeft(colW));
        summaryFps.setBounds(row.removeFromLeft(colW));
        summaryDrift.setBounds(row.removeFromLeft(colW));
        summaryFallback.setBounds(row);
    }

    // Footer
    footerLabel.setBounds(inner.removeFromBottom(18));
    inner.removeFromBottom(GAP_MED);

    // Store column separator geometry (3 lines between 4 equal columns)
    const int colW = inner.getWidth() / 4;
    colSepY1 = inner.getY();
    colSepY2 = inner.getBottom();
    for (int i = 0; i < 3; ++i)
        colSepX[i] = inner.getX() + (i + 1) * colW;

    // Lay out columns
    auto layoutCol = [&](juce::Label& title, juce::Label& content)
    {
        auto col = inner.removeFromLeft(colW).reduced(GAP_SMALL, 0);
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
    summaryFallback.setText(juce::String("FALLBACK: ") + (fallback ? "YES" : "NO"),
                            juce::dontSendNotification);
}

// ============================================================
// ConfigSection
// ============================================================

ConfigSection::ConfigSection()
{
    titleLabel.setText("DEVICE CONFIGURATION", juce::dontSendNotification);
    styleTitle(titleLabel);

    styleFieldLabel(modeLabel);
    styleFieldLabel(groupLabel);
    styleFieldLabel(slotLabel);
    styleFieldLabel(ltcChLabel);
    styleFieldLabel(labelLabel);
    modeLabel.setText("Mode",         juce::dontSendNotification);
    groupLabel.setText("Group",       juce::dontSendNotification);
    slotLabel.setText("Slot",         juce::dontSendNotification);
    ltcChLabel.setText("LTC Channel", juce::dontSendNotification);
    labelLabel.setText("Label",       juce::dontSendNotification);

    modeBox.addItem("Master", 1);
    modeBox.addItem("Slave",  2);

    for (int i = 1; i <= 8; ++i)
        slotBox.addItem(juce::String(i), i);

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

    titleLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(GAP_MED);

    // Row 1: Mode (300 px fixed) | gap | Group (flex)
    {
        auto row = area.removeFromTop(18 + GAP_SMALL + 24);
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

    // Row 2: Slot(20%) | gap | LTC ch(25%) | gap | Label(55%)
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

        slotLabel.setBounds(slotArea.removeFromTop(18));
        slotArea.removeFromTop(GAP_SMALL);
        slotBox.setBounds(slotArea.removeFromTop(24));

        ltcChLabel.setBounds(ltcArea.removeFromTop(18));
        ltcArea.removeFromTop(GAP_SMALL);
        {
            auto ctrl = ltcArea.removeFromTop(24);
            const int half = (ctrl.getWidth() - GAP_SMALL) / 2;
            ltcChL.setBounds(ctrl.removeFromLeft(half));
            ctrl.removeFromLeft(GAP_SMALL);
            ltcChR.setBounds(ctrl);
        }

        labelLabel.setBounds(labelArea.removeFromTop(18));
        labelArea.removeFromTop(GAP_SMALL);
        labelEditor.setBounds(labelArea.removeFromTop(24));
    }
}

// ============================================================
// AutoSyncAudioProcessorEditor
// ============================================================

AutoSyncAudioProcessorEditor::AutoSyncAudioProcessorEditor(AutoSyncAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    addAndMakeVisible(header);
    addAndMakeVisible(signalCard);
    addAndMakeVisible(delayCard);
    addAndMakeVisible(diagCard);
    addAndMakeVisible(configSection);

    // FPS selector
    signalCard.fpsBox.setSelectedId(audioProcessor.fps == 25 ? 2 : 1, juce::dontSendNotification);
    signalCard.fpsBox.onChange = [&]()
    {
        audioProcessor.fps = (signalCard.fpsBox.getSelectedId() == 2) ? 25 : 30;
    };

    // Delay toggle
    delayCard.delayToggle.onClick = [&]()
    {
        audioProcessor.active_delay = !audioProcessor.active_delay;
    };

    // Manual correction slider
    delayCard.manualSlider.setValue(audioProcessor.by_slider, juce::dontSendNotification);
    delayCard.manualSlider.onValueChange = [&]()
    {
        const double v = delayCard.manualSlider.getValue();
        audioProcessor.by_slider = v;
        delayCard.setManualValue(juce::String((int)v) + " ms");
    };
    delayCard.resetButton.onClick = [&]()
    {
        delayCard.manualSlider.setValue(0.0, juce::sendNotification);
    };

    // Config: Mode
    configSection.modeBox.setSelectedId((int)audioProcessor.pluginMode + 1, juce::dontSendNotification);
    configSection.modeBox.onChange = [&]()
    {
        audioProcessor.pluginMode = (PluginMode)(configSection.modeBox.getSelectedId() - 1);
    };

    // Config: Group
    configSection.groupEditor.setText(audioProcessor.groupName, false);
    auto applyGroup = [&]()
    {
        juce::String g = configSection.groupEditor.getText().trim();
        if (g.isEmpty()) { configSection.groupEditor.setText(audioProcessor.groupName, false); return; }
        if (g != audioProcessor.groupName) { audioProcessor.groupName = g; audioProcessor.reopenShm(); }
    };
    configSection.groupEditor.onReturnKey = applyGroup;
    configSection.groupEditor.onFocusLost = applyGroup;

    // Config: Slot
    configSection.slotBox.setSelectedId(audioProcessor.slotId, juce::dontSendNotification);
    configSection.slotBox.onChange = [&]()
    {
        audioProcessor.slotId = configSection.slotBox.getSelectedId();
    };

    // Config: LTC channel
    configSection.ltcChL.setToggleState(audioProcessor.ltcChannel == 0, juce::dontSendNotification);
    configSection.ltcChR.setToggleState(audioProcessor.ltcChannel == 1, juce::dontSendNotification);
    configSection.ltcChL.onClick = [&]() { audioProcessor.ltcChannel = 0; };
    configSection.ltcChR.onClick = [&]() { audioProcessor.ltcChannel = 1; };

    // Config: Label
    configSection.labelEditor.setText(audioProcessor.slotLabel, false);
    configSection.labelEditor.onTextChange = [&]()
    {
        audioProcessor.slotLabel = configSection.labelEditor.getText();
    };

    setSize(900, 620);
    startTimer(1);
}

AutoSyncAudioProcessorEditor::~AutoSyncAudioProcessorEditor() {}

// ============================================================
// paint  — window background
// ============================================================

void AutoSyncAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(Theme::bg);
}

// ============================================================
// resized  — top-level Rectangle slicing
// ============================================================

void AutoSyncAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(PAD);
    const int totalH = getHeight();

    header.setBounds(area.removeFromTop(34));
    area.removeFromTop(GAP_SECTION);

    const int mainH = int(totalH * 0.44f);
    auto mainRow = area.removeFromTop(mainH);
    area.removeFromTop(GAP_SECTION);

    const int diagH = int(totalH * 0.20f);
    diagCard.setBounds(area.removeFromTop(diagH));
    area.removeFromTop(GAP_SECTION);

    configSection.setBounds(area);

    const int leftW = int(mainRow.getWidth() * 0.68f);
    signalCard.setBounds(mainRow.removeFromLeft(leftW));
    mainRow.removeFromLeft(GAP_H);
    delayCard.setBounds(mainRow);
}

// ============================================================
// timerCallback  — push processor state to all sub-components
// ============================================================

void AutoSyncAudioProcessorEditor::timerCallback()
{
    // --- HeaderBar ---
    header.setMode(audioProcessor.pluginMode == PluginMode::Master ? "MASTER" : "SLAVE");
    header.setGroup(audioProcessor.groupName);
    header.setFps(juce::String(audioProcessor.fps) + " FPS");

    {
        const char* srcNames[] = { "NONE", "LTC", "AUD" };
        header.setSource(srcNames[std::clamp(audioProcessor.aud_fusionSource, 0, 2)]);
    }

    if (audioProcessor.pluginMode == PluginMode::Master)
    {
        header.setSyncBadge("SM:" + juce::String(audioProcessor.shmWriteCount), Theme::badgeBg);
        header.setSlot("MASTER");
    }
    else
    {
        header.setSlot("SLOT " + juce::String(audioProcessor.slotId));
        if (audioProcessor.holding)
            header.setSyncBadge("HOLD", Theme::accentOrange);
        else if (audioProcessor.masterValid)
            header.setSyncBadge("SYNC", Theme::accentGreen);
        else
            header.setSyncBadge("WAIT", Theme::cardBorder);
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
            ? juce::String(audioProcessor.aud_deltaMs, 0) + "ms" : "---";
        const juce::String activeStr = audioProcessor.aud_activeDelayMs != 0.0
            ? juce::String((int)std::round(audioProcessor.aud_activeDelayMs)) + "ms" : "off";
        diagCard.setAudioClock(
            "dt=" + dtStr +
            "  conf=" + juce::String(audioProcessor.aud_conf, 2) +
            "  src=" + srcNm[src] +
            "  applied=" + activeStr);
    }

    repaint();
}
