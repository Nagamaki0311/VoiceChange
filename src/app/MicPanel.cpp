#include "MicPanel.h"

#include "core/MicProcessing.h"

#include <cmath>

// ===== SECTION: MicPanel実装 =====
// docs/design.md 10章のレイアウト・部品仕様に対応する。UIスレッド（メッセージスレッド）だけがAtomicParamsへ書く。

namespace vc
{

namespace
{

using L = AppLookAndFeel;

// 10.3節のツールチップ（スクリーンリーダーの説明にも同じ文を使う）。
const char* const kNrSwitchTip = "エフェクト全体をOFFにしても、ノイズ除去はかかったままです。ONにすると声の遅れが約30 ms増えます";
const char* const kEqSwitchTip = "エフェクト全体をOFFにしても、EQはかかったままです";
const char* const kBackgroundTip = "空調やファンなど続く雑音を抑える強さ。50 %を超えると、話していない間の音も下げます。ダブルクリックで初期値（70 %）に戻します";
const char* const kImpactTip = "キーボードやクリックなど短い音を抑える強さ。0 %では働きません。話し声と重なった音は残ります。ダブルクリックで初期値（0 %）に戻します";
const char* const kTypeTip = "フィルターの種類。ローカットは低い音を削り（ハイパス）、ハイカットは高い音を削ります（ローパス）";
const char* const kHzTip = "20〜20000 Hz。クリックして入力（1.2k のようにkも使えます）。ホイールと↑↓で1/12オクターブずつ";
const char* const kGainTip = "-18.0〜+18.0 dB。クリックして入力。ホイールと↑↓で0.5 dBずつ";
const char* const kQTip = "0.10〜10.00。大きいほど狭い範囲にかかります。ホイールと↑↓で約6 %ずつ";
const char* const kGainUnusedTip = "このタイプではゲインを使いません";
const char* const kResetTip = "5つのバンドの値を初期値に戻します。ノイズ除去とEQのON/OFFは変わりません";
const char* const kUndoTip = "初期値に戻す前の値に戻します";

juce::String utf8 (const char* text)
{
    return juce::String::fromUTF8 (text);
}

// 数値欄の編集用TextEditor。↑↓とTabは、Labelの既定（↑↓は行の移動、Tabはコンテナの中で循環）ではなく、欄の動作にする。
class FieldEditor final : public juce::TextEditor
{
public:
    explicit FieldEditor (NumberField& ownerIn) : juce::TextEditor (ownerIn.getName()), owner (ownerIn) {}

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        {
            owner.requestStepFromEditor (key == juce::KeyPress::upKey ? 1 : -1);
            return true;
        }

        if (key.isKeyCode (juce::KeyPress::tabKey))
        {
            owner.requestTabFromEditor (! key.getModifiers().isShiftDown());
            return true;
        }

        return juce::TextEditor::keyPressed (key);
    }

private:
    NumberField& owner;
};

// Windows UIAへの公開。題名・説明は部品の設定（setTitle / setDescription）を使い、値は表示と同じ文字列（例「1200 Hz」）。
class NumberFieldAccessibility final : public juce::AccessibilityHandler
{
public:
    explicit NumberFieldAccessibility (NumberField& fieldIn)
        : juce::AccessibilityHandler (fieldIn,
                                      fieldIn.isEnabled() ? juce::AccessibilityRole::editableText : juce::AccessibilityRole::staticText,
                                      juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press,
                                                                              [&fieldIn] { fieldIn.showEditor(); }),
                                      { std::make_unique<ValueInterface> (fieldIn) }),
          field (fieldIn)
    {
    }

    juce::String getHelp() const override { return field.getTooltip(); }

    juce::AccessibleState getCurrentState() const override
    {
        if (field.isBeingEdited())
            return {}; // フォーカスは編集中のTextEditorへ渡す

        return juce::AccessibilityHandler::getCurrentState();
    }

private:
    class ValueInterface final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit ValueInterface (NumberField& fieldIn) : field (fieldIn) {}

        bool isReadOnly() const override { return ! field.isEnabled(); }
        juce::String getCurrentValueAsString() const override { return field.getText(); }
        void setValueAsString (const juce::String& newValue) override { field.applyText (newValue); }

    private:
        NumberField& field;
    };

    NumberField& field;
};

} // namespace

// ===== SECTION: NumberField =====

