#include <juce_gui_extra/juce_gui_extra.h>

#include "AudioIO.h"

// ===== SECTION: MainWindow =====
// UI本体はT-006（MainComponent）で追加する。T-003時点では起動時に既定の入出力デバイスで
// AudioIOを開くだけ。Linux（Xvfb環境）ではオーディオデバイスが無いことがあるため、
// open()が失敗してもクラッシュしない（エラーテキストが残るだけ。表示はT-006/T-007）。

namespace
{
// design.md 3.1節「bg.window」。デザイントークンの正本はdocs/design.md、コード側の定数はここ（将来はLookAndFeelクラス）。
constexpr juce::uint32 kBackgroundColour = 0xFF17181A;
constexpr int kWindowWidth = 460;
constexpr int kWindowHeight = 600;
} // namespace

class VoiceChangeMainWindow final : public juce::DocumentWindow
{
public:
    VoiceChangeMainWindow()
        : DocumentWindow ("VoiceChange",
                           juce::Colour (kBackgroundColour),
                           DocumentWindow::minimiseButton | DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setResizable (false, false);

        auto* content = new juce::Component();
        content->setSize (kWindowWidth, kWindowHeight);
        setContentOwned (content, true);

        centreWithSize (kWindowWidth, kWindowHeight);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        // T-007でトレイへの格納に置き換える。現時点ではアプリを終了する。
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceChangeMainWindow)
};

class VoiceChangeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "VoiceChange"; }
    const juce::String getApplicationVersion() override    { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override              { return false; } // D-009

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<VoiceChangeMainWindow>();

        // 既定の入力・出力デバイス(一覧の先頭)で開く。開けなくてもクラッシュしない
        // （Linux/Xvfb環境ではデバイスが無い場合がある。デバイス未検出・保存済み設定からの
        // 復元・エラー表示はT-006/T-007で扱う）。
        const auto inputNames = audioIO.getInputNames();
        const auto outputNames = audioIO.getOutputNames();

        if (! inputNames.isEmpty() && ! outputNames.isEmpty())
            audioIO.open (inputNames[0], outputNames[0]);
    }

    void shutdown() override
    {
        audioIO.close();
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<VoiceChangeMainWindow> mainWindow;
    vc::AudioIO audioIO;
};

START_JUCE_APPLICATION (VoiceChangeApplication)
