#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_data_structures/juce_data_structures.h>

#include "AudioIO.h"
#include "MainComponent.h"
#include "core/Params.h"

#include <cmath>
#include <memory>

// ===== SECTION: MainWindow =====
// docs/spec.md「UI」「デバイス選択」「設定の保存」「VB-CABLE検出」、docs/design.md 7.4節、
// docs/plan.md 2.5節「Main.cpp」参照。UI本体（MainComponent）はT-006で追加した。
// トレイ常駐・切断時の再接続・統計ログはT-007。

namespace
{

juce::String pickDefaultInputName (const juce::StringArray& names)
{
    // docs/spec.md「初回起動時の既定値: 入力はシステム既定の入力デバイス」。
    // AudioIO::getInputNames()はscanForDevices()済みの一覧をそのまま返し、既定デバイスが先頭に来る
    // （docs/plan.md 1章の事実5「一覧では既定デバイスがindex 0」）。
    return names.isEmpty() ? juce::String() : names[0];
}

juce::String pickDefaultOutputName (const juce::StringArray& names)
{
    // docs/spec.md「出力は『CABLE Input』を含むデバイス（なければシステム既定の出力）」。
    for (const auto& n : names)
        if (n.containsIgnoreCase ("CABLE Input"))
            return n;

    return names.isEmpty() ? juce::String() : names[0];
}

vc::SavedSettings loadSettings (juce::PropertiesFile& props)
{
    vc::SavedSettings s;
    s.inputDevice = props.getValue ("inputDevice");
    s.outputDevice = props.getValue ("outputDevice");
    s.gainDb = (float) props.getDoubleValue ("gainDb", 0.0);
    // pitchは設定ファイルの手編集等で小数になっている可能性があるため、丸めてから整数として読む。
    s.pitch = (int) std::lround (props.getDoubleValue ("pitch", 0.0));
    s.reverb = (float) props.getDoubleValue ("reverb", 0.0);
    s.preset = vc::presetFromId (props.getValue ("preset", "normal")); // 不明な名前・キー無し→Normal
    s.enabled = props.getBoolValue ("enabled", true);
    s.trayNoticeShown = props.getBoolValue ("trayNoticeShown", false);
    return vc::sanitize (s); // 範囲外の値を範囲の端へ丸める
}

// design.md 7.4節「VB-CABLE未検出ダイアログ（非モーダル）」。
class VbCableDialog final : public juce::DialogWindow
{
public:
    VbCableDialog()
        : DialogWindow (juce::String::fromUTF8 ("VB-CABLEが見つかりません"),
                         juce::Colour (vc::AppLookAndFeel::bgWindow), true, true)
    {
        setUsingNativeTitleBar (true);
        setResizable (false, false);

        auto* content = new Content();
        setContentOwned (content, true);

        centreWithSize (420, 200);
        setVisible (true);
    }

    // 非モーダル。閉じたら自分自身を破棄する（LaunchOptions::launchAsync相当の後始末を手動で行う）。
    void closeButtonPressed() override
    {
        setVisible (false);
        delete this;
    }

private:
    class Content final : public juce::Component
    {
    public:
        Content()
        {
            body.setText (juce::String::fromUTF8 (
                "加工した声をDiscordやOBSへ渡すための仮想オーディオデバイス「VB-CABLE」（CABLE Input）が"
                "見つかりませんでした。公式サイトからインストールし、本アプリを再起動してください。"),
                juce::dontSendNotification);
            body.setFont (vc::AppLookAndFeel::uiFont (14.0f));
            body.setColour (juce::Label::textColourId, juce::Colour (vc::AppLookAndFeel::textPrimary));
            body.setJustificationType (juce::Justification::topLeft);
            body.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (body);

            urlLabel.setText ("https://vb-audio.com/Cable/", juce::dontSendNotification);
            urlLabel.setFont (vc::AppLookAndFeel::uiFont (13.0f));
            urlLabel.setColour (juce::Label::textColourId, juce::Colour (vc::AppLookAndFeel::textSecondary));
            addAndMakeVisible (urlLabel);

            openButton.setButtonText (juce::String::fromUTF8 ("公式サイトを開く"));
            openButton.setColour (juce::TextButton::buttonColourId, juce::Colour (vc::AppLookAndFeel::textPrimary));
            openButton.setColour (juce::TextButton::textColourOffId, juce::Colour (vc::AppLookAndFeel::bgWindow));
            openButton.onClick = [] { juce::URL ("https://vb-audio.com/Cable/").launchInDefaultBrowser(); };
            addAndMakeVisible (openButton);

            closeButton.setButtonText (juce::String::fromUTF8 ("閉じる"));
            closeButton.onClick = [this]
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->closeButtonPressed();
            };
            addAndMakeVisible (closeButton);