NumberField::NumberField (EqField fieldKind) : juce::Label ({}, {}), field (fieldKind)
{
    setEditable (true, false, false); // 単一クリック（ドラッグなし）で編集。フォーカスを失ったら確定。Tabでフォーカスしても編集が始まる
    setRepaintsOnMouseActivity (true);
    setJustificationType (juce::Justification::centredRight);
    onTextChange = [this] { applyText (getText()); };
}

void NumberField::setValue (float newValue)
{
    value = newValue;
    refreshText();
}

void NumberField::setUnused (bool shouldBeUnused)
{
    if (unused == shouldBeUnused)
        return;

    unused = shouldBeUnused;
    hideEditor (true);
    setEnabled (! unused);
    invalidateAccessibilityHandler();
    repaint();
}

void NumberField::setValueColour (juce::Colour c)
{
    valueColour = c;
    repaint();
}

void NumberField::refreshText()
{
    setText (formatEqValue (field, value), juce::dontSendNotification);
}

void NumberField::applyValue (float newValue)
{
    const bool changed = ! juce::approximatelyEqual (newValue, value);
    value = newValue;
    refreshText();

    if (changed && onUserChange != nullptr)
        onUserChange (newValue);
}

void NumberField::applyText (const juce::String& text)
{
    // 数値として読めない入力は確定せず、元の値に戻す（エラー表示は出さない）。
    if (const auto parsed = parseEqInput (field, text))
        applyValue (*parsed);
    else
        refreshText();
}

void NumberField::step (int direction)
{
    applyValue (stepEqValue (field, value, direction));
}

void NumberField::commitEdit()
{
    if (isBeingEdited())
        hideEditor (false);
}

void NumberField::requestStepFromEditor (int direction)
{
    // 編集用のTextEditorは、そのキー処理の中では削除できない（Label自身のReturn・Escも非同期）。
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<NumberField> (this), direction]
    {
        if (safe == nullptr)
            return;

        safe->commitEdit();
        safe->step (direction);
        safe->grabKeyboardFocus(); // 編集は閉じて、欄にフォーカスを残す
    });
}

void NumberField::requestTabFromEditor (bool forward)
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<NumberField> (this), forward]
    {
        if (safe == nullptr)
            return;

        safe->commitEdit();
        safe->grabKeyboardFocus();
        safe->moveKeyboardFocusToSibling (forward); // 次の欄（Tabでフォーカスされた欄は編集を始める）
    });
}

juce::TextEditor* NumberField::createEditorComponent()
{
    auto* ed = new FieldEditor (*this);

    ed->applyFontToAllText (L::uiFont (14.0f));
    ed->setJustification (juce::Justification::centredRight);
    ed->setBorder (juce::BorderSize<int>());
    ed->setIndents (0, 0);

    // 編集中: 文字と縁はtext.primary、選択範囲の背景はline.borderHover。背景と枠は欄（paint）が描く。
    ed->setColour (juce::TextEditor::textColourId, juce::Colour (L::textPrimary));
    ed->setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    ed->setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    ed->setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    ed->setColour (juce::TextEditor::highlightColourId, juce::Colour (L::lineBorderHover));
    ed->setColour (juce::TextEditor::highlightedTextColourId, juce::Colour (L::textPrimary));
    ed->setColour (juce::CaretComponent::caretColourId, juce::Colour (L::textPrimary));

    return ed;
}

std::unique_ptr<juce::AccessibilityHandler> NumberField::createAccessibilityHandler()
{
    return std::make_unique<NumberFieldAccessibility> (*this);
}

void NumberField::resized()
{
    if (auto* ed = getCurrentTextEditor())
    {
        const int left = field == EqField::Q ? 4 : 8;
        ed->setBounds (left, 0, getWidth() - left - 8, getHeight());
    }
}

void NumberField::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    if (unused)
    {
        // 使わない欄（ローカット・ハイカットのゲイン）: 背景なし、1pxのline.divider、「—」をtext.disabled。
        g.setColour (juce::Colour (L::lineDivider));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (juce::Colour (L::textDisabled));
        g.setFont (L::uiFont (14.0f));
        g.drawText (juce::String::fromUTF8 ("\xE2\x80\x94"), getLocalBounds().withTrimmedRight (8), juce::Justification::centredRight, false);
        return;
    }

    g.setColour (juce::Colour (L::surface));
    g.fillRoundedRectangle (bounds, 4.0f);

    juce::Colour edge (L::lineBorder);
    float borderWidth = 1.0f;

    if (hasKeyboardFocus (true)) // フォーカス中・編集中は2pxのfocus
    {
        edge = juce::Colour (L::focus);
        borderWidth = 2.0f;
    }
    else if (isMouseOver (true))
    {
        edge = juce::Colour (L::lineBorderHover);
    }

    g.setColour (edge);
    g.drawRoundedRectangle (bounds.reduced (borderWidth * 0.5f), 4.0f, borderWidth);

    if (! isBeingEdited())
    {
        // 値は14pt Regular、右寄せ。余白は右8、左8（Qだけ左4）。
        g.setColour (valueColour);
        g.setFont (L::uiFont (14.0f));
        g.drawFittedText (getText(), getLocalBounds().withTrimmedLeft (field == EqField::Q ? 4 : 8).withTrimmedRight (8),
                          juce::Justification::centredRight, 1, 0.9f);
    }
}

