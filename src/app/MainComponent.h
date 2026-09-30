#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_data_structures/juce_data_structures.h>

#include "AudioIO.h"
#include "core/Params.h"

#include <array>
#include <atomic>
#include <functional>

// ===== SECTION: MainComponent =====
// docs/design.md をUIの正本とする。座標・色・フォントサイズは同書3章の値を使う。
// 色トークンの正本はdesign.md 3.1節。コード側の定数はAppLookAndFeel内に置く（design.md 9章の確定事項）。

namespace vc
{

// ===== SECTION: AppLookAndFeel =====
class AppLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    AppLookAndFeel();

    // design.md 3.1節「デザイントークン」。正本はdesign.md、ここはコード側の定数。
    static constexpr juce::uint32 bgWindow         = 0xFF17181A;
    static constexpr juce::uint32 bgInset          = 0xFF111214;
    static constexpr juce::uint32 surface          = 0xFF222428;
    static constexpr juce::uint32 surfaceHover     = 0xFF2B2E32;
    static constexpr juce::uint32 surfacePressed   = 0xFF1B1D20;
    static constexpr juce::uint32 lineDivider      = 0xFF2A2C30;
    static constexpr juce::uint32 lineBorder       = 0xFF3A3D42;
    static constexpr juce::uint32 lineBorderHover  = 0xFF55595F;
    static constexpr juce::uint32 track            = 0xFF34373C;
    static constexpr juce::uint32 textPrimary      = 0xFFECEDEE;
    static constexpr juce::uint32 textSecondary    = 0xFFA3A7AD;
    static constexpr juce::uint32 textDisabled     = 0xFF6B6F76;
    static constexpr juce::uint32 live             = 0xFF3DDC97;
    static constexpr juce::uint32 liveInk          = 0xFF0B1F16;
    static constexpr juce::uint32 liveTint         = 0xFF15291F;
    static constexpr juce::uint32 liveTintHover    = 0xFF1A3126;
    static constexpr juce::uint32 liveTintPressed  = 0xFF112019;
    static constexpr juce::uint32 idleFill         = 0xFF858A92;
    static constexpr juce::uint32 meterLit         = 0xFFC9CDD2;
    static constexpr juce::uint32 meterOff         = 0xFF26282C;
    static constexpr juce::uint32 warn             = 0xFFF5B83D;
    static constexpr juce::uint32 error            = 0xFFFF5A5F;
    static constexpr juce::uint32 focus            = 0xFFF4F4F5;
    static constexpr juce::uint32 tooltipBg        = 0xFF2B2E32;
    static constexpr juce::uint32 tooltipBorder    = 0xFF45484E;
    static constexpr juce::uint32 popupHighlight   = 0xFF33363B;

    // フォント名の探索(design.md 3.1節)。起動時に一度だけ調べ、以後キャッシュする。
    static juce::Font uiFont (float pointHeight, bool bold = false);
    static juce::Font numericFont (float pointHeight);

    // プリセットボタン・スライダーの塗り分けに使う「バイパス中か」の参照。
    // 音声スレッドとはatomicで共有している値をそのまま読む(UI描画は読むだけ)。
    void setEnabledFlag (const std::atomic<bool>* flag) noexcept { chainEnabledFlag = flag; }
    bool isChainEnabled() const noexcept
    {
        return chainEnabledFlag == nullptr || chainEnabledFlag->load (std::memory_order_relaxed);
    }

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                        int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;

    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                             bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                             const juce::String& shortcutKeyText, const juce::Drawable* icon,
                             const juce::Colour* textColour) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                bool isHighlighted, bool isDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isHighlighted, bool isDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPos, float minSliderPos, float maxSliderPos,
                            juce::Slider::SliderStyle, juce::Slider&) override;

private:
    const std::atomic<bool>* chainEnabledFlag = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppLookAndFeel)
};

// ===== SECTION: DeviceComboBox =====
// design.md 2章「デバイスのコンボボックスは上下矢印キーで選択を直接切り替えない」対応。
// JUCEの既定のComboBox::keyPressed()は上下左右矢印キーでnudgeSelectedItem()を呼び、
// 選択を直接変更してonChangeを発火させる（配信中に矢印キーで誤ってデバイスが切り替わる。
// レビュー指摘1）。矢印キーだけを消費してポップアップを開くのみにし、Space/Enter/Alt+↓は
// 既定の挙動（showPopupIfNotActive()相当）に委ねる。
class DeviceComboBox final : public juce::ComboBox
{
public:
    using juce::ComboBox::ComboBox;

