#include "MainComponent.h"

#include <cmath>

// ===== SECTION: MainComponent実装 =====
// docs/design.md 3章のレイアウト表・3.5節のコンポーネント仕様・6章の状態設計に対応する。
// UIスレッドと音声スレッド間はAtomicParams(std::atomic)のみでやり取りする（音声パスには触れない）。

namespace vc
{

// ===== SECTION: AppLookAndFeel =====

AppLookAndFeel::AppLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (bgWindow));

    setColour (juce::ComboBox::backgroundColourId, juce::Colour (surface));
    setColour (juce::ComboBox::textColourId, juce::Colour (textPrimary));
    setColour (juce::ComboBox::outlineColourId, juce::Colour (lineBorder));
    setColour (juce::ComboBox::arrowColourId, juce::Colour (textSecondary));
    setColour (juce::ComboBox::focusedOutlineColourId, juce::Colour (focus));
    setColour (juce::ComboBox::buttonColourId, juce::Colour (surface));

    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xFF222428));
    setColour (juce::PopupMenu::textColourId, juce::Colour (textPrimary));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (popupHighlight));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (textPrimary));

    setColour (juce::TextButton::buttonColourId, juce::Colour (surface));
    setColour (juce::TextButton::textColourOffId, juce::Colour (textPrimary));
    setColour (juce::TextButton::textColourOnId, juce::Colour (liveInk));

    setColour (juce::Slider::backgroundColourId, juce::Colour (track));
    setColour (juce::Slider::trackColourId, juce::Colour (live));
    setColour (juce::Slider::thumbColourId, juce::Colour (textPrimary));
    setColour (juce::Slider::textBoxTextColourId, juce::Colour (textPrimary));
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    setColour (juce::Label::textColourId, juce::Colour (textPrimary));

    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (tooltipBg));
    setColour (juce::TooltipWindow::textColourId, juce::Colour (textPrimary));
    setColour (juce::TooltipWindow::outlineColourId, juce::Colour (tooltipBorder));

    setColour (juce::AlertWindow::backgroundColourId, juce::Colour (bgWindow));
    setColour (juce::AlertWindow::textColourId, juce::Colour (textPrimary));
    setColour (juce::AlertWindow::outlineColourId, juce::Colour (lineBorder));
}

juce::Font AppLookAndFeel::uiFont (float pointHeight, bool bold)
{
    static const juce::String name = [] () -> juce::String
    {
        const auto available = juce::Font::findAllTypefaceNames();
        for (const char* candidate : { "Yu Gothic UI", "Meiryo UI", "Noto Sans CJK JP" })
            if (available.contains (candidate))
                return juce::String (candidate);
        return juce::Font::getDefaultSansSerifFontName();
    }();

    auto options = juce::FontOptions {}.withName (name).withPointHeight (pointHeight)
                       .withStyleFlags (bold ? juce::Font::bold : juce::Font::plain);
    return juce::Font (options);
}

juce::Font AppLookAndFeel::numericFont (float pointHeight)
{
    static const juce::String name = [] () -> juce::String
    {
        const auto available = juce::Font::findAllTypefaceNames();
        if (available.contains ("Consolas"))
            return "Consolas";
        return juce::Font::getDefaultMonospacedFontName();
    }();

    return juce::Font (juce::FontOptions {}.withName (name).withPointHeight (pointHeight));
}

void AppLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                                    int, int, int, int, juce::ComboBox& box)
{
    const juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);
    const bool errorState = (bool) box.getProperties().getWithDefault ("errorState", false);
    const bool hasFocus = box.hasKeyboardFocus (true);
    const bool hover = box.isMouseOver (true);

    g.setColour (juce::Colour (surface));
    g.fillRoundedRectangle (bounds, 4.0f);

    juce::Colour borderColour (lineBorder);
    float borderWidth = 1.0f;

    if (errorState)     { borderColour = juce::Colour (error);          borderWidth = 1.5f; }
    else if (hasFocus)  { borderColour = juce::Colour (focus);          borderWidth = 2.0f; }
    else if (hover)     { borderColour = juce::Colour (lineBorderHover); borderWidth = 1.0f; }

    g.setColour (borderColour);
    g.drawRoundedRectangle (bounds.reduced (borderWidth * 0.5f), 4.0f, borderWidth);

    // シェブロン（design.md 3.5節: 幅10×高さ6、線幅1.5、右端から12px）
    const float cx = (float) width - 12.0f - 5.0f;
    const float cy = (float) height * 0.5f;

    juce::Path chevron;
    chevron.startNewSubPath (cx - 5.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 3.0f);
    chevron.lineTo (cx + 5.0f, cy - 2.0f);

    g.setColour (juce::Colour (errorState ? error : (juce::uint32) textSecondary));
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font AppLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return uiFont (14.0f);
}

juce::Font AppLookAndFeel::getPopupMenuFont()
{
    return uiFont (14.0f);
}

void AppLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (juce::Colour (0xFF222428));
    g.setColour (juce::Colour (lineBorder));
    g.drawRect (0, 0, width, height, 1);
}

void AppLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                         bool /*isActive*/, bool isHighlighted, bool isTicked, bool /*hasSubMenu*/,
                                         const juce::String& text, const juce::String&, const juce::Drawable*,
                                         const juce::Colour*)
{
    if (isSeparator)
    {
        g.setColour (juce::Colour (lineDivider));
        g.fillRect (area.withY (area.getCentreY()).withHeight (1).reduced (8, 0));
        return;
    }

    if (isHighlighted)
    {
        g.setColour (juce::Colour (popupHighlight));
        g.fillRect (area);
    }

    auto r = area.reduced (12, 0);

    g.setColour (juce::Colour (textPrimary));
    g.setFont (uiFont (14.0f));

    if (isTicked)
        g.drawText (juce::String::fromUTF8 ("\xE2\x9C\x93"), r.removeFromLeft (18), juce::Justification::centredLeft, false);
    else
        r.removeFromLeft (18);

    g.drawText (text, r, juce::Justification::centredLeft, true);
}

// 選択中のボタンをミントで塗るか。既定はメインのプリセットボタン（ノーマル以外、全体バイパスでない）。マイク処理ウィンドウのEQプリセットボタンは
// 全体バイパスと関係なく決めるため、ボタンごとのプロパティ"liveState"があればそれを優先する（スライダーと同じ。design.md 10.7節）。
bool AppLookAndFeel::isLiveButton (const juce::Button& button, int presetIdx) const noexcept
{
    if (button.getProperties().contains ("liveState"))
        return (bool) button.getProperties()["liveState"];

    return presetIdx != (int) Preset::Normal && isChainEnabled();
}

void AppLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                            bool isHighlighted, bool isDown)
{
    const auto bounds = button.getLocalBounds().toFloat();
    const bool selected = button.getToggleState();
    const int presetIdx = (int) button.getProperties().getWithDefault ("presetIndex", -1);
    const bool liveStyle = selected && isLiveButton (button, presetIdx);

    juce::Colour bg (surface);
    juce::Colour border (lineBorder);
    float borderWidth = 1.0f;
    bool noBorder = false;

    if (! button.isEnabled())
    {
        // 無効（「元に戻す」で取り消せる操作がないとき）: 背景surface、枠1px line.divider。ホバー・押下で変えない（design.md 10.3節）。
        border = juce::Colour (lineDivider);
    }
    else if (liveStyle)
    {
        bg = juce::Colour (live);
        noBorder = true;
    }
    else if (selected)
    {
        border = juce::Colour (textSecondary);
        borderWidth = 2.0f;
    }
    else if (isDown)
    {
        bg = juce::Colour (surfacePressed);
        border = juce::Colour (lineBorderHover);
    }
    else if (isHighlighted)
    {
        bg = juce::Colour (surfaceHover);
        border = juce::Colour (lineBorderHover);
    }

    g.setColour (bg);
    g.fillRoundedRectangle (bounds, 4.0f);

    if (! noBorder)
    {
        g.setColour (border);
        g.drawRoundedRectangle (bounds.reduced (borderWidth * 0.5f), 4.0f, borderWidth);
    }

    if (button.hasKeyboardFocus (true))
    {
        g.setColour (juce::Colour (liveStyle ? liveInk : focus));
        g.drawRoundedRectangle (bounds.reduced (3.0f), 3.0f, 2.0f);
    }
}

void AppLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    const bool selected = button.getToggleState();
    const int presetIdx = (int) button.getProperties().getWithDefault ("presetIndex", -1);
    const bool liveStyle = selected && isLiveButton (button, presetIdx);

    g.setColour (juce::Colour (! button.isEnabled() ? textDisabled : (liveStyle ? liveInk : textPrimary)));
    g.setFont (uiFont (14.0f, selected));

    g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (4, 0),
                       juce::Justification::centred, 1, 0.85f);
}

juce::Font AppLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return uiFont (14.0f);
}

void AppLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float, float,
                                        juce::Slider::SliderStyle, juce::Slider& slider)
{
    const bool zeroBased = (bool) slider.getProperties().getWithDefault ("zeroBasedFill", false);

    // 塗りの色: 既定は全体バイパス（isChainEnabled）で決める。マイク処理ウィンドウのスライダーは全体バイパスと
    // 関係なくノイズ除去のON/OFFで決めるため、スライダーごとのプロパティ"liveState"があればそれを優先する（design.md 10.7節）。
    const bool isLive = slider.getProperties().contains ("liveState") ? (bool) slider.getProperties()["liveState"]
                                                                     : isChainEnabled();
    const juce::Colour fillColour (isLive ? live : idleFill);

    const float trackY = (float) y + (float) height * 0.5f - 2.0f;
    g.setColour (juce::Colour (track));
    g.fillRoundedRectangle ((float) x, trackY, (float) width, 4.0f, 2.0f);

    const float zeroPos = zeroBased ? (float) slider.valueToProportionOfLength (0.0) * (float) width + (float) x
                                     : (float) x;
    const float fillStart = juce::jmin (zeroPos, sliderPos);
    const float fillEnd = juce::jmax (zeroPos, sliderPos);

    if (fillEnd > fillStart)
    {
        g.setColour (fillColour);
        g.fillRoundedRectangle (fillStart, trackY, fillEnd - fillStart, 4.0f, 2.0f);
    }

    if (zeroBased)
    {
        g.setColour (juce::Colour (lineBorderHover));
        g.fillRect (juce::Rectangle<float> (zeroPos - 0.5f, (float) y + (float) height * 0.5f - 5.0f, 1.0f, 10.0f));
    }

    constexpr float thumbW = 10.0f, thumbH = 20.0f;
    const juce::Rectangle<float> thumb (sliderPos - thumbW * 0.5f, (float) y + (float) height * 0.5f - thumbH * 0.5f,
                                         thumbW, thumbH);

    g.setColour (slider.isMouseOverOrDragging() ? juce::Colours::white : juce::Colour (textPrimary));
    g.fillRoundedRectangle (thumb, 2.0f);
    g.setColour (juce::Colour (bgWindow));
    g.drawRoundedRectangle (thumb, 2.0f, 1.0f);
    g.setColour (juce::Colour (0xFF7A7F87));
    g.fillRect (juce::Rectangle<float> (thumb.getCentreX() - 0.5f, thumb.getY() + 5.0f, 1.0f, 10.0f));

    // キーボードフォーカス: つまみの外側3pxに2pxのfocus（16×26、角丸4。design.md 3.5節）。塗りと値の色は変えない。
    if (slider.hasKeyboardFocus (false))
    {
        g.setColour (juce::Colour (focus));
        g.drawRoundedRectangle (thumb.expanded (3.0f), 4.0f, 2.0f);
    }
}

// ===== SECTION: LevelMeter =====

LevelMeter::LevelMeter()
{
    setInterceptsMouseClicks (false, false);
}

void LevelMeter::setDisconnected (bool isDisconnected) noexcept
{
    disconnected = isDisconnected;

    if (disconnected)
    {
        envelopeDb = kMinDb;
        peakHoldDb = kMinDb;
        peakHoldRemainingSec = 0.0;
    }
}

void LevelMeter::advance (float linearPeak, double dtSeconds) noexcept
{
    if (disconnected)
    {
        repaint();
        return;
    }

    const float instDb = linearPeak > 0.0f ? juce::jmax (kMinDb, juce::Decibels::gainToDecibels (linearPeak)) : kMinDb;

    envelopeDb = instDb > envelopeDb ? instDb : juce::jmax (kMinDb, envelopeDb - (float) (kFallDbPerSec * dtSeconds));

    if (instDb >= peakHoldDb)
    {
        peakHoldDb = instDb;
        peakHoldRemainingSec = 1.0;
    }
    else if (peakHoldRemainingSec > 0.0)
    {
        peakHoldRemainingSec -= dtSeconds;
    }
    else
    {
        peakHoldDb = juce::jmax (kMinDb, peakHoldDb - (float) (kFallDbPerSec * dtSeconds));
    }

    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    constexpr int segW = 5, segGap = 2;

    for (int i = 0; i < kNumSegments; ++i)
    {
        const float segStartDb = kMinDb + kSegmentDb * (float) i;
        const bool lit = (! disconnected) && envelopeDb >= segStartDb;

        juce::Colour c (AppLookAndFeel::meterOff);

        if (lit)
        {
            if (i <= 33)      c = juce::Colour (AppLookAndFeel::meterLit);
            else if (i <= 37) c = juce::Colour (AppLookAndFeel::warn);
            else              c = juce::Colour (AppLookAndFeel::error);
        }

        g.setColour (c);
        g.fillRoundedRectangle ((float) (i * (segW + segGap)), 0.0f, (float) segW, (float) getHeight(), 1.0f);
    }
}

// ===== SECTION: ToggleSwitch =====

ToggleSwitch::ToggleSwitch() : juce::Button ("effectToggle")
{
    setClickingTogglesState (true);
    setWantsKeyboardFocus (true);
}

void ToggleSwitch::setTexts (const juce::String& wordIn, const juce::String& onSubText, const juce::String& offSubText)
{
    word = wordIn;
    onSub = onSubText;
    offSub = offSubText;
    repaint();
}

void ToggleSwitch::setOffSubText (const juce::String& subText)
{
    if (offSub != subText)
    {
        offSub = subText;
        repaint();
    }
}

