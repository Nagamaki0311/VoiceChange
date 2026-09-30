#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_data_structures/juce_data_structures.h>

#include "AudioIO.h"
#include "MainComponent.h"
#include "core/Params.h"
#include "core/StatsLog.h"

#include <cmath>
#include <memory>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <psapi.h>
#endif

// ===== SECTION: MainWindow =====
// docs/spec.md「UI」「デバイス選択」「設定の保存」「VB-CABLE検出」「システムトレイ」「計測とログ」、
// docs/design.md 7章、docs/plan.md 2.5節「Main.cpp」参照。UI本体（MainComponent）はT-006で追加した。
// トレイ常駐・切断時の再接続・統計ログはT-007。

namespace
{

// プロセスのメモリ使用量(docs/spec.md「計測とログ」)。Windows以外は0でよい(plan.md T-007)。
std::int64_t currentProcessMemoryBytes() noexcept
{
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS pmc {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), &pmc, sizeof (pmc)))
        return (std::int64_t) pmc.WorkingSetSize;
    return 0;
#else
    return 0;
#endif
}

// トレイアイコン(design.md 7.1節)。16単位グリッドで設計し、32×32では座標・線幅を2倍にする。
// 背景は透明。errorBadgeはON/OFFいずれの版にも重ねて描く。
juce::Image makeTrayImage (bool on, bool errorBadge)
{
    juce::Image img (juce::Image::ARGB, 32, 32, true);
    juce::Graphics g (img);

    const juce::Rectangle<float> square (2.0f, 2.0f, 28.0f, 28.0f);

    juce::Path polyline;
    polyline.startNewSubPath (6.0f, 16.0f);
    polyline.lineTo (10.0f, 9.0f);
    polyline.lineTo (16.0f, 23.0f);
    polyline.lineTo (22.0f, 9.0f);
    polyline.lineTo (26.0f, 16.0f);

    if (on)
    {
        g.setColour (juce::Colour (vc::AppLookAndFeel::live));
        g.fillRoundedRectangle (square, 6.0f);
        g.setColour (juce::Colour (vc::AppLookAndFeel::liveInk));
    }
    else
    {
        constexpr juce::uint32 offColour = 0xFF8A9099; // design.md 7.1節「OFF版」固有の色(トークン表にはない)
        g.setColour (juce::Colour (offColour));
        g.drawRoundedRectangle (square, 6.0f, 3.0f);
    }

    g.strokePath (polyline, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (errorBadge)
    {
        constexpr float cx = 25.0f, cy = 25.0f, r = 7.0f, stroke = 2.0f;
        g.setColour (juce::Colour (vc::AppLookAndFeel::bgWindow));
        g.fillEllipse (cx - r - stroke, cy - r - stroke, (r + stroke) * 2.0f, (r + stroke) * 2.0f);
        g.setColour (juce::Colour (vc::AppLookAndFeel::error));
        g.fillEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
    }

    return img;
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
    VoiceChangeMainWindow (vc::AudioIO& audioIOIn, juce::PropertiesFile& settingsIn)
        : DocumentWindow ("VoiceChange", juce::Colour (vc::AppLookAndFeel::bgWindow),
                           DocumentWindow::minimiseButton | DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setResizable (false, false);

        auto* content = new vc::MainComponent (audioIOIn, settingsIn);
        setContentOwned (content, true);

        // design.md 7章「ウィンドウアイコンはトレイアイコンのON版（32px）と同じ図案」。
        setIcon (makeTrayImage (true, false));

        centreWithSize (content->getWidth(), content->getHeight());
        setVisible (true);
    }

    // 初回のトレイ格納時にだけ呼ばれる(Main.cppのVoiceChangeApplicationが設定する。design.md 7.3節)。
    std::function<void()> onHiddenToTray;

    void closeButtonPressed() override
    {
        // design.md「システムトレイ」: 閉じるボタン・Escキー(MainComponent::keyPressed経由)でトレイに格納する。
        setVisible (false);

        if (onHiddenToTray != nullptr)
            onHiddenToTray();
    }

    void showFromTray()
    {
        setVisible (true);
        toFront (true);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceChangeMainWindow)
};

// ===== SECTION: TrayIcon =====
// design.md 7章「システムトレイ、通知、ダイアログ」参照。
class TrayIcon final : public juce::SystemTrayIconComponent,
                        private juce::Timer
{
public:
    TrayIcon (vc::AudioIO& audioIOIn, VoiceChangeMainWindow& mainWindowIn, juce::PropertiesFile& settingsIn)
        : audioIO (audioIOIn), mainWindow (mainWindowIn), settings (settingsIn)
    {
        refresh();
        startTimerHz (2); // 状態表示は秒単位で十分(design.mdの30fpsはウィンドウ本体のみ)
    }

    ~TrayIcon() override { stopTimer(); }

    // 初回の格納時にだけ通知を出す(design.md 7.3節)。済みフラグは設定に保存する。
    void showFirstMinimizeNoticeIfNeeded()
    {
        if (settings.getBoolValue ("trayNoticeShown", false))
            return;

        showInfoBubble (juce::String::fromUTF8 ("VoiceChange"),
                         juce::String::fromUTF8 ("タスクトレイで動作を続けています。アイコンをクリックするとウィンドウを再表示します。"));
        settings.setValue ("trayNoticeShown", true);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::Process::makeForegroundProcess();
            showMenu();
        }
        else
        {
            mainWindow.showFromTray();
        }
    }

private:
    void timerCallback() override { refresh(); }

    // design.md 6.1節「トレイ」列: ON/OFF版、または赤いバッジ(デバイス切断・再接続中、開けない、E5の10秒間、
    // E6=入力デバイス0件)。
    bool hasErrorBadge() const
    {
        return audioIO.isReconnecting()
            || audioIO.hasRecentEngineError()
            || audioIO.getInputNames().isEmpty()
            || ((! audioIO.isOpen()) && audioIO.getFailedSide() != vc::AudioIO::FailedSide::None);
    }

    // (on, err, tooltip)が前回から変わったときだけアイコンとツールチップを更新する
    // (OSのトレイ更新は不要に繰り返さない)。
    void refresh()
    {
        const bool on = audioIO.engineParams().enabled.load (std::memory_order_relaxed);
        const bool err = hasErrorBadge();
        const juce::String tooltip = buildTooltip (on, err);

        if (hasShown && on == shownOn && err == shownErr && tooltip == shownTooltip)
            return;

        hasShown = true;
        shownOn = on;
        shownErr = err;
        shownTooltip = tooltip;

        const auto image = makeTrayImage (on, err);
        setIconImage (image, image);
        setIconTooltip (tooltip);
    }

    juce::String buildTooltip (bool on, bool err) const
    {
        if (err)
            return juce::String::fromUTF8 ("VoiceChange — エラー：") + buildErrorSummary();

        if (on)
        {
            const auto preset = (vc::Preset) audioIO.engineParams().preset.load (std::memory_order_relaxed);
            return juce::String::fromUTF8 ("VoiceChange — ON（") + vc::presetDisplayName (preset) + juce::String::fromUTF8 ("）");
        }

        return juce::String::fromUTF8 ("VoiceChange — OFF（バイパス）");
    }

    // design.md 7.1節「例『出力デバイス切断』」に沿った短い要約。
    juce::String buildErrorSummary() const
    {
        if (audioIO.isReconnecting())
        {
            const bool inMissing = audioIO.getDesiredInputName().isNotEmpty()
                                  && ! audioIO.getInputNames().contains (audioIO.getDesiredInputName());
            const bool outMissing = audioIO.getDesiredOutputName().isNotEmpty()
                                   && ! audioIO.getOutputNames().contains (audioIO.getDesiredOutputName());

            if (inMissing)  return juce::String::fromUTF8 ("入力デバイス切断");
            if (outMissing) return juce::String::fromUTF8 ("出力デバイス切断");
            return juce::String::fromUTF8 ("音声停止・再接続中");
        }

        if (audioIO.hasRecentEngineError())
            return juce::String::fromUTF8 ("音声処理で異常を検出");

        if (audioIO.getInputNames().isEmpty())
            return juce::String::fromUTF8 ("入力デバイスなし");

        return juce::String::fromUTF8 ("デバイスを開けません");
    }

    void showMenu()
    {
        const bool on = audioIO.engineParams().enabled.load (std::memory_order_relaxed);

        juce::PopupMenu menu;
        menu.setLookAndFeel (&menuLookAndFeel); // design.md 7.2節「ウィンドウと同じLookAndFeel」
        menu.addItem (1, juce::String::fromUTF8 ("ウィンドウを表示"));
        menu.addItem (2, on ? juce::String::fromUTF8 ("エフェクトをOFFにする")
                             : juce::String::fromUTF8 ("エフェクトをONにする"));
        menu.addSeparator();
        menu.addItem (3, juce::String::fromUTF8 ("終了"));

        menu.showMenuAsync (juce::PopupMenu::Options {}, [this] (int result)
        {
            if (result == 1)
            {
                mainWindow.showFromTray();
            }
            else if (result == 2)
            {
                auto& enabled = audioIO.engineParams().enabled;
                const bool newState = ! enabled.load (std::memory_order_relaxed);
                enabled.store (newState, std::memory_order_relaxed);
                settings.setValue ("enabled", newState);
                refresh();
            }
            else if (result == 3)
            {
                juce::JUCEApplication::getInstance()->systemRequestedQuit();
            }
        });
    }

    vc::AudioIO& audioIO;
    VoiceChangeMainWindow& mainWindow;
    juce::PropertiesFile& settings;
    vc::AppLookAndFeel menuLookAndFeel;

    bool hasShown = false, shownOn = false, shownErr = false;
    juce::String shownTooltip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrayIcon)
};

} // namespace