    bool keyPressed (const juce::KeyPress& key) override
    {
        using juce::KeyPress;

        if (key == KeyPress::upKey || key == KeyPress::downKey
            || key == KeyPress::leftKey || key == KeyPress::rightKey)
        {
            if (! isPopupActive()) // showPopupIfNotActive()相当（同メソッドはprivateのため公開APIで代替）
                showPopup();
            return true;
        }

        return juce::ComboBox::keyPressed (key);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceComboBox)
};

// ===== SECTION: LevelMeter =====
// design.md 3.5節「入力レベルメーター（自前描画）」。バーの上昇は即時、下降は20dB/秒。
// ピークホールド値(数値表示用)は1.0秒保持した後20dB/秒で落とす。
class LevelMeter final : public juce::Component
{
public:
    LevelMeter();

    // 30fpsタイマーから毎回呼ぶ。linearPeakは前回呼び出し以降の入力ピーク(線形、takeInputPeak())。
    void advance (float linearPeak, double dtSeconds) noexcept;
    void setDisconnected (bool isDisconnected) noexcept;

    bool isDisconnected() const noexcept { return disconnected; }
    float getPeakHoldDb() const noexcept { return peakHoldDb; }

    void paint (juce::Graphics&) override;

private:
    static constexpr int kNumSegments = 40;
    static constexpr float kMinDb = -60.0f;
    static constexpr float kSegmentDb = 1.5f;
    static constexpr float kFallDbPerSec = 20.0f;

    float envelopeDb = kMinDb;
    float peakHoldDb = kMinDb;
    double peakHoldRemainingSec = 0.0;
    bool disconnected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

// ===== SECTION: ToggleSwitch =====
// design.md 3.5節「全体ON/OFFトグル」・10.3節「スイッチ」。ランプ付きの横長スイッチ、自前描画。
// メインの全体ON/OFF（高さ48）とマイク処理ウィンドウのスイッチ2つ（高さ40）で共用する。ランプは部品内(24, 高さ/2)、
// 主文は部品内x40・幅132に左寄せ、副文は部品内x176・幅228に右寄せ（重ならない）。
class ToggleSwitch final : public juce::Button
{
public:
    ToggleSwitch();

    // 主文の語（「エフェクト」「ノイズ除去」「EQ」。ONのとき「語 ON」、OFFのとき「語 OFF」）と、状態ごとの副文。
    void setTexts (const juce::String& wordIn, const juce::String& onSubText, const juce::String& offSubText);
    // OFFの副文だけを差し替える（メインのトグルはマイク処理の状態で出し分ける）。
    void setOffSubText (const juce::String& subText);
    const juce::String& getDisplayedSubText() const noexcept { return getToggleState() ? onSub : offSub; }

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    juce::String word, onSub, offSub;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToggleSwitch)
};

// ===== SECTION: MicButton =====
// design.md 3.5節「マイク処理ボタン」。入力行の右端。ランプはノイズ除去かEQのどちらかがONなら塗り、両方OFFなら中抜き。
class MicButton final : public juce::Button
{
public:
    MicButton();

    void setLampOn (bool on);

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    bool lampOn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MicButton)
};

// ===== SECTION: StatusPanel =====
// design.md 3.3節「状態パネル」・6章「UI状態の設計」。表示に必要な値をまとめて受け取り、
// 30fpsタイマーの8回に1回だけ更新する(design.md 4章)。
struct StatusData
{
    enum class IconKind { None, Warn, Error };

    double delayMs = 0.0;
    bool delayAsDash = false;   // 起動中・デバイス異常中は"--.-"/"—"を出す
    bool delayStarting = false; // 起動中は"--.-"
    bool delayWarn = false;     // シフター分を除く遅延がW2条件のとき数値もwarn色にする(6.2節)

    double cpuPercent = 0.0;

    double deviceMs = 0.0;
    double bufferMs = 0.0;
    double shifterMs = 0.0;
    bool shifterResting = true;
    double noiseMs = 0.0;
    bool noiseResting = true; // ノイズ除去の遅延が0（OFF中と、ONにした直後の準備中）は「除去 OFF」

    juce::String inputModeText;
    juce::String outputModeText;
    bool inputModeWarn = false;
    bool outputModeWarn = false;

    juce::Colour lineColour { AppLookAndFeel::lineDivider };