            setSize (420, 200);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (20);
            body.setBounds (r.removeFromTop (110));
            urlLabel.setBounds (r.removeFromTop (24));
            r.removeFromTop (8);

            auto buttonRow = r.removeFromTop (32);
            closeButton.setBounds (buttonRow.removeFromRight (96));
            buttonRow.removeFromRight (8);
            openButton.setBounds (buttonRow.removeFromRight (140));
        }

    private:
        juce::Label body, urlLabel;
        juce::TextButton openButton, closeButton;
    };
};

class VoiceChangeMainWindow final : public juce::DocumentWindow
{
public:
    VoiceChangeMainWindow (vc::AudioIO& audioIOIn, juce::PropertiesFile& settingsIn,
                            const juce::String& desiredInput, const juce::String& desiredOutput)
        : DocumentWindow ("VoiceChange", juce::Colour (vc::AppLookAndFeel::bgWindow),
                           DocumentWindow::minimiseButton | DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setResizable (false, false);

        auto* content = new vc::MainComponent (audioIOIn, settingsIn, desiredInput, desiredOutput);
        setContentOwned (content, true);

        centreWithSize (content->getWidth(), content->getHeight());
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

} // namespace

class VoiceChangeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "VoiceChange"; }
    const juce::String getApplicationVersion() override    { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override              { return false; } // D-009

    void initialise (const juce::String&) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "VoiceChange";
        options.filenameSuffix = "settings";
        options.folderName = "VoiceChange";
        options.osxLibrarySubFolder = "Application Support";
        options.millisecondsBeforeSaving = 1000; // 変更の1秒後にまとめて保存
        options.storageFormat = juce::PropertiesFile::storeAsXML;

        settings = std::make_unique<juce::PropertiesFile> (options);
        const auto saved = loadSettings (*settings);

        // AudioIO::open()（engine.prepare()を内部で呼ぶ）より前に層1パラメータ・プリセット・
        // ON/OFFを設定しておく必要がある（Engine::prepare()がgainSmoothedの初期値に使うため）。
        auto& params = audioIO.engineParams();
        params.gainDb.store (saved.gainDb, std::memory_order_relaxed);
        params.pitch.store (saved.pitch, std::memory_order_relaxed);
        params.reverb.store (saved.reverb, std::memory_order_relaxed);
        params.preset.store ((int) saved.preset, std::memory_order_relaxed);
        params.enabled.store (saved.enabled, std::memory_order_relaxed);

        const auto inputNames = audioIO.getInputNames();
        const auto outputNames = audioIO.getOutputNames();

        // D-009: 起動時に保存済みのデバイスが見つからない場合でも既定デバイスへ切り替えない。
        // ここでの「既定値」は保存済み設定が無い場合にのみ使う。
        const juce::String desiredInput = saved.inputDevice.isNotEmpty() ? saved.inputDevice
                                                                          : pickDefaultInputName (inputNames);
        const juce::String desiredOutput = saved.outputDevice.isNotEmpty() ? saved.outputDevice
                                                                            : pickDefaultOutputName (outputNames);

        if (desiredInput.isNotEmpty() && desiredOutput.isNotEmpty())
            audioIO.open (desiredInput, desiredOutput); // Linux/Xvfbでデバイスが無ければ失敗するがクラッシュしない

        mainWindow = std::make_unique<VoiceChangeMainWindow> (audioIO, *settings, desiredInput, desiredOutput);

        if (! vc::containsCableInput (outputNames))
            new VbCableDialog(); // 非モーダル。閉じると自分自身を破棄する

        handleScreenshotArgumentIfPresent();
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        audioIO.close();

        if (settings != nullptr)
            settings->saveIfNeeded();

        settings = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    // `--screenshot <path>`: 起動後にMainComponentのスナップショットをPNG保存して終了する
    // （Xvfb上でのUI確認用。docs/plan.md 2.5節「Main.cpp」参照）。
    void handleScreenshotArgumentIfPresent()
    {
        const auto args = getCommandLineParameterArray();

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--screenshot" && i + 1 < args.size())
            {
                const juce::String path = args[i + 1];

                juce::Timer::callAfterDelay (400, [this, path] { takeScreenshotAndQuit (path); });
                return;
            }
        }
    }

    void takeScreenshotAndQuit (const juce::String& path)
    {
        if (mainWindow != nullptr)
        {
            if (auto* content = mainWindow->getContentComponent())
            {
                const auto image = content->createComponentSnapshot (content->getLocalBounds());

                juce::File file (path);
                file.deleteFile();

                if (auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
                {
                    juce::PNGImageFormat png;
                    png.writeImageToStream (image, *stream);
                }
            }
        }

        quit();
    }

    std::unique_ptr<VoiceChangeMainWindow> mainWindow;
    std::unique_ptr<juce::PropertiesFile> settings;
    vc::AudioIO audioIO;
};

START_JUCE_APPLICATION (VoiceChangeApplication)