void ToggleSwitch::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    const bool on = getToggleState();
    const auto bounds = getLocalBounds().toFloat();

    juce::Colour bg, border;
    float borderWidth;

    if (on)
    {
        bg = isDown ? juce::Colour (AppLookAndFeel::liveTintPressed)
                    : (isHighlighted ? juce::Colour (AppLookAndFeel::liveTintHover) : juce::Colour (AppLookAndFeel::liveTint));
        border = juce::Colour (AppLookAndFeel::live);
        borderWidth = 1.5f;
    }
    else
    {
        bg = isDown ? juce::Colour (AppLookAndFeel::surfacePressed)
                    : (isHighlighted ? juce::Colour (AppLookAndFeel::surfaceHover) : juce::Colour (AppLookAndFeel::surface));
        border = juce::Colour (AppLookAndFeel::lineBorderHover);
        borderWidth = 1.0f;
    }

    g.setColour (bg);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (border);
    g.drawRoundedRectangle (bounds.reduced (borderWidth * 0.5f), 4.0f, borderWidth);

    // ランプ: 直径12、中心は部品内(24, 高さ/2)。メインのトグル（20,428・高さ48）ではdesign.md絶対座標(44,452)。
    constexpr float lampCx = 24.0f, lampR = 6.0f;
    const float lampCy = bounds.getHeight() * 0.5f;

    if (on)
    {
        g.setColour (juce::Colour (AppLookAndFeel::live));
        g.fillEllipse (lampCx - lampR, lampCy - lampR, lampR * 2.0f, lampR * 2.0f);
    }
    else
    {
        g.setColour (juce::Colour (AppLookAndFeel::idleFill));
        g.drawEllipse (lampCx - lampR, lampCy - lampR, lampR * 2.0f, lampR * 2.0f, 1.5f);
    }

    const auto mainFont = AppLookAndFeel::uiFont (16.0f, true);
    const auto subFont = AppLookAndFeel::uiFont (13.0f, false);

    // 主文は部品内x40・幅132に左寄せ、副文は部品内x176・幅228（右端x404）に右寄せ。2つの領域は重ねない（design.md 3.5節）。
    // ともに1行、drawFittedTextの最小横倍率0.9。文言は、主文が最長でも約113px、副文は16字（13ptで最大208px）に収める。
    const juce::Rectangle<int> mainArea (40, 0, 132, getHeight());
    const juce::Rectangle<int> subArea (176, 0, 228, getHeight());

    if (on)
    {
        juce::AttributedString as;
        as.setJustification (juce::Justification::centredLeft);
        as.append (word + " ", mainFont, juce::Colour (AppLookAndFeel::textPrimary));
        as.append ("ON", mainFont, juce::Colour (AppLookAndFeel::live));
        as.draw (g, mainArea.toFloat());
    }
    else
    {
        g.setColour (juce::Colour (AppLookAndFeel::textPrimary));
        g.setFont (mainFont);
        g.drawFittedText (word + " OFF", mainArea, juce::Justification::centredLeft, 1, 0.9f);
    }

    g.setColour (juce::Colour (AppLookAndFeel::textSecondary));
    g.setFont (subFont);
    g.drawFittedText (getDisplayedSubText(), subArea, juce::Justification::centredRight, 1, 0.9f);

    if (hasKeyboardFocus (true))
    {
        g.setColour (juce::Colour (AppLookAndFeel::focus));
        g.drawRoundedRectangle (bounds.reduced (3.0f), 3.0f, 2.0f);
    }
}

// ===== SECTION: MicButton =====

MicButton::MicButton() : juce::Button ("micButton")
{
    setButtonText (juce::String::fromUTF8 ("マイク処理"));
    setWantsKeyboardFocus (true);
}

void MicButton::setLampOn (bool on)
{
    if (lampOn != on)
    {
        lampOn = on;
        repaint();
    }
}

void MicButton::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    // 背景・枠・フォーカス輪郭は非選択のプリセットボタンと同じ（AppLookAndFeel::drawButtonBackground）。
    getLookAndFeel().drawButtonBackground (g, *this, {}, isHighlighted, isDown);

    // ランプ: 直径8、中心は部品内(16, 16)。ONはliveで塗り、両方OFFなら中抜き（線幅1.5、idle.fill）。
    constexpr float lampR = 4.0f;

    if (lampOn)
    {
        g.setColour (juce::Colour (AppLookAndFeel::live));
        g.fillEllipse (16.0f - lampR, 16.0f - lampR, lampR * 2.0f, lampR * 2.0f);
    }
    else
    {
        g.setColour (juce::Colour (AppLookAndFeel::idleFill));
        g.drawEllipse (16.0f - lampR + 0.75f, 16.0f - lampR + 0.75f, lampR * 2.0f - 1.5f, lampR * 2.0f - 1.5f, 1.5f);
    }

    // 文字: 部品内x26・幅68に左寄せ・上下中央（13pt Regular、text.primary、最小横倍率0.9）。
    g.setColour (juce::Colour (AppLookAndFeel::textPrimary));
    g.setFont (AppLookAndFeel::uiFont (13.0f));
    g.drawFittedText (getButtonText(), 26, 0, 68, getHeight(), juce::Justification::centredLeft, 1, 0.9f);
}

// ===== SECTION: StatusPanel =====

StatusPanel::StatusPanel()
{
    setInterceptsMouseClicks (false, false);
}

void StatusPanel::setData (const StatusData& newData)
{
    data = newData;
    repaint();
}

void StatusPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (AppLookAndFeel::bgInset));

    g.setColour (data.lineColour);
    g.fillRect (0, 0, getWidth(), 2);

    const auto uiSmall = AppLookAndFeel::uiFont (13.0f);
    const auto delayFont = AppLookAndFeel::numericFont (22.0f);
    const auto cpuFont = AppLookAndFeel::numericFont (18.0f);

    // ----- 1行目: 遅延 / CPU -----
    g.setColour (juce::Colour (AppLookAndFeel::textSecondary));
    g.setFont (uiSmall);
    g.drawText (juce::String::fromUTF8 ("遅延"), 20, 12, 36, 26, juce::Justification::centredLeft);

    juce::String delayText;
    juce::Colour delayColour (AppLookAndFeel::textPrimary);

    if (data.delayStarting)
    {
        delayText = "--.-";
    }
    else if (data.delayAsDash)
    {
        delayText = juce::String::fromUTF8 ("\xE2\x80\x94");
        delayColour = juce::Colour (AppLookAndFeel::error);
    }
    else
    {
        delayText = juce::String (data.delayMs, 1);
        delayColour = data.delayWarn ? juce::Colour (AppLookAndFeel::warn) : juce::Colour (AppLookAndFeel::textPrimary);
    }

    g.setColour (delayColour);
    g.setFont (delayFont);
    g.drawText (delayText, 60, 12, 72, 26, juce::Justification::centredRight);

    g.setColour (juce::Colour (AppLookAndFeel::textSecondary));
    g.setFont (uiSmall);
    g.drawText ("ms", 136, 12, 40, 26, juce::Justification::centredLeft);
    g.drawText ("CPU", 300, 12, 40, 26, juce::Justification::centredLeft);

    g.setColour (juce::Colour (AppLookAndFeel::textPrimary));
    g.setFont (cpuFont);
    g.drawText (juce::String (data.cpuPercent, 1), 340, 12, 72, 26, juce::Justification::centredRight);

    g.setColour (juce::Colour (AppLookAndFeel::textSecondary));
    g.setFont (uiSmall);
    g.drawText ("%", 416, 12, 30, 26, juce::Justification::centredLeft);

    // ----- 2行目: 内訳（design.md 6.3節、AttributedStringで1行に描く） -----
    {
        juce::AttributedString as;
        as.setJustification (juce::Justification::centredLeft);

        const auto wordFont = AppLookAndFeel::uiFont (12.0f);
        const auto numFont = AppLookAndFeel::numericFont (12.0f);
        const juce::Colour secondary (AppLookAndFeel::textSecondary);
        const juce::Colour primary (AppLookAndFeel::textPrimary);

        as.append (juce::String::fromUTF8 ("内訳  デバイス "), wordFont, secondary);
        as.append (juce::String (data.deviceMs, 1), numFont, primary);
        as.append (juce::String::fromUTF8 (" ＋ バッファ "), wordFont, secondary);
        as.append (juce::String (data.bufferMs, 1), numFont, primary);
        as.append (juce::String::fromUTF8 (" ＋ 除去 "), wordFont, secondary);

        if (data.noiseResting)
            as.append ("OFF", wordFont, secondary);
        else
            as.append (juce::String (data.noiseMs, 1), numFont, primary);

        as.append (juce::String::fromUTF8 (" ＋ ピッチ "), wordFont, secondary);

        if (data.shifterResting)
            as.append (juce::String::fromUTF8 ("休止"), wordFont, secondary);
        else
            as.append (juce::String (data.shifterMs, 1), numFont, primary);

        as.append (" ms", wordFont, secondary);
        as.draw (g, juce::Rectangle<float> (20.0f, 42.0f, 420.0f, 16.0f));
    }

    // ----- 3行目: デバイスのモード -----
    g.setFont (uiSmall);
    g.setColour (juce::Colour (AppLookAndFeel::textSecondary));
    g.drawText (juce::String::fromUTF8 ("入力"), 20, 60, 32, 16, juce::Justification::centredLeft);

    g.setColour (data.inputModeWarn ? juce::Colour (AppLookAndFeel::warn) : juce::Colour (AppLookAndFeel::textPrimary));
    g.drawText (data.inputModeText, 52, 60, 100, 16, juce::Justification::centredLeft);

    g.setColour (juce::Colour (AppLookAndFeel::textSecondary));
    g.drawText (juce::String::fromUTF8 ("出力"), 220, 60, 32, 16, juce::Justification::centredLeft);

    g.setColour (data.outputModeWarn ? juce::Colour (AppLookAndFeel::warn) : juce::Colour (AppLookAndFeel::textPrimary));
    g.drawText (data.outputModeText, 252, 60, 150, 16, juce::Justification::centredLeft);

    // ----- メッセージのアイコン + 本文 -----
    if (data.icon != StatusData::IconKind::None)
    {
        const bool isError = data.icon == StatusData::IconKind::Error;
        g.setColour (isError ? juce::Colour (AppLookAndFeel::error) : juce::Colour (AppLookAndFeel::warn));

        if (isError)
        {
            g.fillEllipse (20.0f, 91.0f, 12.0f, 12.0f);
        }
        else
        {
            juce::Path tri;
            tri.addTriangle (26.0f, 91.0f, 20.0f, 103.0f, 32.0f, 103.0f);
            g.fillPath (tri);
        }

        g.setColour (juce::Colour (AppLookAndFeel::bgWindow));
        g.setFont (AppLookAndFeel::uiFont (10.0f, true));
        g.drawText ("!", 20, 91, 12, 12, juce::Justification::centred);
    }

    g.setColour (data.messageColour);
    g.setFont (AppLookAndFeel::uiFont (13.0f, data.messageBold));
    g.drawFittedText (data.message, 40, 88, 400, 40, juce::Justification::topLeft, 2, 1.0f);
}