bool NumberField::keyPressed (const juce::KeyPress& key)
{
    if (unused)
        return false;

    if (key == juce::KeyPress::upKey)     { step (1);  return true; }
    if (key == juce::KeyPress::downKey)   { step (-1); return true; }
    if (key == juce::KeyPress::returnKey) { showEditor(); return true; } // Enterで確定した後、もう一度Enterで編集を始められる

    return false;
}

void NumberField::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // ホイールは編集中でないときだけ効く。上で増える（丸めた結果が変わらなければ最小刻み）。
    if (isBeingEdited() || unused)
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    const float dy = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;

    if (dy > 0.0f)
        step (1);
    else if (dy < 0.0f)
        step (-1);
}

// ===== SECTION: BandCheck =====

BandCheck::BandCheck() : juce::Button ("bandCheck")
{
    setClickingTogglesState (true);
    setWantsKeyboardFocus (true);
}

void BandCheck::setEqOn (bool on)
{
    if (eqOn != on)
    {
        eqOn = on;
        repaint();
    }
}

void BandCheck::paintButton (juce::Graphics& g, bool isHighlighted, bool)
{
    // 描画する箱は16×16、角丸3、部品内(4, 8)。
    const juce::Rectangle<float> box (4.0f, 8.0f, 16.0f, 16.0f);

    if (getToggleState())
    {
        g.setColour (juce::Colour (eqOn ? L::live : L::idleFill));
        g.fillRoundedRectangle (box, 3.0f);

        juce::Path check;
        check.startNewSubPath (box.getX() + 4.0f, box.getY() + 8.5f);
        check.lineTo (box.getX() + 7.0f, box.getY() + 11.5f);
        check.lineTo (box.getX() + 12.0f, box.getY() + 5.0f);

        g.setColour (juce::Colour (eqOn ? L::liveInk : L::bgWindow));
        g.strokePath (check, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        // 未選択: 塗りなし、1.5pxのidle.fill（OFFのランプと同じ中抜きの表現）。ホバー時は縁をtext.secondary。
        g.setColour (juce::Colour (isHighlighted ? L::textSecondary : L::idleFill));
        g.drawRoundedRectangle (box.reduced (0.75f), 3.0f, 1.5f);
    }

    if (hasKeyboardFocus (true))
    {
        // 箱を各辺3px広げた22×22の外形、角丸5、2pxのfocus。
        g.setColour (juce::Colour (L::focus));
        g.drawRoundedRectangle (box.expanded (3.0f).reduced (1.0f), 4.0f, 2.0f);
    }
}

// ===== SECTION: EqGraph =====

EqGraph::EqGraph (AudioIO& audioIOIn) : audioIO (audioIOIn)
{
    setInterceptsMouseClicks (false, false); // 操作なし。カーソルも変えない（つかめる点に見せない）
}

float EqGraph::hzToX (double hz) noexcept
{
    return kPlotX + kPlotW * (float) (std::log10 (hz / 20.0) / 3.0); // 20 Hz〜20 kHzの対数
}

float EqGraph::dbToY (double db) noexcept
{
    return 56.0f - (float) db * kPlotH / 36.0f; // +18〜-18 dB
}

std::unique_ptr<juce::AccessibilityHandler> EqGraph::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::image);
}