    juce::String message;
    IconKind icon = IconKind::None;
    bool messageBold = false;
    juce::Colour messageColour { AppLookAndFeel::textSecondary };
};

class StatusPanel final : public juce::Component
{
public:
    StatusPanel();

    void setData (const StatusData& newData);
    const juce::String& getMessage() const noexcept { return data.message; }

    void paint (juce::Graphics&) override;

private:
    StatusData data;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StatusPanel)
};

// ===== SECTION: MainComponent =====
class MainComponent final : public juce::Component,
                             private juce::Timer
{
public:
    // マイク処理ボタン（クリック・Enter・Space）が押されたときに呼ぶ。マイク処理ウィンドウはMain.cppが所有する。
    std::function<void()> onOpenMicPanel;

    // audioIO・settingsはMain.cpp（アプリ全体）が所有し、本コンポーネントより長生きする。
    // コンボボックスの選択表示は、起動時にopen()を試みたデバイス名（失敗していてもよい）を
    // AudioIO::getDesiredInputName()/getDesiredOutputName()から読む（design.md 3.5節）。
    MainComponent (AudioIO& audioIOIn, juce::PropertiesFile& settingsIn);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    void timerCallback() override;
    // isShowing()はaddAndMakeVisible時点(祖先がまだ非表示)ではfalseを返すため、
    // visibilityChanged()単独では初回表示を検出できない。祖先の表示状態が変わったときに
    // 呼ばれるparentHierarchyChanged()と合わせて同じ判定を行う。
    void updateTimerRunState();

    void refreshDeviceCombo (juce::ComboBox& combo, const juce::StringArray& names, const juce::String& desiredName,
                              juce::StringArray& realNamesOut);
    // デバイス一覧・選択中の名前が前回の構築から変わったときだけコンボボックスを作り直す
    // （design.md 3.5節「（未接続）」項目の追加・除去と新規デバイスの反映）。ポップアップ表示中は次回に回す。
    void syncDeviceCombos();
    void deviceComboChanged (bool isInputCombo);

    void createPresetButtons();
    void updatePresetButtonStates();

    void updateSliderAppearance (juce::Slider& slider);
    void applyGainFromSlider();
    void applyPitchFromSlider();
    void applyReverbFromSlider();

    void applyToggle();
    void refreshEnabledAppearance();
    // ノイズ除去・EQのON/OFF（AtomicParams。マイク処理ウィンドウが書く）に、ボタンのランプ・ツールチップ・説明、
    // 全体トグルの副文と説明を合わせる。状態パネルの更新周期（約270ms）ごとに呼ぶ。
    void refreshMicAppearance (bool nrOn, bool eqOn);

    void updateStatus (bool slowUpdate);
    // announceKeyは経過秒数など毎秒変わる部分を除いた文言。キーが変わったときだけ読み上げ通知する。
    void announceIfChanged (const juce::String& announceKey, bool isWarnOrError);
    // design.md 5章「状態パネル: 要約文を1秒に1回まで更新」。可視の8フレーム更新より遅い
    // 周期（30fps想定で30フレーム=約1秒）でsetDescription()を更新する（レビュー指摘4）。
    juce::String buildStatusSummary (const StatusData& d) const;

    AudioIO& audioIO;
    juce::PropertiesFile& settings;

    AppLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 500 };

    juce::Label inputLabel, outputLabel, levelLabel, gainLabel, pitchLabel, reverbLabel;
    DeviceComboBox inputCombo, outputCombo;
    MicButton micButton;

    LevelMeter levelMeter;
    juce::Label levelValueLabel;

    juce::Slider gainSlider, pitchSlider, reverbSlider;

    std::array<std::unique_ptr<juce::TextButton>, 8> presetButtons;

    ToggleSwitch toggleButton;

    StatusPanel statusPanel;

    bool micStateKnown = false, shownNrOn = false, shownEqOn = false; // refreshMicAppearanceの変化検出

    int frameCounter = 0;
    int accessibilityFrameCounter = 0; // 30fps想定で30回=約1秒ごとにstatusPanelのdescriptionを更新
    juce::String lastAnnouncedKey;
    StatusData lastStatusData; // buildStatusSummary()用に直近の状態を保持する

    juce::StringArray inputComboRealNames, outputComboRealNames;
    juce::String inputComboKey, outputComboKey; // 構築時の(一覧+選択名)。変化検出用。初期値の空は実際のキーと一致しない

    // W1(VB-CABLE未検出)は起動時にしか判定しない(design.md 6.2節)。
    bool vbCableMissing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace vc