// ===== SECTION: MainComponent =====

MainComponent::MainComponent (AudioIO& audioIOIn, juce::PropertiesFile& settingsIn)
    : audioIO (audioIOIn), settings (settingsIn)
{
    setLookAndFeel (&lookAndFeel);
    tooltipWindow.setLookAndFeel (&lookAndFeel);
    lookAndFeel.setEnabledFlag (&audioIO.engineParams().enabled);

    setSize (460, 640);
    setTitle (juce::String::fromUTF8 ("VoiceChange"));

    vbCableMissing = ! containsCableInput (audioIO.getOutputNames());

    auto setupLabel = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (AppLookAndFeel::uiFont (13.0f));
        l.setColour (juce::Label::textColourId, juce::Colour (AppLookAndFeel::textSecondary));
        l.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
    };

    setupLabel (inputLabel,  juce::String::fromUTF8 ("入力"));
    setupLabel (outputLabel, juce::String::fromUTF8 ("出力"));
    setupLabel (levelLabel,  juce::String::fromUTF8 ("レベル"));
    setupLabel (gainLabel,   juce::String::fromUTF8 ("ゲイン"));
    setupLabel (pitchLabel,  juce::String::fromUTF8 ("ピッチ"));
    setupLabel (reverbLabel, juce::String::fromUTF8 ("リバーブ"));

    inputCombo.setTitle (juce::String::fromUTF8 ("入力デバイス"));
    outputCombo.setTitle (juce::String::fromUTF8 ("出力デバイス"));

    syncDeviceCombos();

    inputCombo.onChange = [this] { deviceComboChanged (true); };
    outputCombo.onChange = [this] { deviceComboChanged (false); };

    // Tab順（design.md 5章）: 入力(1) → マイク処理(2) → 出力(3) → ゲイン(4) → ピッチ(5) → リバーブ(6) → プリセット(7〜14) → 音域(15〜16。トークボックス選択中だけ) → トグル(17)。
    inputCombo.setExplicitFocusOrder (1);
    micButton.setExplicitFocusOrder (2);
    outputCombo.setExplicitFocusOrder (3);

    micButton.setTitle (juce::String::fromUTF8 ("マイク処理（ノイズ除去・EQ）の設定"));
    micButton.onClick = [this]
    {
        if (onOpenMicPanel != nullptr)
            onOpenMicPanel();
    };

    addAndMakeVisible (inputCombo);
    addAndMakeVisible (micButton);
    addAndMakeVisible (outputCombo);

    levelMeter.setTitle (juce::String::fromUTF8 ("入力レベル"));
    addAndMakeVisible (levelMeter);

    levelValueLabel.setFont (AppLookAndFeel::uiFont (14.0f));
    levelValueLabel.setColour (juce::Label::textColourId, juce::Colour (AppLookAndFeel::textSecondary));
    levelValueLabel.setJustificationType (juce::Justification::centredRight);
    levelValueLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (levelValueLabel);

    auto setupSlider = [this] (juce::Slider& s, double lo, double hi, bool zeroBased, const juce::String& title,
                                std::function<juce::String (double)> fmt, const juce::String& tip)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, true, 64, 32);
        s.setRange (lo, hi, 1.0);
        s.setDoubleClickReturnValue (true, 0.0);
        s.setSliderSnapsToMousePosition (false);
        s.getProperties().set ("zeroBasedFill", zeroBased);
        s.textFromValueFunction = std::move (fmt);
        s.setTitle (title);
        s.setTooltip (tip);
        s.setWantsKeyboardFocus (true); // Tab順・矢印キー・フォーカス輪郭（design.md 3.5節・5章）。JUCEのSliderは既定ではフォーカスを受けない
        addAndMakeVisible (s);
    };

    setupSlider (gainSlider, -20.0, 20.0, true, juce::String::fromUTF8 ("ゲイン（音量）"),
        [] (double v) { return (v > 0.0 ? juce::String ("+") : juce::String()) + juce::String ((int) v) + " dB"; },
        juce::String::fromUTF8 ("ダブルクリックで初期値（0 dB）に戻します"));

    setupSlider (pitchSlider, -(double) kMaxLayer1PitchSemitones, (double) kMaxLayer1PitchSemitones, true, juce::String::fromUTF8 ("ピッチ（音程）"),
        [] (double v) { return (v > 0.0 ? juce::String ("+") : juce::String()) + juce::String ((int) v) + juce::String::fromUTF8 (" 半音"); },
        juce::String::fromUTF8 ("ダブルクリックで初期値（0 半音）に戻します"));

    setupSlider (reverbSlider, 0.0, 100.0, false, juce::String::fromUTF8 ("リバーブ（残響）"),
        [] (double v) { return juce::String ((int) v) + " %"; },
        juce::String::fromUTF8 ("ダブルクリックで初期値（0 %）に戻します"));

    gainSlider.setExplicitFocusOrder (4);
    pitchSlider.setExplicitFocusOrder (5);
    reverbSlider.setExplicitFocusOrder (6);

    auto& ap = audioIO.engineParams();
    gainSlider.setValue (ap.gainDb.load(), juce::dontSendNotification);
    pitchSlider.setValue ((double) ap.pitch.load(), juce::dontSendNotification);
    reverbSlider.setValue ((double) ap.reverb.load() * 100.0, juce::dontSendNotification);

    // Slider::setValue()は値が実際に変わらない限りテキストボックスを更新しない
    // （既定値0のスライダーに対して初期値0をsetValueしても素通りする）ため、
    // textFromValueFunctionをまだ反映していない初期表示("0"のみ)を明示的に更新する。
    gainSlider.updateText();
    pitchSlider.updateText();
    reverbSlider.updateText();

    gainSlider.onValueChange = [this] { applyGainFromSlider(); };
    pitchSlider.onValueChange = [this] { applyPitchFromSlider(); };
    reverbSlider.onValueChange = [this] { applyReverbFromSlider(); };

    updateSliderAppearance (gainSlider);
    updateSliderAppearance (pitchSlider);
    updateSliderAppearance (reverbSlider);

    createPresetButtons();
    createRangeButtons();

    toggleButton.setTexts (juce::String::fromUTF8 ("エフェクト"), juce::String::fromUTF8 ("加工した声を出力中"),
                           juce::String::fromUTF8 ("原音をそのまま出力中（バイパス）"));
    toggleButton.setToggleState (ap.enabled.load(), juce::dontSendNotification);
    toggleButton.setTitle (juce::String::fromUTF8 ("エフェクト全体のON/OFF"));
    toggleButton.setExplicitFocusOrder (17);
    toggleButton.onClick = [this] { applyToggle(); };
    addAndMakeVisible (toggleButton);

    statusPanel.setTitle (juce::String::fromUTF8 ("状態表示"));
    addAndMakeVisible (statusPanel);

    updateStatus (true);
}