void EqGraph::paint (juce::Graphics& g)
{
    const auto& params = audioIO.engineParams();
    const bool eqOn = params.eqEnabled.load (std::memory_order_relaxed);
    const double rate = audioIO.getOutputInfo().rate;
    const double fs = rate > 0.0 ? rate : 48000.0; // デバイス停止中は48000 Hz

    std::array<EqBandSettings, kEqBands> bands;

    for (size_t i = 0; i < bands.size(); ++i)
        bands[i] = params.eqBands[i].load (kEqDefaults[i]);

    g.setColour (juce::Colour (L::bgInset));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

    // ----- 格子（描画域の中の1pxの線） -----
    const int plotX = (int) kPlotX, plotY = (int) kPlotY, plotW = (int) kPlotW, plotH = (int) kPlotH;

    auto verticalLine = [&] (double hz, juce::uint32 colour)
    {
        g.setColour (juce::Colour (colour));
        g.fillRect ((int) std::floor (hzToX (hz)), plotY, 1, plotH);
    };

    auto horizontalLine = [&] (double db, juce::uint32 colour)
    {
        g.setColour (juce::Colour (colour));
        g.fillRect (plotX, (int) std::lround (dbToY (db)), plotW, 1);
    };

    for (const double hz : { 50.0, 200.0, 500.0, 2000.0, 5000.0 })
        verticalLine (hz, L::lineDivider);

    for (const double hz : { 100.0, 1000.0, 10000.0 })
        verticalLine (hz, L::lineBorderHover);

    for (const double db : { -12.0, -6.0, 6.0, 12.0 })
        horizontalLine (db, L::lineDivider);

    horizontalLine (0.0, L::lineBorderHover);

    // ----- 軸の文字（12pt text.secondary） -----
    g.setColour (juce::Colour (L::textSecondary));
    g.setFont (L::uiFont (12.0f));

    const std::pair<double, const char*> dbLabels[] = { { 12.0, "+12" }, { 0.0, "0" }, { -12.0, "-12" } };

    for (const auto& [db, text] : dbLabels)
        g.drawText (text, 2, (int) std::lround (dbToY (db)) - 8, 26, 16, juce::Justification::centredRight, false);

    const std::pair<double, const char*> hzLabels[] = { { 100.0, "100" }, { 1000.0, "1k" }, { 10000.0, "10k" } };

    for (const auto& [hz, text] : hzLabels)
        g.drawText (text, (int) std::lround (hzToX (hz)) - 16, 106, 32, 14, juce::Justification::centred, false);

    // ----- 曲線: 有効なバンドの振幅[dB]の和を横1pxごとに計算し、2pxの線で描く。EQがOFFでも設定中の特性を見せる -----
    const juce::Colour curveColour (eqOn ? L::live : L::idleFill);

    juce::Path curve;

    for (int px = 0; px <= plotW; ++px)
    {
        const double hz = 20.0 * std::pow (10.0, 3.0 * (double) px / (double) plotW);
        const float x = kPlotX + (float) px;
        const float y = dbToY (eqCurveDb (bands, fs, hz));

        if (px == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plotX, plotY, plotW, plotH);
        g.setColour (curveColour);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // ----- バンドの印: 有効なバンドの周波数での曲線上に、直径8の円と番号 -----
    for (size_t i = 0; i < bands.size(); ++i)
    {
        if (! bands[i].on)
            continue;

        const double hz = juce::jlimit ((double) kEqMinHz, (double) kEqMaxHz, std::min ((double) bands[i].hz, 0.45 * fs));
        const float cx = hzToX (hz);
        const float cy = juce::jlimit (kPlotY, kPlotY + kPlotH, dbToY (eqCurveDb (bands, fs, hz)));

        g.setColour (juce::Colour (L::bgInset));
        g.fillEllipse (cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        g.setColour (curveColour);
        g.drawEllipse (cx - 4.0f + 0.75f, cy - 4.0f + 0.75f, 8.0f - 1.5f, 8.0f - 1.5f, 1.5f);

        // 番号は円の上2px（12pt、幅12で中央寄せ）。円が描画域の上端から16px以内なら円の下に置く。
        const bool nearTop = (cy - 4.0f) - kPlotY < 16.0f;
        const float labelY = nearTop ? cy + 4.0f + 2.0f : cy - 4.0f - 2.0f - 14.0f;

        g.setColour (juce::Colour (L::textSecondary));
        g.setFont (L::uiFont (12.0f));
        g.drawText (juce::String ((int) i + 1), juce::Rectangle<float> (cx - 6.0f, labelY, 12.0f, 14.0f),
                    juce::Justification::centred, false);
    }
}

// ===== SECTION: MicPanel =====

namespace
{

constexpr int kBandRowY0 = 356, kBandRowPitch = 36;

} // namespace

MicPanel::MicPanel (AudioIO& audioIOIn, juce::PropertiesFile& settingsIn)
    : audioIO (audioIOIn), settings (settingsIn), graph (audioIOIn)
{
    setLookAndFeel (&lookAndFeel);
    tooltipWindow.setLookAndFeel (&lookAndFeel);

    setTitle (utf8 ("マイク処理"));

    // ----- ノイズ除去 -----
    nrSwitch.setTexts (utf8 ("ノイズ除去"), utf8 ("背景の雑音を抑えています"), utf8 ("雑音を抑えていません"));
    nrSwitch.setTitle (utf8 ("ノイズ除去のON/OFF"));
    nrSwitch.setTooltip (utf8 (kNrSwitchTip));
    nrSwitch.setDescription (utf8 (kNrSwitchTip));
    nrSwitch.setExplicitFocusOrder (1);
    nrSwitch.onClick = [this] { setNrEnabled (nrSwitch.getToggleState()); };
    addAndMakeVisible (nrSwitch);

    auto setupLabel = [this] (juce::Label& l, const char* text)
    {
        l.setText (utf8 (text), juce::dontSendNotification);
        l.setFont (L::uiFont (13.0f));
        l.setColour (juce::Label::textColourId, juce::Colour (L::textSecondary));
        l.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
    };

    setupLabel (backgroundLabel, "背景ノイズ");
    setupLabel (impactLabel, "インパクト");

    auto setupSlider = [this] (juce::Slider& s, const char* title, const char* tip, double initial, int focusOrder,
                               std::atomic<float>& target)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, true, 64, 32);
        s.setRange (0.0, 100.0, 1.0);
        s.setDoubleClickReturnValue (true, initial);
        s.setSliderSnapsToMousePosition (false);
        s.textFromValueFunction = [] (double v) { return juce::String ((int) std::lround (v)) + " %"; };
        s.setWantsKeyboardFocus (true);
        s.setTitle (utf8 (title));
        s.setTooltip (utf8 (tip));
        s.setDescription (utf8 (tip));
        s.setExplicitFocusOrder (focusOrder);
        s.setValue (std::round ((double) target.load (std::memory_order_relaxed) * 100.0), juce::dontSendNotification);
        s.updateText();
        s.onValueChange = [this, &s, &target] { applySlider (s, target); };
        addAndMakeVisible (s);
    };

    auto& params = audioIO.engineParams();
    setupSlider (backgroundSlider, "背景ノイズの抑制量", kBackgroundTip, (double) kNrBackgroundDefault * 100.0, 2, params.nrBackground);
    setupSlider (impactSlider, "インパクトノイズの抑制量", kImpactTip, (double) kNrImpactDefault * 100.0, 3, params.nrImpact);

    // ----- EQ -----
    eqSwitch.setTexts (utf8 ("EQ"), utf8 ("音質を調整しています"), utf8 ("音質は変えていません"));
    eqSwitch.setTitle (utf8 ("EQのON/OFF"));
    eqSwitch.setTooltip (utf8 (kEqSwitchTip));
    eqSwitch.setDescription (utf8 (kEqSwitchTip));
    eqSwitch.setExplicitFocusOrder (4);
    eqSwitch.onClick = [this] { setEqEnabled (eqSwitch.getToggleState()); };
    addAndMakeVisible (eqSwitch);

    graph.setTitle (utf8 ("EQの周波数特性"));
    graph.setDescription (utf8 ("グラフです。数値は各バンドの欄で確認できます"));
    addAndMakeVisible (graph);

    // Tab順: バンド1（有効 → タイプ → 周波数 → ゲイン → Q）→ … → バンド5 → 初期値に戻すボタン（5〜29、30）。
    for (int i = 0; i < kEqBands; ++i)
    {
        auto& b = bands[(size_t) i];
        const juce::String bandName = utf8 ("バンド") + juce::String (i + 1) + " ";
        const int order = 5 + i * 5;

        b.check.setTitle (bandName + utf8 ("有効"));
        b.check.setTooltip (utf8 ("このバンドを使う"));
        b.check.setExplicitFocusOrder (order);
        b.check.onClick = [this, i]
        {
            auto s = readBand (i);
            s.on = bands[(size_t) i].check.getToggleState();
            bandChangedByUser (i, s);
        };
        addAndMakeVisible (b.check);

        // 一覧の順はEqTypeの並びと同じ（項目のidはEqTypeのint値 + 1）。
        b.type.addItem (utf8 ("ピーキング"), (int) EqType::Peak + 1);
        b.type.addItem (utf8 ("ローシェルフ"), (int) EqType::LowShelf + 1);
        b.type.addItem (utf8 ("ハイシェルフ"), (int) EqType::HighShelf + 1);
        b.type.addItem (utf8 ("ローカット"), (int) EqType::LowCut + 1);
        b.type.addItem (utf8 ("ハイカット"), (int) EqType::HighCut + 1);
        b.type.setTitle (bandName + utf8 ("タイプ"));
        b.type.setTooltip (utf8 (kTypeTip));
        b.type.setDescription (utf8 (kTypeTip));
        b.type.setExplicitFocusOrder (order + 1);
        b.type.onChange = [this, i]
        {
            auto s = readBand (i);
            s.type = eqTypeFromInt (bands[(size_t) i].type.getSelectedId() - 1, s.type);
            bandChangedByUser (i, s);
        };
        addAndMakeVisible (b.type);

        auto makeField = [&] (EqField kind, const char* what, const char* tip, int focusOrder,
                              std::function<void (EqBandSettings&, float)> assign)
        {
            auto field = std::make_unique<NumberField> (kind);
            field->setTitle (bandName + utf8 (what));
            field->setTooltip (utf8 (tip));
            field->setDescription (utf8 (tip));
            field->setExplicitFocusOrder (focusOrder);
            field->onUserChange = [this, i, assign] (float v)
            {
                auto s = readBand (i);
                assign (s, v);
                bandChangedByUser (i, s);
            };
            addAndMakeVisible (*field);
            return field;
        };

        b.hz = makeField (EqField::Hz, "周波数", kHzTip, order + 2, [] (EqBandSettings& s, float v) { s.hz = v; });
        b.gain = makeField (EqField::GainDb, "ゲイン", kGainTip, order + 3, [] (EqBandSettings& s, float v) { s.gainDb = v; });
        b.q = makeField (EqField::Q, "Q", kQTip, order + 4, [] (EqBandSettings& s, float v) { s.q = v; });
    }

    resetButton.setExplicitFocusOrder (30);
    resetButton.onClick = [this] { resetOrUndoEq(); };
    addAndMakeVisible (resetButton);

    refreshAll();
    setSize (460, 600); // resized()が全部品を使うため、最後に設定する
}

