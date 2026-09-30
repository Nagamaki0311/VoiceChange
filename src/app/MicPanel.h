#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_data_structures/juce_data_structures.h>

#include "AudioIO.h"
#include "MainComponent.h"
#include "core/Params.h"

#include <array>
#include <functional>

// ===== SECTION: MicPanel =====
// マイク処理ウィンドウ（ノイズ除去とEQの設定）。docs/design.md 10章が正本。460×600、非モーダルのネイティブ別ウィンドウ。
// 変更は即座にAtomicParams（音声スレッドが読むatomic）へ反映し、設定ファイルへ保存する。書くのはメッセージスレッドだけ。
// 色・フォント・角丸はAppLookAndFeel（design.md 3.1節）を使い、新しい色は追加しない。

namespace vc
{

// ===== SECTION: NumberField =====
// 周波数・ゲイン・Qの数値欄（design.md 10.3節）。単一クリック（ドラッグなし）またはTabで編集を始めるjuce::Labelが土台。
// 入力の解釈・表示形式・刻みはParams.hの純粋関数（parseEqInput / formatEqValue / stepEqValue）。値の保存先は持たない。
class NumberField final : public juce::Label
{
public:
    explicit NumberField (EqField fieldKind);

    // 表示する値。ユーザー操作の通知（onUserChange）は呼ばない。
    void setValue (float newValue);
    float getValue() const noexcept { return value; }

    // ゲインを使わないタイプ（ローカット・ハイカット）では、操作もフォーカスもできない「—」欄にする。
    void setUnused (bool shouldBeUnused);
    void setValueColour (juce::Colour c);

    // 入力の確定・ホイール・矢印キーで値が変わったときに、丸めて範囲内へ収めた値で呼ぶ。
    std::function<void (float)> onUserChange;

    // 編集中なら入力中の文字列を確定する（ウィンドウを閉じるとき・Tabで次の欄へ移るとき）。
    void commitEdit();

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // 編集中のTextEditorから呼ぶ。↑↓は確定してから1回分動かす、Tabは確定して隣の欄へ移る（どちらもeditorを閉じるため非同期）。
    void requestStepFromEditor (int direction);
    void requestTabFromEditor (bool forward);

    // アクセシビリティ（Windows UIA）から値を書き込まれたとき。
    void applyText (const juce::String& text);

protected:
    juce::TextEditor* createEditorComponent() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void applyValue (float newValue);
    void refreshText();
    void step (int direction);

    const EqField field;
    float value = 0.0f;
    bool unused = false;
    juce::Colour valueColour { AppLookAndFeel::textPrimary };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NumberField)
};

// ===== SECTION: BandCheck =====
// バンドの有効チェック（design.md 10.3節）。部品は24×32、描画する箱は16×16。
class BandCheck final : public juce::Button
{
public:
    BandCheck();

    // EQがONならlive、OFFならidle.fillで塗る。
    void setEqOn (bool on);

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    bool eqOn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandCheck)
};

// ===== SECTION: EqGraph =====
// 周波数特性グラフ（自前描画、操作なし、フォーカス対象外。design.md 10.3節）。曲線はEqualizerと同じ係数計算の関数
// （eqCurveDb）で作り、表示と音がずれないようにする。値はAtomicParamsから描画のたびに読む（値が変わったときだけ再描画する）。
class EqGraph final : public juce::Component
{
public:
    explicit EqGraph (AudioIO& audioIOIn);

    void paint (juce::Graphics&) override;

    // グラフの描画域（部品内の座標。design.md 10.3節）と、周波数・dBとの対応。
    static constexpr float kPlotX = 32.0f, kPlotY = 8.0f, kPlotW = 380.0f, kPlotH = 96.0f;
    static float hzToX (double hz) noexcept;
    static float dbToY (double db) noexcept;

protected:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    AudioIO& audioIO;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqGraph)
};

// ===== SECTION: MicPanel（ウィンドウの内容） =====
class MicPanel final : public juce::Component
{
public:
    MicPanel (AudioIO& audioIOIn, juce::PropertiesFile& settingsIn);
    ~MicPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

    // 表示するたびに、背景ノイズのスライダーへフォーカスを当てる（スイッチに置くとSpace一発でノイズ除去が切れるため）。
    void focusInitial();

private:
    struct Band
    {
        BandCheck check;
        DeviceComboBox type;
        std::unique_ptr<NumberField> hz, gain, q;
    };

    EqBandSettings readBand (int index) const noexcept;
    void writeBand (int index, const EqBandSettings& settings);
    void saveSettings();

    void setNrEnabled (bool on);
    void setEqEnabled (bool on);
    void applySlider (juce::Slider& slider, std::atomic<float>& target);
    void bandChangedByUser (int index, const EqBandSettings& settings);
    void resetOrUndoEq();

    void refreshNrAppearance();
    void refreshEqAppearance();
    void refreshBandAppearance (int index);
    void refreshResetButton();
    void refreshAll();
    void updateShowingState();
    void clearUndo();

    AudioIO& audioIO;
    juce::PropertiesFile& settings;

    AppLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 500 };

    ToggleSwitch nrSwitch, eqSwitch;
    juce::Label backgroundLabel, impactLabel;
    juce::Slider backgroundSlider, impactSlider;
    EqGraph graph;
    std::array<Band, kEqBands> bands;
    juce::TextButton resetButton;

    // 「EQを初期値に戻す」の取り消し用。押した直後だけ有効（EQのバンドの値を変えた・ウィンドウを非表示にしたら無効）。
    bool undoAvailable = false;
    std::array<EqBandSettings, kEqBands> undoBands {};

    bool wasShowing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicPanel)
};

// ===== SECTION: MicWindow =====
// design.md 10.1節。ネイティブのタイトルバー（閉じるボタンのみ）、サイズ変更不可。閉じるボタンとEscで非表示にする（破棄しない）。
class MicWindow final : public juce::DocumentWindow
{
public:
    MicWindow (AudioIO& audioIOIn, juce::PropertiesFile& settingsIn);

    void closeButtonPressed() override { setVisible (false); }
    bool keyPressed (const juce::KeyPress& key) override;

    // 表示して前面に出し、背景ノイズのスライダーへフォーカスを当てる。初回だけ、メインウィンドウ（画面上の矩形）の右隣に置く
    // （左端 = メインの右端 + 8、上端をそろえる。作業領域に収まらなければメインの中央に重ねる）。以後は非表示にした位置のまま。
    void showBeside (const juce::Rectangle<int>& mainScreenBounds);

    MicPanel& getPanel() noexcept { return *panel; }

private:
    MicPanel* panel = nullptr; // setContentOwnedが所有する

    bool positioned = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicWindow)
};

} // namespace vc