MainComponent::~MainComponent()
{
    stopTimer();
    tooltipWindow.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void MainComponent::createPresetButtons()
{
    struct PresetUiInfo { Preset preset; const char* tooltip; };

    // design.md 3.3節のレイアウト表どおりの表示順（種類別。内部のPreset/kPresetsの並びとは異なる）。
    // UI表示名(ボタンのラベル)はvc::presetDisplayName()を正とする(src/core/Params.h。トレイの
    // ツールチップと共有)。ここではその並び順とツールチップの説明文だけを持つ。
    static const PresetUiInfo kOrder[8] = {
        { Preset::Normal,   "特殊効果なし（音響卓の設定だけを反映）" },
        { Preset::Helium,   "声を高く細く軽くして、ヘリウムを吸ったような声にする" },
        { Preset::Minion,   "声を高くして、小さなキャラクター風にする" },
        { Preset::Giant,    "声を低くして、大柄なキャラクター風にする" },
        { Preset::Echo,     "やまびこのように声が繰り返し響く" },
        { Preset::Kerokero, "音程を半音単位に吸着させる" },
        { Preset::Robot,    "金属的な響きのロボット声にする" },
        { Preset::Talkbox,  "声の高さではなく、決まったフレーズの音程でしゃべるボコーダー声にする" },
    };

    constexpr int colX[4] = { 20, 127, 234, 341 };
    constexpr int rowY[2] = { 278, 330 };

    for (int i = 0; i < 8; ++i)
    {
        const auto& info = kOrder[(size_t) i];
        const juce::String label = presetDisplayName (info.preset);

        auto button = std::make_unique<juce::TextButton> (label);
        button->setClickingTogglesState (true);
        button->setRadioGroupId (1001, juce::dontSendNotification);
        button->getProperties().set ("presetIndex", (int) info.preset);
        button->setTitle (label);
        button->setDescription (juce::String::fromUTF8 (info.tooltip));
        button->setTooltip (juce::String::fromUTF8 (info.tooltip));
        button->setExplicitFocusOrder (7 + i);
        button->setBounds (colX[i % 4], rowY[i / 4], 99, 44);

        const Preset presetValue = info.preset;
        button->onClick = [this, presetValue]
        {
            audioIO.engineParams().preset.store ((int) presetValue, std::memory_order_relaxed);
            settings.setValue ("preset", juce::String (kPresets[(size_t) presetValue].id));
            updatePresetButtonStates();
        };

        addAndMakeVisible (*button);
        presetButtons[(size_t) i] = std::move (button);
    }

    updatePresetButtonStates();
}

void MainComponent::updatePresetButtonStates()
{
    const int current = audioIO.engineParams().preset.load (std::memory_order_relaxed);

    for (auto& b : presetButtons)
    {
        if (b == nullptr)
            continue;

        const int idx = (int) b->getProperties().getWithDefault ("presetIndex", -1);
        b->setToggleState (idx == current, juce::dontSendNotification);
        b->repaint();
    }

    updateRangeButtonStates();
}

// design.md 3.5節「音域の2択」。ラベルと2つのボタンの表示はトークボックス選択中だけ（場所は常に空けてあり、ほかの部品は動かない）。
// 選択状態はAtomicParams::talkboxRange（保存済みの値で起動時に設定される）に合わせる。
void MainComponent::createRangeButtons()
{
    rangeLabel.setText (juce::String::fromUTF8 ("音域"), juce::dontSendNotification);
    rangeLabel.setFont (AppLookAndFeel::uiFont (13.0f));
    rangeLabel.setColour (juce::Label::textColourId, juce::Colour (AppLookAndFeel::textSecondary));
    rangeLabel.setInterceptsMouseClicks (false, false);
    rangeLabel.setBounds (20, 382, 60, 32);
    addChildComponent (rangeLabel);

    const char* tips[2] = { "低めの音域で鳴らします（F2〜D3のフレーズ）", "1オクターブ高い音域で鳴らします（F3〜D4のフレーズ）" };

    for (int i = 0; i < 2; ++i)
    {
        const auto range = static_cast<TalkboxRange> (i);
        const juce::String label = talkboxRangeDisplayName (range);

        auto button = std::make_unique<juce::TextButton> (label);
        button->setClickingTogglesState (true);
        button->setRadioGroupId (1002, juce::dontSendNotification); // プリセット（1001）とは別のグループ。UIAにラジオボタンとして公開される
        button->setTitle (juce::String::fromUTF8 ("音域 ") + label);
        button->setDescription (juce::String::fromUTF8 (tips[i]));
        button->setTooltip (juce::String::fromUTF8 (tips[i]));
        button->setExplicitFocusOrder (15 + i);
        button->setBounds (88 + 80 * i, 382, 72, 32);

        button->onClick = [this, range]
        {
            audioIO.engineParams().talkboxRange.store ((int) range, std::memory_order_relaxed);
            storeTalkboxRange (settings, range);
            updateRangeButtonStates();
        };

        addChildComponent (*button);
        rangeButtons[(size_t) i] = std::move (button);
    }

    updateRangeButtonStates();
}

void MainComponent::updateRangeButtonStates()
{
    const bool talkboxSelected = audioIO.engineParams().preset.load (std::memory_order_relaxed) == (int) Preset::Talkbox;
    const auto range = talkboxRangeFromInt (audioIO.engineParams().talkboxRange.load (std::memory_order_relaxed));

    rangeLabel.setVisible (talkboxSelected);

    for (size_t i = 0; i < rangeButtons.size(); ++i)
    {
        if (rangeButtons[i] == nullptr)
            continue;

        rangeButtons[i]->setToggleState (i == (size_t) range, juce::dontSendNotification);
        rangeButtons[i]->setVisible (talkboxSelected);
        rangeButtons[i]->repaint();
    }
}

void MainComponent::updateSliderAppearance (juce::Slider& slider)
{
    const bool bypassed = ! audioIO.engineParams().enabled.load (std::memory_order_relaxed);
    const bool atInitial = slider.getValue() == 0.0;

    juce::Colour c (AppLookAndFeel::textPrimary);
    if (! atInitial)
        c = bypassed ? juce::Colour (AppLookAndFeel::textSecondary) : juce::Colour (AppLookAndFeel::live);

    slider.setColour (juce::Slider::textBoxTextColourId, c);
    slider.repaint();
}

void MainComponent::applyGainFromSlider()
{
    const float v = (float) gainSlider.getValue();
    audioIO.engineParams().gainDb.store (v, std::memory_order_relaxed);
    settings.setValue ("gainDb", (double) v);
    updateSliderAppearance (gainSlider);
}

void MainComponent::applyPitchFromSlider()
{
    const int v = (int) pitchSlider.getValue();
    audioIO.engineParams().pitch.store (v, std::memory_order_relaxed);
    settings.setValue ("pitch", (double) v);
    updateSliderAppearance (pitchSlider);
}

void MainComponent::applyReverbFromSlider()
{
    const float v = (float) (reverbSlider.getValue() * 0.01);
    audioIO.engineParams().reverb.store (v, std::memory_order_relaxed);
    settings.setValue ("reverb", (double) v);
    updateSliderAppearance (reverbSlider);
}

void MainComponent::applyToggle()
{
    const bool on = toggleButton.getToggleState();
    audioIO.engineParams().enabled.store (on, std::memory_order_relaxed);
    settings.setValue ("enabled", on);

    refreshEnabledAppearance();
}

void MainComponent::refreshEnabledAppearance()
{
    updateSliderAppearance (gainSlider);
    updateSliderAppearance (pitchSlider);
    updateSliderAppearance (reverbSlider);

    for (auto& b : presetButtons)
        if (b != nullptr)
            b->repaint();

    for (auto& b : rangeButtons)
        if (b != nullptr)
            b->repaint();

    toggleButton.setDescription (toggleButton.getDisplayedSubText()); // 表示中の副文（design.md 5章）
}

void MainComponent::refreshMicAppearance (bool nrOn, EqPresetId eqPreset)
{
    if (micStateKnown && nrOn == shownNrOn && eqPreset == shownEqPreset)
        return;

    micStateKnown = true;
    shownNrOn = nrOn;
    shownEqPreset = eqPreset;

    // EQの部分は「EQ OFF / EQ A2 / EQ A3 / EQ カスタム」（eqStatusText。EQがOFFなら表示中のプリセットはEQなし = EQ OFF）。
    const bool eqOn = eqPreset != EqPresetId::None;
    const juce::String nrText = juce::String::fromUTF8 (nrOn ? "ノイズ除去 ON" : "ノイズ除去 OFF");

    micButton.setLampOn (nrOn || eqOn);
    micButton.setDescription (nrText + juce::String::fromUTF8 ("、") + eqStatusText (eqPreset));
    micButton.setTooltip (juce::String::fromUTF8 ("ノイズ除去とEQの設定を開きます（") + nrText
                          + juce::String::fromUTF8 ("・") + eqStatusText (eqPreset) + juce::String::fromUTF8 ("）"));

    // 全体トグルの副文（OFF時）: マイク処理がすべてOFFなら原音、どちらかがONなら「マイク処理のみ適用中」（D-020）。
    toggleButton.setOffSubText (nrOn || eqOn ? juce::String::fromUTF8 ("マイク処理のみ適用中（バイパス）")
                                             : juce::String::fromUTF8 ("原音をそのまま出力中（バイパス）"));
    toggleButton.setDescription (toggleButton.getDisplayedSubText());
}

void MainComponent::refreshDeviceCombo (juce::ComboBox& combo, const juce::StringArray& names,
                                         const juce::String& desiredName, juce::StringArray& realNamesOut)
{
    combo.clear (juce::dontSendNotification);
    realNamesOut.clear();

    combo.setTextWhenNothingSelected (juce::String::fromUTF8 ("デバイスを選択"));
    combo.setTextWhenNoChoicesAvailable (juce::String::fromUTF8 ("（利用できるデバイスがありません）"));

    const bool desiredMissing = desiredName.isNotEmpty() && ! names.contains (desiredName);

    int id = 1;
    int selectId = 0;

    if (desiredMissing)
    {
        combo.addItem (desiredName + juce::String::fromUTF8 ("（未接続）"), id);
        realNamesOut.add ({});
        selectId = id;
        ++id;
    }

    for (const auto& n : names)
    {
        combo.addItem (n, id);
        realNamesOut.add (n);

        if (n == desiredName)
            selectId = id;

        ++id;
    }

    combo.setSelectedId (selectId, juce::dontSendNotification);
}

void MainComponent::syncDeviceCombos()
{
    auto sync = [this] (DeviceComboBox& combo, juce::StringArray& realNames, juce::String& shownKey,
                         const juce::StringArray& names, const juce::String& desiredName)
    {
        const juce::String key = names.joinIntoString ("\n") + juce::String::charToString ((juce::juce_wchar) 0x1F) + desiredName;

        if (key == shownKey || combo.isPopupActive())
            return;

        // refreshDeviceCombo()はdontSendNotificationで構築するため、onChange（＝open()）は発火しない。
        refreshDeviceCombo (combo, names, desiredName, realNames);
        shownKey = key;
    };

    sync (inputCombo, inputComboRealNames, inputComboKey, audioIO.getInputNames(), audioIO.getDesiredInputName());
    sync (outputCombo, outputComboRealNames, outputComboKey, audioIO.getOutputNames(), audioIO.getDesiredOutputName());
}

void MainComponent::deviceComboChanged (bool isInputCombo)
{
    auto& combo = isInputCombo ? inputCombo : outputCombo;
    auto& realNames = isInputCombo ? inputComboRealNames : outputComboRealNames;

    const int id = combo.getSelectedId();
    if (id <= 0 || id > realNames.size())
        return;

    const juce::String chosen = realNames[id - 1];
    if (chosen.isEmpty())
        return; // 「(未接続)」の項目は実デバイスではないので何もしない

    const juce::String inName = isInputCombo ? chosen : audioIO.getDesiredInputName();
    const juce::String outName = isInputCombo ? audioIO.getDesiredOutputName() : chosen;

    audioIO.open (inName, outName);

    settings.setValue ("inputDevice", inName);
    settings.setValue ("outputDevice", outName);

    updateStatus (true);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (AppLookAndFeel::bgWindow));

    g.setColour (juce::Colour (AppLookAndFeel::lineDivider));
    g.fillRect (20, 94, 420, 1);
    g.fillRect (20, 140, 420, 1);
    g.fillRect (20, 266, 420, 1);
}