MicPanel::~MicPanel()
{
    tooltipWindow.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

EqBandSettings MicPanel::readBand (int index) const noexcept
{
    return audioIO.engineParams().eqBands[(size_t) index].load (kEqDefaults[(size_t) index]);
}

void MicPanel::writeBand (int index, const EqBandSettings& s)
{
    audioIO.engineParams().eqBands[(size_t) index].store (s);
}

void MicPanel::saveSettings()
{
    // 現在のatomicの値から、マイク処理の全項目を書く（値が変わらないキーはPropertiesFileが変更扱いにしない）。1秒後にまとめて保存される。
    const auto& p = audioIO.engineParams();

    SavedSettings s;
    s.nrEnabled = p.nrEnabled.load (std::memory_order_relaxed);
    s.nrBackground = p.nrBackground.load (std::memory_order_relaxed);
    s.nrImpact = p.nrImpact.load (std::memory_order_relaxed);
    s.eqEnabled = p.eqEnabled.load (std::memory_order_relaxed);

    for (int i = 0; i < kEqBands; ++i)
        s.eqBands[(size_t) i] = readBand (i);

    storeMicSettings (settings, s);
}

void MicPanel::setNrEnabled (bool on)
{
    audioIO.engineParams().nrEnabled.store (on, std::memory_order_relaxed);
    saveSettings();
    refreshNrAppearance();
}

void MicPanel::setEqEnabled (bool on)
{
    audioIO.engineParams().eqEnabled.store (on, std::memory_order_relaxed);
    saveSettings();
    refreshEqAppearance();
}

void MicPanel::applySlider (juce::Slider& slider, std::atomic<float>& target)
{
    target.store ((float) (slider.getValue() * 0.01), std::memory_order_relaxed);
    saveSettings();
    refreshNrAppearance();
}

void MicPanel::bandChangedByUser (int index, const EqBandSettings& s)
{
    writeBand (index, s);
    clearUndo(); // EQのバンドの値を変えたら、「元に戻す」は使えなくなる
    saveSettings();
    refreshBandAppearance (index);
    graph.repaint();
}

void MicPanel::resetOrUndoEq()
{
    if (undoAvailable)
    {
        for (int i = 0; i < kEqBands; ++i)
            writeBand (i, undoBands[(size_t) i]);

        undoAvailable = false;
        juce::AccessibilityHandler::postAnnouncement (utf8 ("EQを元の値に戻しました"),
                                                      juce::AccessibilityHandler::AnnouncementPriority::medium);
    }
    else
    {
        // ノイズ除去とEQのON/OFFは変えない。5バンドの有効/無効・タイプ・周波数・ゲイン・Qを初期値（kEqDefaults）へ。
        for (int i = 0; i < kEqBands; ++i)
        {
            undoBands[(size_t) i] = readBand (i);
            writeBand (i, kEqDefaults[(size_t) i]);
        }

        undoAvailable = true;
        juce::AccessibilityHandler::postAnnouncement (utf8 ("EQを初期値に戻しました。「元に戻す」で取り消せます"),
                                                      juce::AccessibilityHandler::AnnouncementPriority::medium);
    }

    saveSettings();
    refreshEqAppearance();
    refreshResetButton();
}

void MicPanel::clearUndo()
{
    if (undoAvailable)
    {
        undoAvailable = false;
        refreshResetButton();
    }
}

void MicPanel::refreshResetButton()
{
    const juce::String tip = utf8 (undoAvailable ? kUndoTip : kResetTip);

    resetButton.setButtonText (utf8 (undoAvailable ? "元に戻す" : "EQを初期値に戻す"));
    resetButton.setTooltip (tip);
    resetButton.setDescription (tip);
}

void MicPanel::refreshNrAppearance()
{
    const auto& p = audioIO.engineParams();
    const bool nrOn = p.nrEnabled.load (std::memory_order_relaxed);

    nrSwitch.setToggleState (nrOn, juce::dontSendNotification);

    // スライダーの塗りと値の色は、全体バイパスではなくノイズ除去のON/OFFで決める（10.3節）。
    for (auto* slider : { &backgroundSlider, &impactSlider })
    {
        const bool atZero = slider->getValue() < 0.5;

        slider->getProperties().set ("liveState", nrOn);
        slider->setColour (juce::Slider::textBoxTextColourId,
                           juce::Colour (atZero ? L::textPrimary : (nrOn ? L::live : L::textSecondary)));
        slider->repaint();
    }
}

void MicPanel::refreshEqAppearance()
{
    eqSwitch.setToggleState (audioIO.engineParams().eqEnabled.load (std::memory_order_relaxed), juce::dontSendNotification);

    for (int i = 0; i < kEqBands; ++i)
        refreshBandAppearance (i);

    graph.repaint();
}

void MicPanel::refreshBandAppearance (int index)
{
    auto& w = bands[(size_t) index];
    const auto s = readBand (index);
    const bool eqOn = audioIO.engineParams().eqEnabled.load (std::memory_order_relaxed);

    w.check.setToggleState (s.on, juce::dontSendNotification);
    w.check.setEqOn (eqOn);

    w.type.setSelectedId ((int) s.type + 1, juce::dontSendNotification);

    // 値の文字色: 無効なバンドはすべてtext.secondary（欄は操作できるため、text.disabledは使わない）。有効なバンドは、
    // 周波数とQがtext.primary、ゲインは0.0ならtext.primary、0以外でEQがONならlive、EQがOFFならtext.secondary。
    const juce::Colour primary (s.on ? L::textPrimary : L::textSecondary);
    const bool gainIsZero = std::abs (s.gainDb) < 0.05f;
    const juce::Colour gainColour (! s.on ? L::textSecondary
                                          : (gainIsZero ? L::textPrimary : (eqOn ? L::live : L::textSecondary)));

    w.type.setColour (juce::ComboBox::textColourId, juce::Colour (s.on ? L::textPrimary : L::textSecondary));
    w.type.repaint();

    w.hz->setValue (s.hz);
    w.hz->setValueColour (primary);
    w.q->setValue (s.q);
    w.q->setValueColour (primary);
    w.gain->setValue (s.gainDb);
    w.gain->setValueColour (gainColour);

    const bool gainUnused = s.type == EqType::LowCut || s.type == EqType::HighCut;
    w.gain->setUnused (gainUnused);

    const juce::String gainTip = utf8 (gainUnused ? kGainUnusedTip : kGainTip);
    w.gain->setTooltip (gainTip);
    w.gain->setDescription (gainTip);
}

void MicPanel::refreshAll()
{
    refreshNrAppearance();
    refreshEqAppearance();
    refreshResetButton();
}

void MicPanel::updateShowingState()
{
    const bool showing = isShowing();

    if (wasShowing && ! showing)
    {
        // 非表示にしたとき: 編集中の入力を確定し、「元に戻す」は使えなくする。
        for (auto& b : bands)
        {
            b.hz->commitEdit();
            b.gain->commitEdit();
            b.q->commitEdit();
        }

        clearUndo();
    }

    wasShowing = showing;
}

void MicPanel::visibilityChanged()
{
    updateShowingState();
}

void MicPanel::parentHierarchyChanged()
{
    // 親のウィンドウの表示・非表示はparentHierarchyChangedで届く（MainComponentと同じ理由）。
    updateShowingState();
}

void MicPanel::focusInitial()
{
    backgroundSlider.grabKeyboardFocus();
}

void MicPanel::resized()
{
    nrSwitch.setBounds (20, 12, 420, 40);
    backgroundLabel.setBounds (20, 60, 84, 32);
    backgroundSlider.setBounds (108, 60, 332, 32);
    impactLabel.setBounds (20, 96, 84, 32);
    impactSlider.setBounds (108, 96, 332, 32);

    eqSwitch.setBounds (20, 152, 420, 40);
    graph.setBounds (20, 200, 420, 124);

    for (int i = 0; i < kEqBands; ++i)
    {
        auto& b = bands[(size_t) i];
        const int y = kBandRowY0 + kBandRowPitch * i;

        b.check.setBounds (40, y, 24, 32);
        b.type.setBounds (72, y + 2, 132, 28);
        b.hz->setBounds (212, y + 2, 84, 28);
        b.gain->setBounds (304, y + 2, 76, 28);
        b.q->setBounds (388, y + 2, 52, 28);
    }

    resetButton.setBounds (300, 556, 140, 32);
}

void MicPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (L::bgWindow));

    g.setColour (juce::Colour (L::lineDivider));
    g.fillRect (20, 140, 420, 1);
    g.fillRect (20, 544, 420, 1);

    // 列見出し（12pt text.secondary。y332・高さ20）。
    g.setColour (juce::Colour (L::textSecondary));
    g.setFont (L::uiFont (12.0f));
    g.drawText (utf8 ("有効"), 36, 332, 32, 20, juce::Justification::centred, false);
    g.drawText (utf8 ("タイプ"), 78, 332, 120, 20, juce::Justification::centredLeft, false);
    g.drawText (utf8 ("周波数"), 212, 332, 76, 20, juce::Justification::centredRight, false);
    g.drawText (utf8 ("ゲイン"), 304, 332, 68, 20, juce::Justification::centredRight, false);
    g.drawText ("Q", 388, 332, 44, 20, juce::Justification::centredRight, false);

    // バンドの番号（13pt text.secondary、部品x20・幅16、左寄せ・上下中央）。
    g.setFont (L::uiFont (13.0f));

    for (int i = 0; i < kEqBands; ++i)
        g.drawText (juce::String (i + 1), 20, kBandRowY0 + kBandRowPitch * i, 16, 32, juce::Justification::centredLeft, false);

    // 操作のヒント（12pt text.secondary。操作できない静的な文字）。
    g.setFont (L::uiFont (12.0f));
    g.drawText (utf8 ("欄をクリックで入力、ホイールで微調整"), 20, 556, 268, 32, juce::Justification::centredLeft, false);
}