class VoiceChangeApplication final : public juce::JUCEApplication,
                                      private juce::Timer
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
        const auto saved = vc::loadSettings (*settings);

        // AudioIO::open()（engine.prepare()を内部で呼ぶ）より前に層1パラメータ・プリセット・
        // ON/OFFを設定しておく必要がある（Engine::prepare()がgainSmoothedの初期値に使うため）。
        auto& params = audioIO.engineParams();
        params.gainDb.store (saved.gainDb, std::memory_order_relaxed);
        params.pitch.store (saved.pitch, std::memory_order_relaxed);
        params.reverb.store (saved.reverb, std::memory_order_relaxed);
        params.preset.store ((int) saved.preset, std::memory_order_relaxed);
        params.enabled.store (saved.enabled, std::memory_order_relaxed);

        // マイク処理（ノイズ除去・EQ）。全体ON/OFFの対象外（D-020）。
        params.nrEnabled.store (saved.nrEnabled, std::memory_order_relaxed);
        params.nrBackground.store (saved.nrBackground, std::memory_order_relaxed);
        params.nrImpact.store (saved.nrImpact, std::memory_order_relaxed);
        params.eqEnabled.store (saved.eqEnabled, std::memory_order_relaxed);

        for (size_t i = 0; i < saved.eqBands.size(); ++i)
            params.eqBands[i].store (saved.eqBands[i]);

        const auto inputNames = audioIO.getInputNames();
        const auto outputNames = audioIO.getOutputNames();

        // D-009: 起動時に保存済みのデバイスが見つからない場合でも既定デバイスへ切り替えない。
        // ここでの「既定値」は保存済み設定が無い場合にのみ使う。
        const juce::String desiredInput = saved.inputDevice.isNotEmpty() ? saved.inputDevice
                                                                          : vc::pickDefaultInputName (inputNames);
        const juce::String desiredOutput = saved.outputDevice.isNotEmpty() ? saved.outputDevice
                                                                            : vc::pickDefaultOutputName (outputNames);

        // 名前が空の側(デバイス0件)も含めて必ず呼ぶ。失敗しても名前は記録され、AudioIOが2秒ごとの再試行と
        // 既定デバイスが現れたときの採用を続ける。Linux/Xvfbでデバイスが無ければ失敗するがクラッシュしない。
        audioIO.open (desiredInput, desiredOutput);

        mainWindow = std::make_unique<VoiceChangeMainWindow> (audioIO, *settings);

        // design.md 7章「システムトレイ」。閉じるボタン・Escで格納したとき、初回だけ通知を出す。
        trayIcon = std::make_unique<TrayIcon> (audioIO, *mainWindow, *settings);
        mainWindow->onHiddenToTray = [this] { trayIcon->showFirstMinimizeNoticeIfNeeded(); };

        if (! vc::containsCableInput (outputNames))
            new VbCableDialog(); // 非モーダル。閉じると自分自身を破棄する

        // docs/spec.md「計測とログ」。起動時に1MB超なら作り直し、以後60秒ごとに追記する。
        logFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("VoiceChange").getChildFile ("VoiceChange.log");
        logFile.getParentDirectory().createDirectory(); // PropertiesFileの保存タイミングに依存しない
        vc::resetIfLarger (logFile, 1024 * 1024);
        appStartMs = (juce::int64) juce::Time::getMillisecondCounterHiRes(); // 32bitのgetMillisecondCounter()は約49.7日でラップする
        startTimer (60000);

        handleScreenshotArgumentIfPresent();
    }

    void shutdown() override
    {
        stopTimer();
        trayIcon = nullptr;
        mainWindow = nullptr;

        // 設定の保存はデバイスのclose()（ドライバ次第で時間がかかる・固まる）より前に済ませる。
        if (settings != nullptr)
            settings->saveIfNeeded();

        audioIO.close();
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

    // 60秒ごとの統計ログ追記(docs/spec.md「計測とログ」)。メッセージスレッドのタイマーからのみ書く。
    void timerCallback() override
    {
        const auto& fifoStats = audioIO.getFifoStats();
        const double inRate = audioIO.getInputInfo().rate;

        vc::StatsSnapshot snap;
        snap.elapsedSeconds = (double) ((juce::int64) juce::Time::getMillisecondCounterHiRes() - appStartMs) * 0.001;
        snap.latencyMs = audioIO.getLatency().totalMs;
        snap.fillMs = inRate > 0.0 ? (double) fifoStats.fillSmoothedSamples.load (std::memory_order_relaxed) / inRate * 1000.0 : 0.0;
        snap.underruns = fifoStats.underruns.load (std::memory_order_relaxed);
        snap.overruns = fifoStats.overruns.load (std::memory_order_relaxed);
        snap.speedCorrectionPpm = (double) fifoStats.speedCorrectionPpm.load (std::memory_order_relaxed);
        snap.cpuPercent = (double) audioIO.getCpuLoad() * 100.0;
        snap.memoryBytes = currentProcessMemoryBytes();

        logFile.appendText (vc::formatStatsLine (snap) + "\n", false, false, nullptr);
    }

    std::unique_ptr<VoiceChangeMainWindow> mainWindow;
    std::unique_ptr<TrayIcon> trayIcon;
    std::unique_ptr<juce::PropertiesFile> settings;
    vc::AudioIO audioIO;

    juce::File logFile;
    juce::int64 appStartMs = 0;
};

START_JUCE_APPLICATION (VoiceChangeApplication)