void MainComponent::resized()
{
    inputLabel.setBounds (20, 12, 60, 32);
    inputCombo.setBounds (88, 12, 240, 32);
    micButton.setBounds (336, 12, 104, 32);
    outputLabel.setBounds (20, 50, 60, 32);
    outputCombo.setBounds (88, 50, 352, 32);

    levelLabel.setBounds (20, 104, 60, 24);
    levelMeter.setBounds (93, 110, 278, 12);
    levelValueLabel.setBounds (376, 104, 64, 24);

    gainLabel.setBounds (20, 150, 60, 32);
    gainSlider.setBounds (88, 150, 352, 32);
    pitchLabel.setBounds (20, 186, 60, 32);
    pitchSlider.setBounds (88, 186, 352, 32);
    reverbLabel.setBounds (20, 222, 60, 32);
    reverbSlider.setBounds (88, 222, 352, 32);

    toggleButton.setBounds (20, 428, 420, 48);
    statusPanel.setBounds (0, 492, 460, 148);
}

void MainComponent::visibilityChanged()
{
    updateTimerRunState();
}

void MainComponent::parentHierarchyChanged()
{
    // setContentOwned()がこのコンポーネントを表示状態にする時点では、親のDocumentWindow自体は
    // まだsetVisible(true)を呼ばれておらずisShowing()はfalseを返す。DocumentWindow側の
    // setVisible(true)はinternalHierarchyChanged()経由で子孫のparentHierarchyChanged()を
    // 呼ぶため、初回表示はこちらで検出する。
    updateTimerRunState();
}