// ===== SECTION: MicWindow =====

MicWindow::MicWindow (AudioIO& audioIOIn, juce::PropertiesFile& settingsIn)
    : DocumentWindow (juce::String::fromUTF8 ("マイク処理 — VoiceChange"), juce::Colour (AppLookAndFeel::bgWindow),
                      DocumentWindow::closeButton)
{
    setUsingNativeTitleBar (true);
    setResizable (false, false);

    panel = new MicPanel (audioIOIn, settingsIn);
    setContentOwned (panel, true);
}

bool MicWindow::keyPressed (const juce::KeyPress& key)
{
    // Esc: ウィンドウを非表示にする。数値欄の編集中は編集の取り消しだけ、コンボボックスのポップアップ表示中は
    // ポップアップを閉じるだけ（どちらも先にキーを消費するため、ここへは届かない）。
    if (key == juce::KeyPress::escapeKey)
    {
        closeButtonPressed();
        return true;
    }

    return false;
}

void MicWindow::showBeside (const juce::Rectangle<int>& mainScreenBounds)
{
    if (! positioned)
    {
        positioned = true;

        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* display = displays.getDisplayForRect (mainScreenBounds);
        const auto area = (display != nullptr ? display : displays.getPrimaryDisplay())->userBounds.getLargestIntegerWithin();

        juce::Rectangle<int> r (mainScreenBounds.getRight() + 8, mainScreenBounds.getY(), getWidth(), getHeight());

        if (! area.contains (r))
            r = juce::Rectangle<int> (getWidth(), getHeight()).withCentre (mainScreenBounds.getCentre());

        setBounds (r);
    }

    setVisible (true);
    toFront (true);
    panel->focusInitial();
}

} // namespace vc
