#include <juce_gui_extra/juce_gui_extra.h>

// ===== SECTION: MainWindow =====
// T-002時点では空の固定サイズウィンドウのみ。UI本体はT-006（MainComponent）で追加する。

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
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<VoiceChangeMainWindow> mainWindow;
};

START_JUCE_APPLICATION (VoiceChangeApplication)