void MainComponent::updateTimerRunState()
{
    if (isShowing())
    {
        if (! isTimerRunning())
        {
            frameCounter = 0;
            startTimerHz (30);
            levelMeter.setDisconnected (! audioIO.isOpen()); // 最初のupdateStatus()前に反映しておく
            updateStatus (true);

            const int current = audioIO.engineParams().preset.load (std::memory_order_relaxed);
            for (auto& b : presetButtons)
            {
                if (b != nullptr && (int) b->getProperties().getWithDefault ("presetIndex", -1) == current)
                {
                    b->grabKeyboardFocus();
                    break;
                }
            }
        }
    }
    else
    {
        stopTimer();
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
            dw->closeButtonPressed();

        return true;
    }

    return false;
}

void MainComponent::timerCallback()
{
    constexpr double dt = 1.0 / 30.0;

    levelMeter.setDisconnected (! audioIO.isOpen());
    levelMeter.advance (audioIO.takeInputPeak(), dt);

    ++frameCounter;
    if (frameCounter >= 8)
    {
        frameCounter = 0;
        updateStatus (true);
    }

    // design.md 5章「状態パネル: 要約文を1秒に1回まで更新」。可視の数値(8フレーム=約267ms)より
    // 遅い周期でアクセシビリティの要約文だけを更新する（レビュー指摘4）。
    ++accessibilityFrameCounter;
    if (accessibilityFrameCounter >= 30)
    {
        accessibilityFrameCounter = 0;
        statusPanel.setDescription (buildStatusSummary (lastStatusData));
    }
}

void MainComponent::announceIfChanged (const juce::String& announceKey, bool isWarnOrError)
{
    if (! isWarnOrError)
    {
        lastAnnouncedKey = announceKey;
        return;
    }

    if (announceKey == lastAnnouncedKey)
        return;

    lastAnnouncedKey = announceKey;
    juce::AccessibilityHandler::postAnnouncement (announceKey, juce::AccessibilityHandler::AnnouncementPriority::high);
}

// design.md 5章の要約例「遅延 38.4ミリ秒、CPU 0.8パーセント、入力 低遅延、出力 低遅延、
// 正常に動作しています」に合わせた文言を組み立てる（レビュー指摘4）。
juce::String MainComponent::buildStatusSummary (const StatusData& d) const
{
    juce::String delayText;

    if (d.delayStarting)
        delayText = "--.-";
    else if (d.delayAsDash)
        delayText = juce::String::fromUTF8 ("\xE2\x80\x94");
    else
        delayText = juce::String (d.delayMs, 1);

    return juce::String::fromUTF8 ("遅延 ") + delayText + juce::String::fromUTF8 ("ミリ秒、CPU ")
           + juce::String (d.cpuPercent, 1) + juce::String::fromUTF8 ("パーセント、入力 ") + d.inputModeText
           + juce::String::fromUTF8 ("、出力 ") + d.outputModeText + juce::String::fromUTF8 ("、") + d.message;
}

void MainComponent::updateStatus (bool /*slowUpdate*/)
{
    syncDeviceCombos(); // 一覧・選択名が変わっていればコンボボックスを作り直す(以降の判定は同じ一覧を見る)

    const bool isOpen = audioIO.isOpen();
    const auto inputNames = audioIO.getInputNames();
    const auto outputNames = audioIO.getOutputNames();
    const bool noInputDevices = inputNames.isEmpty();
    const auto desiredInput = audioIO.getDesiredInputName();
    const auto desiredOutput = audioIO.getDesiredOutputName();

    // ----- デバイス切断・再接続中(design.md 6.1節 E1〜E3、6.2節) -----
    // 一度は動いていた世代が異常になった場合のみ(D-016)。
    const bool reconnecting = audioIO.isReconnecting();

    // ----- デバイスを開けない(E4)。一度もstartに成功していない世代の失敗。失敗側の行を赤くする -----
    const auto failedSide = audioIO.getFailedSide();
    const bool openFailed = (! isOpen) && (! reconnecting) && failedSide != AudioIO::FailedSide::None;
    const bool inputSideFailed = openFailed && (failedSide == AudioIO::FailedSide::Input || failedSide == AudioIO::FailedSide::Both);
    const bool outputSideFailed = openFailed && (failedSide == AudioIO::FailedSide::Output || failedSide == AudioIO::FailedSide::Both);

    // ----- コンボボックスのエラー表示(design.md 3.5節「切断状態」) -----
    const bool inputMissingFromList = desiredInput.isNotEmpty() && ! inputNames.contains (desiredInput);
    const bool outputMissingFromList = desiredOutput.isNotEmpty() && ! outputNames.contains (desiredOutput);

    // 保存デバイス名が一覧から消えていれば入力/出力を個別に特定できる(E1/E2)。どちらも一覧にある
    // (ドライバが無言で止まった等)場合はE3とし、どちらの行が原因か特定できないため両方を赤くする。
    // reconnectHeadは経過秒数を除いた文言(読み上げ通知のキーにも使う。毎秒変わる文言を通知し続けないため)。
    bool reconnectInputRed = false, reconnectOutputRed = false;
    juce::String reconnectHead, reconnectMessage;

    if (reconnecting)
    {
        const juce::String elapsedText = juce::String::fromUTF8 ("（") + juce::String ((int) audioIO.getReconnectElapsedSeconds())
                                         + juce::String::fromUTF8 ("秒経過）…");

        if (inputMissingFromList)
        {
            reconnectHead = juce::String::fromUTF8 ("入力デバイスが切断されました。再接続を試みています");
            reconnectInputRed = true;
        }
        else if (outputMissingFromList)
        {
            reconnectHead = juce::String::fromUTF8 ("出力デバイスが切断されました。再接続を試みています");
            reconnectOutputRed = true;
        }
        else
        {
            reconnectHead = juce::String::fromUTF8 ("音声が止まっています。デバイスを開き直しています");
            reconnectInputRed = true;
            reconnectOutputRed = true;
        }

        reconnectMessage = reconnectHead + elapsedText;
    }

    const bool inputError = inputMissingFromList || inputSideFailed || noInputDevices || reconnectInputRed;
    const bool outputError = outputMissingFromList || outputSideFailed || reconnectOutputRed;

    inputCombo.getProperties().set ("errorState", inputError);
    outputCombo.getProperties().set ("errorState", outputError);
    inputCombo.repaint();
    outputCombo.repaint();
    inputLabel.setColour (juce::Label::textColourId,
        inputError ? juce::Colour (AppLookAndFeel::error) : juce::Colour (AppLookAndFeel::textSecondary));
    outputLabel.setColour (juce::Label::textColourId,
        outputError ? juce::Colour (AppLookAndFeel::error) : juce::Colour (AppLookAndFeel::textSecondary));

    // ----- E5(異常値・例外): 検出後10秒間表示する(design.md 6.1節) -----
    // 元のフラグ(Engine::getErrorFlags())はAudioIOの500msタイマーだけが読んで消費する(複数箇所からの
    // 重複クリアを避けるため。T-007でトレイも同じ情報を必要とするようになった)。
    const bool e5Active = audioIO.hasRecentEngineError();

    const auto latency = audioIO.getLatency();
    const bool shifterResting = latency.shifterMs <= 0.0;
    const double excludingShifter = latency.deviceInMs + latency.deviceOutMs + latency.ringBufferMs;
    const bool w2Active = isOpen && excludingShifter > 48.0;

    const auto inInfo = audioIO.getInputInfo();
    const auto outInfo = audioIO.getOutputInfo();

    StatusData data;
    data.cpuPercent = (double) audioIO.getCpuLoad() * 100.0;
    data.deviceMs = latency.deviceInMs + latency.deviceOutMs;
    data.bufferMs = latency.ringBufferMs;
    data.shifterMs = latency.shifterMs;
    data.shifterResting = shifterResting;
    data.noiseMs = latency.noiseMs;
    data.noiseResting = latency.noiseMs <= 0.0;
    data.delayWarn = w2Active;
    data.delayMs = latency.totalMs;

    data.inputModeText = isOpen ? (inInfo.lowLatency ? juce::String::fromUTF8 ("低遅延") : juce::String::fromUTF8 ("通常"))
                                 : juce::String::fromUTF8 ("\xE2\x80\x94");
    data.outputModeText = isOpen ? (outInfo.lowLatency ? juce::String::fromUTF8 ("低遅延") : juce::String::fromUTF8 ("通常"))
                                  : juce::String::fromUTF8 ("\xE2\x80\x94");
    data.inputModeWarn = w2Active && ! inInfo.lowLatency;
    data.outputModeWarn = w2Active && ! outInfo.lowLatency;

    const bool engineEnabled = audioIO.engineParams().enabled.load (std::memory_order_relaxed);
    const bool bypassed = ! engineEnabled;

    // マイク処理（ノイズ除去・EQ）は全体バイパスの対象外（D-020）。ボタンのランプ・全体トグルの副文・バイパス中の文言に反映する。
    const bool nrOn = audioIO.engineParams().nrEnabled.load (std::memory_order_relaxed);
    const auto eqState = audioIO.engineParams().loadEq();
    const bool eqOn = eqState.on;
    refreshMicAppearance (nrOn, deriveEqPreset (eqState));

    // トレイのメニューからON/OFFを切り替えた場合、このウィンドウ側の表示にも反映する(T-007)。
    // クリック起点の変更はapplyToggle()がその場で反映するため、ここでは食い違いのときだけ追従する。
    if (toggleButton.getToggleState() != engineEnabled)
    {
        toggleButton.setToggleState (engineEnabled, juce::dontSendNotification);
        refreshEnabledAppearance();
    }

    juce::String message;
    bool isWarnOrError = false;

    juce::String announceKey; // 空ならmessageそのもの。経過秒数入りの文言は秒数を除いたキーにする

    if ((! isOpen) && failedSide == AudioIO::FailedSide::None && ! noInputDevices && ! reconnecting)
    {
        // 起動中(design.md 6.1節)。Main.cppは起動時に同期的にopen()するため通常はほぼ一瞬で終わる。
        data.delayStarting = true;
        message = juce::String::fromUTF8 ("デバイスを開いています…");
    }
    else if (reconnecting)
    {
        // E1〜E3(design.md 6.1節・6.2節、優先順位1〜2)。デバイス切断・再接続中は遅延を"—"にする。
        data.delayAsDash = true;
        message = reconnectMessage;
        announceKey = reconnectHead;
        data.icon = StatusData::IconKind::Error;
        data.messageBold = true;
        data.lineColour = juce::Colour (AppLookAndFeel::error);
        data.messageColour = juce::Colour (AppLookAndFeel::error);
        isWarnOrError = true;
    }
    else if (noInputDevices)
    {
        data.delayAsDash = true;
        message = juce::String::fromUTF8 ("入力デバイスが見つかりません。マイクを接続してください。");
        data.icon = StatusData::IconKind::Error;
        data.messageBold = true;
        data.lineColour = juce::Colour (AppLookAndFeel::error);
        data.messageColour = juce::Colour (AppLookAndFeel::error);
        isWarnOrError = true;
    }
    else if (openFailed)
    {
        data.delayAsDash = true;
        const juce::String side = (inputSideFailed && outputSideFailed) ? juce::String::fromUTF8 ("入力・出力")
                                   : inputSideFailed ? juce::String::fromUTF8 ("入力")
                                                     : juce::String::fromUTF8 ("出力");
        message = side + juce::String::fromUTF8 ("デバイスを開けませんでした。別のデバイスを選んでください。");
        data.icon = StatusData::IconKind::Error;
        data.messageBold = true;
        data.lineColour = juce::Colour (AppLookAndFeel::error);
        data.messageColour = juce::Colour (AppLookAndFeel::error);
        isWarnOrError = true;
    }
    else if (e5Active)
    {
        message = juce::String::fromUTF8 ("音声処理で異常を検出したため、一瞬無音にして復旧しました（")
                   + audioIO.getLastEngineErrorTimeText() + juce::String::fromUTF8 ("）");
        data.icon = StatusData::IconKind::Error;
        data.messageBold = true;
        data.lineColour = juce::Colour (AppLookAndFeel::error);
        data.messageColour = juce::Colour (AppLookAndFeel::error);
        isWarnOrError = true;
    }
    else if (vbCableMissing)
    {
        message = juce::String::fromUTF8 ("VB-CABLEが見つかりません。Discord等へ声を渡すにはインストールが必要です。");
        data.icon = StatusData::IconKind::Warn;
        data.messageBold = true;
        data.lineColour = juce::Colour (AppLookAndFeel::warn);
        data.messageColour = juce::Colour (AppLookAndFeel::warn);
        isWarnOrError = true;
    }
    else if (w2Active)
    {
        const juce::String which = (data.inputModeWarn && data.outputModeWarn) ? juce::String::fromUTF8 ("入出力デバイス")
                                    : data.inputModeWarn ? juce::String::fromUTF8 ("入力デバイス")
                                                          : juce::String::fromUTF8 ("出力デバイス");
        message = juce::String::fromUTF8 ("デバイスとバッファの遅延が目標を超えています。（") + which
                   + juce::String::fromUTF8 ("が「通常」モードのためです）");
        data.icon = StatusData::IconKind::Warn;
        data.messageBold = true;
        data.lineColour = juce::Colour (AppLookAndFeel::warn);
        data.messageColour = juce::Colour (AppLookAndFeel::warn);
        isWarnOrError = true;
    }
    else if (bypassed)
    {
        // design.md 6.1節: どのマイク処理がかかっているかで4通りに出し分ける。
        if (nrOn && eqOn)  message = juce::String::fromUTF8 ("バイパス中：ノイズ除去とEQだけをかけて出力しています");
        else if (nrOn)     message = juce::String::fromUTF8 ("バイパス中：ノイズ除去だけをかけて出力しています");
        else if (eqOn)     message = juce::String::fromUTF8 ("バイパス中：EQだけをかけて出力しています");
        else               message = juce::String::fromUTF8 ("バイパス中：原音をそのまま出力しています");
    }
    else
    {
        message = juce::String::fromUTF8 ("正常に動作しています");
    }

    data.message = message;
    statusPanel.setData (data);
    lastStatusData = data; // buildStatusSummary()（1秒に1回のアクセシビリティ更新）用

    // ----- メーターの数値(design.md 3.5節) -----
    juce::String meterText;
    juce::Colour meterColour (AppLookAndFeel::textSecondary);

    if (levelMeter.isDisconnected())
    {
        meterText = juce::String::fromUTF8 ("\xE2\x80\x94");
    }
    else if (levelMeter.getPeakHoldDb() <= -59.9f)
    {
        meterText = juce::String::fromUTF8 ("無音");
    }
    else
    {
        const float peak = levelMeter.getPeakHoldDb();
        meterText = juce::String ((int) std::lround (peak)) + " dB";

        if (peak >= -0.5f)      meterColour = juce::Colour (AppLookAndFeel::error);
        else if (peak > -3.0f)  meterColour = juce::Colour (AppLookAndFeel::warn);
    }

    levelValueLabel.setText (meterText, juce::dontSendNotification);
    levelValueLabel.setColour (juce::Label::textColourId, meterColour);

    announceIfChanged (announceKey.isEmpty() ? message : announceKey, isWarnOrError);
}

} // namespace vc
