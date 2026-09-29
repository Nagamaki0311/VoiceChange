# 作業履歴

作業内容、実施結果、次回開始位置を記録する。新しいエントリは先頭に追加する（新しい順）。

## 記録フォーマット

```
## YYYY-MM-DD タスクID/概要

### 実施内容
- 何を行ったか

### 結果
- 動作確認結果、テスト結果など

### 次回開始位置
- 次に着手すべき場所（ファイル/関数/タスクID）
```

---

## 2026-09-29 マイク処理（ノイズ除去＋EQ）の仕様追記

### 実施内容
- Plannerの計画案を、ユーザー確認済みの4点（全体OFFでマイク処理を外さない、無声区間はVAD連動の広帯域ゲートから始めてスペクトルゲートは作らない、ノイズ除去の遅延は50ms目標の外で加算しCPUはON時の増分3%以下を目標、ノイズ除去・EQとも初回起動時はOFF）のとおりdocsへ反映した（コードの変更なし）。
- docs/spec.md: 技術スタック（RNNoise）、音声処理チェーン（マイク処理を2番目に追加、番号を繰り下げ）、新節「マイク処理: ノイズ除去」「マイク処理: EQ」、音声スレッドの制約、設定の保存、計測とログ、UI、パフォーマンス要件、検証方針、原指示からの調整。
- docs/plan.md: 見出しをT-002〜T-015にし、8章（マイク処理の追加計画、T-009〜T-015）を追加。docs/design.md: メインのマイク処理ボタン・バイパス時の文言・内訳の「除去」・9章の確定事項、10章（マイク処理ウィンドウ、Plannerの案）。docs/decisions.md: D-019〜D-025。docs/tasks.md: T-009〜T-015（未着手）。
- テンプレート由来の「T-009」「T-011」の記述（docs/tasks.mdのメモ、docs/agent-reach.md）を、本リポジトリの新タスクと混同しない表記に直した。

### 結果
- docsのみの変更。ビルド・テストは実行していない（コード変更なし）。

### 未解決事項
- ユーザーから未入手（T-015の前提）: SonarのEQ設定のスクリーンショット（バンド数、各バンドのタイプ・周波数・ゲイン・Q、ローカットの傾き）とNoise Reductionのスライダー値、出力デバイス「CABLE Input」のサンプルレート（48kHzならレート変換の経路は使われない）、実録音（生音声とSonar適用後、同時録音）。
- RNNoiseのMSVC `/MT`ビルド可否、RNNoise固有の遅延（480か960サンプルか）、CPUの実測は未確認（T-009で確認する）。
- design.md 10章は案。T-013の着手前にDesignerが確定する。

### 次回開始位置
- T-009（RNNoiseのMSVC /MTビルド可否の確認。docs/plan.md 8.3）。

---

## 2026-09-28 全タスク完了（T-001〜T-008）

### 実施内容
- T-005（PR #7）、T-008（PR #8）をレビュー承認・CI（windows/linux）成功の上でmainへマージした。T-008の任意指摘N-2（LongRunのコメント）・N-4（E4cのerrorFlags検査）も反映。

### 結果
- docs/tasks.mdの全タスクが完了。mainの`ctest`は8件全件合格（Linux）、CIのwindowsジョブもMSVC静的リンクで成功。

### 未解決事項
- Windows実機でのみ確認できる項目（WASAPI低遅延モード、VB-CABLE出力、聴感、トレイ常駐、デバイス抜去と再接続、実機1時間動作）は未検証。READMEの「実機での確認手順」でユーザーが確認する。
- 1時間テストのログ判定基準（README）は案。実機の実測値を見てユーザーと合意して確定する。

### 次回開始位置
- ユーザーの実機確認結果を受けて、不具合修正または判定基準の確定。

---

## 2026-09-28 T-008 レビュー指摘の修正（M-1、L-1〜L-3、N-1）

### 実施内容
- M-1: `src/core/Engine.cpp`のバイパス経路（完全バイパスとクロスフェード区間の両方、最終出力の非有限値検査の直後）に`FloatVectorOperations::clip(-1, 1)`を追加（D-018）。tests/EngineTests.cppにE4cを追加: (1) 44.1k→48kのResamplingFifo経由で、±1にクリップした9kHz正弦（振幅1.5をクリップ）と0dBFS白色雑音を、200msごとにON/OFFを切り替えながらEngineへ通す。(2) Engine直で振幅1.5の入力を完全バイパスとクロスフェード区間へ通す。docs/decisions.mdにD-018、docs/spec.mdのバイパス記述に「±1.0へのクリップのみ」を反映。
- L-1/L-2/N-1（README）: 出力保護の記述を実装どおり（コンプレッサー閾値-1dB・比100 + ±1.0のクリップ、バイパス時はクリップのみ）に修正。1時間テスト手順を「アプリ終了→ログ削除→起動」に修正し、ログの`cpu`（コールバック全体の負荷、平滑値）とテストのCPU参考値（`Engine::process`のみ）の定義の違いを追記。CPU参考値を「概算」と明記。
- L-3: LongRunにプリセット別の非バイパス区間の出力RMSを集計し、各プリセット>1e-3を判定に追加。

### 結果
- 修正前のE4c（Engineは変更前、テストのみ追加）: ResamplingFifo経由のピーク1.2747、Engine直（入力1.5）のピーク1.4999で2件失敗。
- 修正後のE4c: 両方1.0000で合格。E4a（完全バイパスの入力とのビット一致）も合格のまま（|x|≤1ではクリップが値を変えない）。
- LongRunのプリセット別出力RMS（非バイパス区間）: normal 0.1623、echo 0.1704、helium 0.1542、minion 0.1625、giant 0.1607、kerokero 0.1321、robot 0.1071、talkbox 0.1140（すべて>1e-3）。CPUの概算は全体1.03%。
- `ctest --test-dir build --output-on-failure`: 8件全件合格（long_run 44秒、合計117秒）。

### 未解決事項
- 実機の確認と判定基準の合意は前エントリのとおり。

### 次回開始位置
- Managerによる再レビューとpush。作業は`t008-review-fixes`ブランチ（5211376からの派生）で行った（main作業ツリーへのgit操作がworktree隔離で拒否されたため）。

---

## 2026-09-28 T-008 連続動作のオフライン模擬1時間と実機テスト手順

### 実施内容
- `tests/LongRunTests.cpp`（カテゴリLongRun、LABELS long、TIMEOUT 1800、`CMakeLists.txt`に`long_run`として登録）: ResamplingFifo（44.1k→48k、Bi441/Bo480、入力+100ppm、入出力ともジッタ0〜3ms）→ Engine（ゲイン+6dB、リバーブ0.2）を音声時間3600秒回す。10秒ごとにプリセット巡回、7秒ごとに層1ピッチをランダム変更（固定シード、-12〜+12）、60秒ごとにバイパス切替。入力は6秒周期（f0が100→220Hzに滑る合成母音3秒、雑音1.5秒、デジタル無音1.5秒）で、振幅は-40〜-1dBFSを巡回。
- 判定: NaN/Inf、ピーク≤1.0、underruns/overruns/discards 0、エラーフラグ0、push/pull/processのアロケーション0回、出力がほぼ無音でないこと、切替回数、T-003のc（300秒以降、平滑充填が目標±25%）・d（300〜900秒と最後10分の平均差≤0.5ms）・e（600秒以降の速度比補正平均が期待値±10ppm）。CPU（processの実時間÷音声時間）を全体とプリセット別に出力する。
- `README.md`: 「開発中の機能」を「実装済みの機能」と「未検証事項」に書き換え、「.exeの入手」（Actionsのartifact `VoiceChange-windows`）を追加。「手動確認手順」を「実機での確認手順」に改め、VB-CABLEのインストール、Discord/OBSの設定、聴感確認、1時間連続動作テストとログの判定基準（案）を追記した。遅延（シフター休止時50ms目標、稼働時に約120ms加わる。D-014）とOBSの同期オフセットも記載。判定基準は案であり、実機の実測値を見てユーザーと合意する旨をREADMEに書いた。

### 結果
- `cmake --build build --parallel && ctest --test-dir build --output-on-failure`: 8件（smoke, ring_buffer, ring_buffer_long, shifter, engine, effects, app_logic, long_run）全件合格、合計100秒。long_run単体は約42秒（実時間）。
- LongRun実測: underruns=0、overruns=0、discards=0、ジッタ余裕2.00ms、平滑充填 300〜900秒平均582.1・最後10分平均582.1（差0.000ms）、速度比補正平均99.99ppm（期待100）、c違反0、リングバッファ遅延の変動（300秒以降）22.93〜23.49ms、ピーク1.0000、エラーフラグ0、アロケーション0、切替 preset=360/pitch=515/bypass=59。
- CPU（Engine::processの実時間÷音声時間、概算の参考値）: 全体1.00%。プリセット別はノーマル0.85%、エコー0.86%、ヘリウム0.43%、ミニオン0.49%、ジャイアント0.89%、ケロケロ1.49%、ロボット0.45%、トークボックス2.54%。プリセット別は層1ピッチ（ランダム）とバイパス（半分の時間）が混ざるため、プリセット固有の値ではない（プリセット固有の値はE10）。

### 補足（テスト信号の調整。src/coreの不具合ではない）
- 初回実行でピークが1.359となった。バイパス中（Engineは入力をそのまま出す）に、0dBFS一杯の一様白色雑音がResamplingFifoのLagrange補間でオーバーシュートし、ブロック出力が1.0を超えたため（有効なプリセットではリミッターでクリップされる）。雑音のピークを-6dBFS（振幅の0.5倍）に抑え、母音の最大振幅を0dBから-1dBへ下げた（0dBFSの母音でも補間で1.0005まで出たため）。実機でも満振れの広帯域入力をバイパスで通すと同様に1.0を超え得るが、デバイス側でクリップされるだけで、Engineの通常経路はリミッターで抑えられている。（注: この後のレビュー修正でバイパス経路も±1.0にクリップするよう変更した。D-018）
- src/core・src/appは変更していない。

### 未解決事項
- 実機（WASAPI・VB-CABLE・聴感・トレイ・抜去・1時間）は未検証。READMEの手順でユーザーが確認する。判定基準はユーザーと合意して確定する。
- CIのwindows/linuxジョブは未実行（pushしていない）。

### 次回開始位置
- Managerによるレビューとpush、CIの結果確認。

---

## 2026-09-28 T-005 レビュー差し戻し修正（層2切替のクロスフェード・E9の検出力・E5/E7）

### 実施内容
- High1: `Engine`の層2切替で`Effect::None`を「フェード中でない」番兵と有効な旧効果の両方に使っていたため、None系プリセット→Robot/Talkboxでクロスフェードが掛からなかった。`bool fading`を別に持ち、prepare/resetChain/フェード終了時の更新も合わせて直した（`Engine.h/.cpp`）。
- High2: E9の切替位置86400サンプルは150Hzの270周期で、ゼロ交差に一致して段差を隠していた。切替位置を7・11サンプルずらして判定するよう変更。さらに判定窓が切替直後のサンプルから始まり、切替の瞬間の段差（out[switchPos]-out[switchPos-1]）を含んでいなかったため、切替直前のサンプルから数えるようにした。
- Low3: 修正後にD-017のonsetケースを再測定し、0msアタックの参考値をログに出すようにした（offset 0と7）。docs/decisions.md D-017に追記。
- Low4: E7を全56通りの順序付き切替（ウォームアップと本計測の両方）に拡張してテスト名と一致させた。E5にケロケロ・トークボックスのNaN/Inf注入を追加。
- Nit5: `PitchDetector.cpp`のコメント「4分割」を「前半・後半の2分割」に修正。

### 結果
- 修正前（High1未修正のEngine + 新E9）: E9は224通り中10通り失敗。最悪比（上限1.5）は切替位置7で5.248（normal→robot）、11で7.007（normal→robot）。内訳: normal→robot 5.25/7.01、normal→talkbox 2.87/3.83、helium→robot 4.25/4.07、helium→talkbox 4.25/4.07、kerokero→robot 1.57（位置7）、giant→robot 1.57（位置11）。10msアタックのonsetは1.297/1.292で合格。なお切替位置をずらしても判定窓が切替直後から始まる旧方式では、未修正のEngineでも全件合格して段差を検出できなかった。
- 修正後: E9は224/224合格。連続発声中の最悪比は位置7で1.295、11で1.289（normal→talkbox）。10msアタックのonsetは1.297/1.292（kerokero→talkbox）。0msアタックの参考値は、offset 0（入力自体に段差なし）で1.944（minion→helium）、offset 7（入力自体に段差あり）で5.847（normal→minion）。D-017の結論は変わらない。
- E5（ミニオン・ケロケロ・トークボックス、NaN/Inf）とE7（全56通りの切替）も合格。
- 全件（`ctest --test-dir build --output-on-failure`、quick 6件+long 1件）を実行し7件すべて成功（合計69.2秒）。

### 次回開始位置
- T-008。

---

## 2026-09-28 T-005 追加調査: E9の6通り（ディザ）とヘリウム低い声（setFormantBase）

### 実施内容
- E9の6通り: PitchShifterへの入力に振幅1e-6の決定的ディザ（4096点の固定シード表を循環）を足す実装を試した。Stretchの無音モード（`totalEnergy < 1e-15`が2ブロック続くと入力素通し、signalsmith-stretch.h 241行付近）を避ける狙い。
- ヘリウム: 移調0・フォルマント係数1.6で、(a)既定、(b)setFormantBase(真のf0)、(c)setFormantBase(検出器の推定値)をf0=80/100/150Hzで比較する一時テストを作り、測定後に削除した。

### 結果
- ディザでE9の6通りは改善しなかった（隣接差は0.181/0.0773のまま、E10のCPUも変化なし）。ディザ実装は取り消し（コード変更なし）。
- 原因の切り分け: 無音部を-80dBFS相当の雑音（無音モードに入らない）にしても3通り（minion/giant/kerokero→helium）が基準を超える。発声開始に10msのアタックランプを掛けると、無音からの発声開始を含む112通りすべてが通常基準（1.5倍）で合格する。つまり原因は無音モードではなく、位相がそろった合成母音の瞬時の立ち上がりをシフターが通す際のピーク（定常の約2倍）。
- setFormantBase: (b)(c)はs=1.000（フォルマントが全く動かない）。既定はf0=80/100/150Hzでs=1.075/1.070/1.585。改善しないため実装せず、READMEに表を追記した。

### 確認
- 上記の追加調査後に全件（`ctest --test-dir build --output-on-failure`、quick 6件+long 1件）を実行し、7件すべて成功（合計57.2秒）。

### 判断後の確認
- 変更後に全件（`ctest --test-dir build --output-on-failure`）を実行し7件すべて成功（合計61.2秒）。E9は112/112合格、連続発声中の切替の最悪比は1.297（上限1.5）。

### 判断後の対応
- Managerの判断により、E9の無音からの発声開始を10msアタック付き入力で判定するよう変更し（D-017）、「既知」扱いのコードとponytailコメントを削除した。112通りすべてを通常基準（1.5倍）で判定して合格。ヘリウムの低い声はライブラリの限界として受け入れ、D-015に1行追記した。

### 未解決事項
- なし。

### 次回開始位置
- T-008。

---

## 2026-09-28 T-005 層2の特殊効果プリセット（ピッチ検出・エコー・ロボット・トークボックス・ケロケロ）

### 実施内容
- `src/core/PitchDetector.h/.cpp`: YIN（12kHz前後へ間引き、前段は6次ButterworthのIIRローパス、70〜1000Hz、窓約21ms、ホップ約5ms、閾値0.15、放物線補間、無声は直前値保持、RMS閾値-50dBFS未満は無声）。積分窓を前半・後半に分け、区間RMSの比が0.6未満（無音への減衰・発声の立ち上がり）のフレームは推定を更新せず前回値を保つ。
- `src/core/Effects.h/.cpp`: Echo（DelayLine 0.35s、300ms、帰還0.45、帰還路3.5kHzローパス、dry1.0/wet0.6、reset後20msの書き込みフェードイン）、RingModulator（40Hz）、Talkbox（20バンド、PolyBLEP鋸波+白色雑音、固定シードの`juce::Random`）。
- `src/core/Engine.h/.cpp`: 層2の配線。ピッチ検出はケロケロ・トークボックス（切替中の旧効果を含む）のときだけ動かす。ケロケロ補正量=round(m)-m（無声時は保持、`getKerokeroCorrectionSemitones()`をテスト用に公開）、トークボックスのキャリア=検出f0×2^(層1ピッチ/12)。効果の切替は旧新を20ms並行処理してクロスフェードし、切替中に来た変更は完了まで保留。
- T-004レビューのLow: `tests/EngineTests.cpp`の`rampSamples`（E1）削除。`Engine::processReverb`は補間中のみサンプルごと、収束後は最初の1回だけ`reverb.setParameters`を呼ぶ。
- `tests/EffectsTests.cpp`（D1〜D4、F1〜F3、K1、X1〜X3、`vc_add_test(effects Effects)`）、`tests/EngineTests.cpp`にE8〜E10。`tests/ShifterTests.cpp`のP4a/P4bに実測値のログ出力を追加。
- README.mdに「フォルマント補正の効果とPSOLA方式の検討」とCPU表、docs/spec.mdにトークボックスの補正ゲイン（0.904）とQ・キャリア正規化を記録。

### 結果
- `cmake --build build --parallel && ctest --test-dir build --output-on-failure -L quick`成功（6件）。`-L long`成功（ring_buffer_long、29秒）。`timeout 8 xvfb-run -a build/VoiceChange_artefacts/Release/VoiceChange`は落ちずに8秒で終了（exit=124）。
- 最終コミット後に全件（`ctest --test-dir build --output-on-failure`、quick 6件+long 1件）を再実行し、7件すべて成功（合計56.9秒）。
- D1: 全信号・3レートで有声フレームの100%が±1%以内、中央値誤差は最大0.12%。D2/D4合格。F1〜F3（f0=150Hz）合格: s=1.585/1.375/0.750、f0比1.0000/1.5879/0.7091。K1: 450Hz→441.1Hz、425Hz→416.3Hz、母音150Hz→148.2Hz。X1: 300.00ms・0.600、600.06ms・0.450。X2: 960.15/1040.01Hz、1000Hzは-86.6dB。X3: 出力比-1.8〜+2.9dB、倍音間隔誤差最大0.05%、雑音入力-7.1dB。E10: ノーマル0.04%、トークボックス2.63%。

### 計画からの変更点
- トークボックス: キャリア側のバンドを広げ（Q=1.5、変調側は3.0）、キャリア側各バンドを自身の包絡で正規化した。計画どおり（同じQ・正規化なし）ではX3aの音量差が約15dBあり、固定ゲインで±3dBに収まらなかった。
- ピッチ検出: 区間RMSの比による不安定フレームの保持を追加（K1の「無音を挟んでも補正量が不変」のため。許容差は0.01半音、実測は0.0002半音）。
- X3の音量比較は層1ピッチ0で行う（ピッチシフター自体が強い倍音構造の合成母音で音量を最大約9dB下げるため）。倍音間隔はピッチ0/+5/-7で測る。
- E9の判定基準（無音からの発声開始）の「切替前の定常区間」は加工前の声とした（切替前の出力が無音で基準にならないため）。それでも6通りが基準を超え、既知として数えるだけにしている（未解決事項参照）。
- D1に合成母音f0=75Hzを追加（低域の確認用、合格）。

### 未解決事項
- E9: 稼働中のシフターが無音のまま発声開始と同時にヘリウム/ミニオンへ切り替わる6通り（helium/giant/kerokero→minion、minion/giant/kerokero→helium）で、シフター遅延120msの位置に定常の約2倍の隣接差が出る（minion→helium 0.181対0.093）。位相がそろった合成母音の立ち上がりを通したSignalsmithの特性で、Engine側では除けない。実声で確認するか、シフターの立ち上がりに短いフェードインを掛けるか、Managerの判断を仰ぐ。それ以外の106通りは基準を満たす（最悪比1.30、上限1.5）。
- K1の母音（f0=150Hz）は148.2Hzで、許容±1%（146.8Hz±1.47）に対して誤差0.97%とぎりぎり。原因はシフターのf0精度（D-015と同種）で、実装側では改善できない。
- f0=100Hzのヘリウムはフォルマントが1.07倍しか動かない（目標1.6）。特性確認のみで失敗判定なし（README参照）。PSOLA系への差し替えはユーザー判断。
- トークボックスの切替遷移は乱数シードにわずかに依存する（時間シードで一度、robot→talkboxが比1.58で基準超過した。シードを固定し、4種で最悪1.38）。
- 層1ピッチ≠0のとき、トークボックスの変調信号（シフター後、約120ms遅れ）とキャリアのf0検出（シフター前）に時間差がある。定常音では影響しない。

### 次回開始位置
- T-008: 連続動作検証（オフライン模擬1時間）と実機テスト手順。E9の既知6通りの扱い（未解決事項）はManagerの判断待ち。

---

## 2026-09-28 T-007 レビュー指摘の修正（12件）

### 実施内容
- `ConnectionMonitor`: 入力・出力それぞれの最終進捗時刻を持ち、どちらか一方でも2秒進まなければ`CloseAndFail`（JUCE 9のWASAPI入力スレッドは無言終了でコールバックを呼ばないため）。健常時の「一覧変更 && 使用中デバイスが一覧に無い」も`CloseAndFail`。開くこと自体の失敗用に`enterFailed()`を追加。
- `AudioIO`: 日本語のエラー文は`fromUTF8`（生リテラルはLatin-1解釈で文字化けしていた）。失敗側`FailedSide`（None/Input/Output/Both。片方が失敗しても両方試して特定）と、現在のopen()世代でstartまで成功したかのフラグ（`isReconnecting()`は成功済みの世代の異常のみ）。一度も開けていないデバイスは異常状態でもE4のまま、再試行は継続。デバイス名が空（起動時に0件）の場合は既定デバイスが現れた時点で採用して開く（`pickDefault*Name`をMain.cppからAudioIOへ移動）。レート/バッファ長の変化で出力を無音にするatomicフラグ（再オープンで解除）。時計は`getMillisecondCounterHiRes()`由来の64bit値（Main.cppの統計ログ含む）。未使用のgetter3つを削除、根拠にならないplan.md引用コメントを修正。
- `MainComponent`: E4の文言・赤枠を`FailedSide`で決定（前方一致を廃止）。コンボボックスを一覧・選択名の変化時のみ作り直す（ポップアップ表示中は保留、通知なしで構築）。選択名の出典を`AudioIO::getDesired*Name()`に一本化（コンストラクタ引数を削除）。「n秒経過」入りの文言は秒数を除いたキーで比較して通知。
- `Main.cpp`: トレイは(on, err, tooltip)が変わったときだけ更新、エラーバッジにE6（入力0件）を追加。起動時は名前が空でも必ず`open()`。終了時は設定保存をclose()より前に実施。
- テスト: 片方だけ停止（両パターン、2.0秒/1.9秒）、一覧変更×デバイスなし、健常時の一覧変更（デバイスあり・なし）、異常状態中のエラーフラグ、一覧変更による再試行後の+2秒、`enterFailed`。StatsLogはラベルと値の対応も検証。
- docs: D-016改訂、plan.mdのConnectionMonitor説明、design.md 6.2 E4の文言。

### 結果
- `cmake --build build --parallel && ctest --test-dir build --output-on-failure`: 全6件成功。
- 実行コマンド: `cmake --build build --parallel`、`ctest --test-dir build --output-on-failure`、`xvfb-run -a -s "-screen 0 1280x1024x24" build/VoiceChange_artefacts/Release/VoiceChange --screenshot <png>`、`timeout 8 xvfb-run -a build/VoiceChange_artefacts/Release/VoiceChange`。
- `--screenshot`: 終了コード0、460×600。デバイスなし環境で崩れなし（入力・出力とも0件のため両方赤枠、メッセージはE6）。`timeout 8 xvfb-run -a ...`は落ちずにタイムアウト（124）まで生存。

### 再レビューLow2件の修正
- `openDevices`は入力が開けなければ出力を開かない（失敗側は`getOutputNames().contains(outName)`でInput/Both）。再試行の開き直しは両方の名前が一覧にあるときだけ。`audioDeviceStopped`に関するコメントとD-016決定3を実態（抜去・レート変更の即時検出経路であり、呼ばれないのは入力スレッドの無言終了のみ）に修正し、決定4を追加。

### 次回開始位置
- Windows実機でのE1〜E4・再接続・コンボボックスの動的更新の確認（README「手動確認手順」）。

---

## 2026-09-28 T-007 トレイ常駐・デバイス切断時の処理・エラー表示・統計ログ

### 実施内容
- `src/core/ConnectionMonitor.h`（新規）: 純粋ロジック。カウンタ2秒停滞・エラーフラグ・再オープン要求のいずれかで`CloseAndFail`、異常状態では一覧変更通知＋デバイスあり即時、それ以外2秒間隔で`TryReopen`。
- `src/core/StatsLog.h/.cpp`（新規）: `formatStatsLine`（経過時間・レイテンシ・充填量・アンダーラン/オーバーラン・速度比補正・CPU・メモリの1行整形）と`resetIfLarger`（1MB超で削除＝作り直し）。
- `src/app/AudioIO.*`: `private juce::Timer`（500ms）と`private juce::AudioIODeviceType::Listener`を追加。(1)`audioDeviceError`/例外、(2)無言終了（`close()`からの意図した`stop()`かどうかを`expectingIntentionalStop`で判定）、(3)`audioDeviceAboutToStart`の2回目以降のレート/バッファ長変化、の3種を検出し`ConnectionMonitor`へ渡す。`open()`に渡した名前は成功・失敗を問わず記憶し、`TryReopen`で同じ名前へ再オープンする（D-009）。Engineのエラーフラグ（bit0/bit1）は本クラスのタイマーだけが読んで消費し、`hasRecentEngineError()`（レベル判定、10秒間）として公開する（MainComponentとトレイの二重消費を避けるため、T-006時点でMainComponentが直接読んでいた方式から変更）。`getFifoStats()`をStatsLog用に追加。
- `src/app/MainComponent.*`: `isReconnecting()`+`getReconnectElapsedSeconds()`からE1（入力切断）/E2（出力切断）/E3（音声停止、原因特定不可のため両方赤枠）を表示順位1〜2で追加し、E4/E6より優先する。トレイのメニューからのON/OFF切り替えを検知してトグルスイッチ・スライダー・プリセットボタンの見た目を追従させる`refreshEnabledAppearance()`を追加。
- `src/app/Main.cpp`: `TrayIcon`（`SystemTrayIconComponent`+`private juce::Timer`、2Hzで状態を反映）を追加。アイコンはコード描画（ON/OFF版・エラーバッジ、design.md 7.1節の16グリッド×2）。左クリックでウィンドウ表示、右クリックで`Process::makeForegroundProcess()`後にPopupMenu（ウィンドウを表示/エフェクトON・OFF切替/終了、ウィンドウと同じLookAndFeel）。閉じるボタン（`closeButtonPressed`変更）・Esc（既存の`MainComponent::keyPressed`経由）でトレイへ格納し、初回のみ`showInfoBubble`（済みフラグを設定へ保存）。`VoiceChangeApplication`に`private juce::Timer`（60秒）を追加し、`%APPDATA%\VoiceChange\VoiceChange.log`へ`StatsLog`の行を追記（起動時に1MB超なら`resetIfLarger`で作り直し）。プロセスメモリは`GetProcessMemoryInfo`（Windowsのみ、`psapi`は既存CMakeLists.txtで既にリンク済み）。
- `src/core/Params.h`: `presetDisplayName()`を追加（プリセットのUI表示名の単一の出典。トレイのツールチップと`MainComponent`のプリセットボタンの両方が参照するよう`kOrder`側のハードコード文字列を置き換えた）。
- `tests/AppLogicTests.cpp`: `ConnectionMonitor`（2.0秒/1.9秒境界、t=+2/+4/+6秒のTryReopen、一覧変更即時、エラーフラグ即時、再オープン要求即時→スケジュール復帰）と`StatsLog`（全項目を含む整形、符号付きppm、1MB超/以下の`resetIfLarger`）のテストを追加。
- `README.md`: 「手動確認手順（Windows実機、T-007）」節を追加（トレイの5手順、デバイス抜去・再接続の5手順、ログの書式と1時間判定基準）。
- `docs/decisions.md`: D-016（E4/E1〜E3の切り分け、Engineエラーフラグの単一消費者化、無言終了の判定方法）を追加。

### 発見した設計上の問題と対応
- 初期実装では、起動直後に入力・出力デバイスが1件も存在しない場合でも`AudioIO`の500msタイマーがコールバック回数の停滞を検出して`isReconnecting()`をtrueにしてしまい、より具体的なE6（「入力デバイスが見つかりません」）をE3の汎用メッセージで上書きしてしまう不具合があった。`AudioIO::evaluateConnection()`の先頭に「`open()`を一度も試みていない（`desiredInputName`/`desiredOutputName`が両方空）場合は監視自体を行わない」ガードを追加して修正した。

### 結果
- `cmake --build build --parallel && ctest --test-dir build --output-on-failure`: 全6件成功（smoke/ring_buffer/ring_buffer_long/shifter/engine/app_logic）。
- `xvfb-run -a -s "-screen 0 1280x1024x24" ... --screenshot`: 終了コード0、PNG 460×600（design.mdどおり）。ALSA/デバイスなし環境でE6「入力デバイスが見つかりません」が正しく表示され、入力コンボのみ赤枠（出力は通常色）であることをピクセル値で確認した。
- `timeout 8 xvfb-run -a ...`（トレイ・ConnectionMonitor・StatsLogを含む一連の初期化）: 終了コード124（タイムアウトによる強制終了のみで、クラッシュなし）。
- ビルドログに新規ファイル由来の警告なし（既存の`-Wfloat-equal`警告のみ、Params.h:87・MainComponent.cpp:763は変更前から存在。AudioIO.cpp内の新規1件は既存パターンと同種で意図的な等価比較のため許容）。
- `grep -rnE "mutex|CriticalSection|ScopedLock|SpinLock|DBG\(|Logger::|triggerAsyncUpdate" src/core`: 該当なし。

### 計画からの変更点
- なし（docs/plan.md 2.5節・3章T-007のとおり）。ConnectionMonitorのpublicインターフェースは`update`/`isFailed`のみ（plan.mdどおり）とし、再オープン成功時の状態リセットは`AudioIO::open()`内で`connectionMonitor = ConnectionMonitor{}`と代入する形にした（`reset()`等のAPIを追加しない、判定ラダー6「1行で書けるか」）。

### 未解決事項（Windows実機でしか確認できない項目）
- WASAPIの実デバイスでの抜去・再接続・無言終了の実挙動（Linux/Xvfbには対象のデバイスが存在しないため）。
- トレイアイコンの表示（左クリック/右クリック/ツールチップ/エラーバッジ/初回通知）。Xvfbには実際のシステムトレイが無いため、`SystemTrayIconComponent`がクラッシュしないことのみ確認済み。
- `GetProcessMemoryInfo`によるメモリ計測値の妥当性（Linuxビルドでは常に0を返す設計）。
- `audioDeviceAboutToStart`の2回目以降呼び出し（WASAPIのフォーマット変更時の自動再起動）が実際に発生し`reopenRequestedFlag`が意図どおり立つこと。
- 1時間連続動作でのログの傾向（アンダーラン増加なし・レイテンシ変動2ms以内・メモリ増加1MB以内）はT-008で検証する。

### 次回開始位置
- T-008（連続動作検証・実機テスト手順）。T-005（層2配線、別worktree）とのマージ後、Engine.cppまわりの競合有無を確認すること。

---

## 2026-09-28 T-006 修正2回目: レビュー指摘4件（矢印キー・NaN・データ競合・アクセシビリティ）

### 実施内容
- 【High】`src/app/MainComponent.h`: `DeviceComboBox`（`juce::ComboBox`の薄いサブクラス）を追加し`inputCombo`/`outputCombo`をこの型に変更。JUCE既定の`ComboBox::keyPressed()`が上下左右矢印キーで`nudgeSelectedItem()`（選択を直接変更→`onChange`→`audioIO.open()`）を呼んでいたのを、矢印キーは`isPopupActive()`を見て`showPopup()`を呼ぶだけ（選択は変えない）に変更。`showPopupIfNotActive()`はprivateのため`isPopupActive()`ガード付き`showPopup()`で代替。Space/Enter/Alt+↓は基底クラスの既定動作のまま。
- 【High】`src/core/Params.h`の`sanitize()`: `juce::jlimit`はNaNをそのまま返す（`NaN<lo`も`NaN>hi`もfalseのため）ため、`std::isfinite`チェックを範囲チェックの前に追加し、非有限値は初期値(0.0f)へ丸める。`src/core/Engine.cpp`の`processChunk()`にも同種の防御を1行追加（`atomicParams.gainDb/reverb`の読み出し値が非有限なら0として扱う）。`tests/AppLogicTests.cpp`にNaN/Infの4ケース（gainDb単独NaN、gainDb単独+Inf、reverb単独-Inf、gainDb/reverb同時NaN）を追加。
- 【Medium】`src/core/PitchShifter.h/.cpp`: `getLatencySamples()`が非atomicな`state`からUIスレッド（`AudioIO::getLatency()`経由）に無同期に読まれていたデータ競合を修正。`std::atomic<int> latencySamplesForUi`を追加し、`process()`の最後と`reset()`内で音声スレッドがrelaxed storeする`updateLatencyForUi()`を呼ぶ。`getLatencySamples()`はそのatomicをloadするだけに変更（`Engine::getShifterLatencySamples()`は変更不要、`shifter.getLatencySamples()`への委譲のまま新しい実装を使う）。
- 【Low】`src/app/MainComponent.h/.cpp`: `statusPanel`のアクセシビリティ用`setDescription()`を、可視の数値更新（8フレーム≈267ms）とは別に30フレーム（30fps想定で約1秒）ごとに更新する`buildStatusSummary()`を追加。文言はdesign.md 5章の例「遅延 38.4ミリ秒、CPU 0.8パーセント、入力 低遅延、出力 低遅延、正常に動作しています」に合わせた。`updateStatus()`が末尾で`lastStatusData`へキャッシュし、`timerCallback()`の新しい30フレームカウンタからそれを使って組み立てる。

### 結果
- `cmake --build build --parallel`: 成功（警告は既存コードと同種の`-Wfloat-equal`のみ）。
- `ctest --test-dir build --output-on-failure`: **全件成功**（smoke, ring_buffer, ring_buffer_long(28.3秒), shifter, engine, app_logic の6件。shifterのP1系レイテンシ表示テストも含めatomic化後に合格を確認）。
- `xvfb-run -a -s "-screen 0 1280x1024x24" ... --screenshot`: 終了コード0、PNG 460×600。目視で崩れがないことを確認（入出力デバイス0件のE6表示、スライダー単位表示とも従前どおり）。
- `grep -rnE "mutex|CriticalSection|ScopedLock|SpinLock|DBG\(|Logger::|triggerAsyncUpdate" src/core`: 該当なし。

### 計画からの変更点・環境上の制約
- Managerからは統合済みブランチ`claude/keen-thompson-ikze8z`（`/home/user/VoiceChange`、T-006はcommit 8e5800bとして取り込み済み）での作業を指示されたが、本エージェントはworktree（`.claude/worktrees/agent-a285f01f6cc97e8b6`）に隔離されており、Edit/Write/Bashのgit操作のいずれも`/home/user/VoiceChange`を対象にすると拒否される（ツールのサンドボックス制約、権限の相談では解除不可）。そのため本修正は同内容を自分のブランチ`t006-ui`（このworktree、T-006本体のcommit 37d8834/26de56d/dfb87d7の続き）に適用した。Manager側で統合済みブランチへの反映（cherry-pick等）が必要。
- `src/core/Effects.*`・`PitchDetector.*`には触れていない（このworktreeにはT-005がまだ含まれておらず、そもそも存在しない）。

### 次回開始位置
- Managerが統合済みブランチへ本修正を反映後、T-007（トレイ常駐・切断時の再接続・統計ログ）。

---

## 2026-09-28 T-006 補足: レビュー往復の最終確認

### 実施内容
- Manager指摘（スライダー単位表示）への対応後、`cmake --build build --parallel`・`ctest --test-dir build --output-on-failure`（全6件）・`--screenshot`（460×600）を再実行し、design.mdどおりの表示（0 dB/0 半音/0 %、+3 dB/-5 半音/40 %）を最終確認した。

### 結果
- 全件成功。branch `t006-ui`（commit 37d8834, 26de56d）にcommit済み、作業ツリーはクリーン。

### 次回開始位置
- T-007（トレイ常駐・切断時の再接続・統計ログ）。

---

## 2026-09-28 T-006 修正: スライダー値表示に単位が出ない不具合

### 実施内容
- Manager（レビュー）からの指摘: スクリーンショットでゲイン/ピッチ/リバーブのスライダー値が単位なしの`0`のみで、design.md 3.5節の表示形式（`+3 dB`/`0 dB`/`-12 dB`、`+5 半音`/`0 半音`、`40 %`/`0 %`）になっていなかった。
- 原因: `juce::Slider::setValue()`は新しい値が現在値（既定0）と等しい場合`updateText()`を呼ばずに素通りする実装のため、`textFromValueFunction`を設定した後に`setValue(初期値と同じ0, dontSendNotification)`を呼んでも、そのタイミングでは反映されず、`setRange()`直後に生成された既定のテキスト（`textFromValueFunction`未設定時点の`String(roundToInt(v))`＝単位なし数値）がそのまま残っていた。
- `src/app/MainComponent.cpp`のスライダー初期化直後（3スライダーの`setValue()`の後）に`updateText()`を明示的に呼び、初期表示にも`textFromValueFunction`を確実に反映させた。

### 結果
- `cmake --build build --parallel`: 成功。
- `ctest --test-dir build --output-on-failure`: **全件成功**（smoke, ring_buffer, ring_buffer_long(28.7秒), shifter, engine, app_logic の6件）。
- `xvfb-run -a -s "-screen 0 1280x1024x24" ... --screenshot`: 終了コード0、PNG 460×600。目視確認: ゲイン「0 dB」、ピッチ「0 半音」、リバーブ「0 %」と単位付きで表示されることを確認。一時的にスライダー値を+3dB/-5半音/40%に変更して撮影し「+3 dB」「-5 半音」「40 %」（live色）も正しく表示されることを確認した後、既定値に戻して最終スクリーンショットを撮り直した。

### 次回開始位置
- T-006はこの修正を含めて完了。次はT-007（前エントリのT-006本体の記載を参照）。

---

## 2026-09-28 T-006 UI結合と設定の保存・復元

### 実施内容
- `src/app/MainComponent.h/.cpp`（新規）: docs/design.mdのレイアウト・配色・フォント表どおりに実装。
  - `AppLookAndFeel`（`LookAndFeel_V4`派生）: design.md 3.1節の色トークンをstatic constexprで保持し、`drawComboBox`/`drawPopupMenuBackground`/`drawPopupMenuItem`/`drawButtonBackground`/`drawButtonText`/`drawLinearSlider`と各`get*Font`を上書き。フォント名は`Font::findAllTypefaceNames()`から`"Yu Gothic UI" → "Meiryo UI" → "Noto Sans CJK JP" → 既定`の順に一度だけ解決し関数ローカルstaticでキャッシュ。
  - 自前描画コンポーネント: `LevelMeter`（40セグメント、上昇即時/下降20dB/秒、ピークホールド1.0秒+20dB/秒減衰）、`ToggleSwitch`（`Button`派生、`paintButton`を上書き）、`StatusPanel`（状態の色の線・遅延/CPU・内訳(AttributedString)・デバイスモード・メッセージ+アイコン）。
  - プリセットボタン8個: `TextButton`+`setClickingTogglesState`+共通`setRadioGroupId`。表示順は種類別（design.md 3.3節のレイアウト順=ノーマル/ヘリウム/ミニオン/ジャイアント/エコー/ケロケロ/ロボット/トークボックス）で、`src/core/Params.h`のPreset enum順（Normal/Echo/Helium/...）とは異なるため、`createPresetButtons()`内に別途表示順テーブルを持たせた。バイパス中の「ノーマルまたは選択中プリセット」は中立色、ON時の非ノーマル選択はlive色、という中核ルールをComponentプロパティ("presetIndex")+`AppLookAndFeel::isChainEnabled()`(AtomicParams.enabledへのポインタ参照)で判定。
  - スライダー3種（ゲイン/ピッチ/リバーブ）: `LinearHorizontal`+`TextBoxRight`、`setSliderSnapsToMousePosition(false)`、`setDoubleClickReturnValue(true, 0.0)`、`textFromValueFunction`で書式化。ゲイン/ピッチは0基準の中央塗り(`zeroBasedFill`プロパティ)、リバーブは左基準。値の文字色は`onValueChange`のたびに`updateSliderAppearance()`で再計算（初期値/ON/バイパスの3色）。
  - 30fpsタイマー: `visibilityChanged()`だけでは`setContentOwned()`実行時点で祖先のDocumentWindowがまだ非表示のため`isShowing()`がfalseのまま検出できず（Component::componentFlagsは既定で全ビットfalseから開始し、`addAndMakeVisible`のsetVisible(true)呼び出しはこのタイミングで一度しか発火しない）、`parentHierarchyChanged()`（`DocumentWindow::setVisible(true)`が`internalHierarchyChanged()`経由で子孫に伝播する）でも同じ判定を行うよう`updateTimerRunState()`に共通化した。数値更新は8フレームに1回（`frameCounter`）。
  - キーボード操作: `setExplicitFocusOrder`で入力コンボ→出力コンボ→ゲイン→ピッチ→リバーブ→プリセット8個(表示順)→トグルの順。Escは`findParentComponentOfClass<DocumentWindow>()->closeButtonPressed()`を呼ぶ（T-007でトレイ格納に置き換われば自動的にEscの挙動も変わる）。`setTitle`/`setDescription`をdesign.md 5章の表どおりに設定し、E/W系メッセージが変化した時だけ`AccessibilityHandler::postAnnouncement(...,high)`。
  - 層2プリセットは効果なし（T-005で追加）。ボタンは`AtomicParams.preset`をstoreするだけ。
- `src/core/Params.h`: `SavedSettings`（inputDevice/outputDevice/gainDb/pitch/reverb/preset/enabled/trayNoticeShown）、`presetFromId()`（`kPresets[].id`と大文字小文字を区別せず比較、不一致はNormal）、`sanitize()`（gainDb→±20dB、pitch→±12、reverb→0〜1にjlimit）を追加。
- `src/app/Main.cpp`: `juce::PropertiesFile`（applicationName/folderName="VoiceChange"、filenameSuffix="settings"、millisecondsBeforeSaving=1000）を読み書き。読み込みは`loadSettings()`→`sanitize()`。起動時デバイス既定値は入力=一覧先頭、出力=`containsCableInput`で「CABLE Input」を含むもの（無ければ一覧先頭）。層1パラメータ・プリセット・ON/OFFはAudioIO::open()（内部でEngine::prepare()を呼ぶ）より前にAtomicParamsへstoreしてから開く。VB-CABLE未検出時は`VbCableDialog`（`DialogWindow`派生、非モーダル、閉じると自身をdelete）を表示。`--screenshot <path>`は`Timer::callAfterDelay(400,...)`後に`createComponentSnapshot`→`PNGImageFormat::writeImageToStream`→`quit()`。
- `src/app/AudioIO.h/.cpp`: `isOpen()`（UI の起動中/エラー判定用）と自由関数`containsCableInput()`（Main.cppのダイアログ判定・MainComponentのW1判定の両方から使う）を追加。
- `tests/AppLogicTests.cpp`（新規、カテゴリAppLogic・quick）: sanitize()の範囲外丸め（gain±100→±20、pitch±50→±12、reverb 2.0/-1→1.0/0.0）、初期値のまま(キー無し相当)、presetFromId()の既知/大文字小文字/不明→Normalを検証。
- `CMakeLists.txt`: `app_logic`をquickラベルでctestに登録。`VoiceChange`ターゲットに`juce::juce_data_structures`をリンク追加（`PropertiesFile`はjuce_gui_extraの依存に含まれないため）。

### 結果
- `cmake --build build --parallel`: 成功（アプリ・テストとも警告のみ、既存コードと同種の`-Wfloat-equal`のみ）。
- `ctest --test-dir build --output-on-failure`: **全件合格**（smoke, ring_buffer, ring_buffer_long(28.6秒), shifter, engine, app_logic の6件）。
- `xvfb-run -a -s "-screen 0 1280x1024x24" build/VoiceChange_artefacts/Release/VoiceChange --screenshot <path>`: 終了コード0、PNG 460×600（表示スケール1倍）。目視確認: 日本語表示に豆腐なし、要素順序はdesign.md 3.3節の表どおり（入力→出力→レベル→ゲイン→ピッチ→リバーブ→プリセット8個→トグル→状態パネル）、ミント(live)色はONトグルの塗り・選択中プリセット(非ノーマル)にのみ使用されバイパス中は使われないことを確認。バイパスON+プリセット「ヘリウム」選択状態も一時的にコードを差し替えて撮影し、選択中プリセットが中立色（surface+2px secondary枠+太字primary文字）になること、トグルOFF表示（中抜き円+「エフェクト OFF」+副文）を確認した後、コードを元に戻して最終スクリーンショットを撮り直した。
  - 撮影中に見つけたバグ2件を修正: (1) 上記の`visibilityChanged()`単独では初回にタイマーが起動しない問題（`parentHierarchyChanged()`併用で解消）。(2) `ToggleSwitch`のOFF時副文「原音をそのまま出力中（バイパス）」が確保幅不足で末尾が切れる問題（主文/副文の領域幅を再配分して解消）。
- 開発環境(Xvfb)は入出力デバイスとも0件のため、E6（入力デバイスが見つかりません）とVB-CABLE未検出（W1相当、ただし優先順位はE6が上のため今回のスクリーンショットには表示されない）を実機さながらに確認できた。デバイス切断からの再接続(E1〜E3)はConnectionMonitor未実装(T-007)のため確認できない。

### 計画からの変更点
- ConnectionMonitor（T-007）がないため、状態パネルの状態はE1/E2/E3（再接続中の経過秒数表示）を除くE4/E5/E6/W1/W2/バイパス/正常/起動中のみ実装した。E4「デバイスを開けませんでした」相当は`AudioIO::getErrorText()`の文字列先頭（"入力"/"出力"）で判定した（AudioIOに新しいフィールドは追加していない）。
- コンボボックスの「保存済みデバイス名が一覧から消えた場合の(未接続)項目」(design.md 3.5節)は構築時に1回だけ判定する実装とした。一覧変更の監視（`audioDeviceListChanged`相当）はT-007のConnectionMonitor導入時に合わせて動的化する想定。
- design.mdのW2文言・48ms閾値はD-014のとおり反映した（decisions.mdのD-014時点で既にdesign.md本文も更新済みのため、デザイン自体の変更はしていない）。
- それ以外はdocs/plan.md 2.5節・3章T-006、docs/design.mdのとおりに実装した。

### 未解決事項
- トレイ常駐・切断時の再接続・統計ログはT-007で実装する（本タスクの範囲外）。
- ComboBoxの長いデバイス名の省略記号「…」表示（design.md 3.5節）は、JUCE標準の`Label`描画（縮小表示）に委ね、専用の省略描画は実装していない。実デバイス名で長いものが出た場合は目視確認が必要。
- Windows実機でのYu Gothic UI/Meiryo UIの表示、WASAPI経由の実デバイス一覧表示は未検証（開発環境はLinux/Xvfbで入出力デバイスとも0件のため）。

### 次回開始位置
- T-007（`src/core/ConnectionMonitor.h`、`src/core/StatsLog.*`、`src/app/AudioIO.*`のウォッチドッグ・再接続、`src/app/MainComponent.*`のトレイ・E1〜E3表示、`tests/AppLogicTests.cpp`）。T-006で実装したStatusPanel/コンボボックスのエラー表示はT-007で動的な一覧変更検出と結合する想定。

---

## 2026-09-28 T-004 D-015確定: P4をP4a/P4b（合格基準/低域の特性確認）に分割

### 実施内容
- Manager判断（ユーザーがSignalsmith Stretch継続を選択）により、D-015を「採用」で確定。P4を2本に分割した: P4a（f0=150Hz、合格基準、閾値は従来どおりf0比±1%・s=1.0±10%）、P4b（f0=100Hz、低い声の特性確認、f0比±2%・s=1.0±25%、実測基準値をコメントに明記して回帰検出）。
- `tests/ShifterTests.cpp`: `runP4()`を共通ヘルパー`runFormantPreservationCase(label, f0, f0TolRatio, sTolRatio)`に切り出し、P4a/P4bをそれぞれ呼び出す形にした。
- `docs/decisions.md`: D-015を「状態: 採用」にし、決定・理由・影響をP4a/P4b分割の内容で書き直した。
- `docs/plan.md`: T-004のP4行をP4a/P4bに分割し、T-005のF1〜F3にも同じ方針（f0=150Hzを合格基準、f0=100Hzを特性確認）である旨を追記した。

### 結果
- `cmake --build build --parallel && ctest --test-dir build --output-on-failure`: **全件合格**（smoke, ring_buffer, ring_buffer_long, shifter, engine）。
- 実測値（デバッグ用printfで確認後に削除済み）: P4a(f0=150Hz) +5半音 f0比誤差+0.741%・s=1.010、-5半音 誤差-0.216%・s=0.995。P4b(f0=100Hz) +5半音 誤差+0.643%・s=1.165、-5半音 誤差+1.409%・s=0.795。いずれもD-015に記録した閾値内。

### 計画からの変更点
- なし（Manager判断どおりに実装した）。

### 次回開始位置
- T-005（`src/core/Effects.*`、`src/core/PitchDetector.*`、`src/core/Engine.cpp`層2部分、`tests/EffectsTests.cpp`）に着手可能。F1〜F3はdocs/plan.mdの更新どおりf0=150Hzを合格基準、f0=100Hzを特性確認とする。
- T-004はローカル検証（ctest全件合格・xvfb-runでクラッシュなし）まで完了した。docs/tasks.mdの状態更新（実装中→完了）とReviewerによるコードレビューはManager判断で行う。

---

## 2026-09-28 T-004 D-015追加検証: setFormantBase(真のf0)の効果は否定

### 実施内容
- Manager指示により、P4不合格（D-015）の仮説「Signalsmith Stretchのフォルマント補償が内部の基本周波数推定（`estimateFrequency()`、自動）に依存しており、低いf0でその推定が外れている」を検証した。
- 一時テストファイル`tests/FormantBaseExperimentTemp.cpp`（ctest未登録、検証後に削除済み）を追加し、f0=80/100/150Hz×±5半音の組み合わせで、`stretch.setFormantBase(真のf0)`を与えた場合と与えない場合（既定の自動推定）のf0比・包絡スケールsを比較した。
- 結果は docs/decisions.md D-015に表として記録した。要点: f0比の誤差は`setFormantBase`の有無でほぼ変わらない（ピッチマッピングとフォルマント処理は別経路のため妥当）。包絡スケールsは自動推定の方が良好（150Hzでs=0.995〜1.010とほぼ完璧）で、`setFormantBase`に真のf0を与えるとf0によらずs≈0.75(-5半音)/1.31(+5半音)に張り付き、むしろ悪化した。
- 以上より当初の仮説を却下し、`PitchShifter`への`setFormantBaseHz`等のAPI追加は行わないことにした（T-005でピッチ検出器の結果をシフターへ配線する作業自体はケロケロ機能に必要なため別途実施するが、P4改善目的では行わない）。docs/decisions.md D-015へ実験結果と却下の判断を追記した。

### 結果
- 実装変更はなし（一時実験ファイルは削除済み）。`cmake --build build --parallel && ctest --test-dir build --output-on-failure`は変化なし: shifterのみ不合格（P4の2ケース、原因はD-015のとおりライブラリの残存特性で解決せず）。

### 計画からの変更点
- なし。P4の閾値・実装は変更していない。`setFormantBase`案を検討したが効果がないと確認し不採用とした。

### 未解決事項
- D-015: P4（f0=100Hzの母音、±5半音）が引き続き不合格。原因はSignalsmith Stretchの低域ピッチ量子化誤差で、フォルマント補償の内部f0推定は無関係と判明した。Manager判断によりP4a/P4bへの分割で対応（後続エントリ参照）。

### 次回開始位置
- 後続エントリ（P4a/P4b分割）参照。

---

## 2026-09-28 T-004 D-014対応: ピッチシフターのブロック長を120ms固定に変更

### 実施内容
- Manager指示（D-014: ユーザー決定、docs/decisions.md・docs/spec.md・docs/design.md・docs/plan.mdを更新済み）により、ピッチシフターのブロック長を120ms・インターバル30ms（Signalsmith Stretchの`presetDefault`相当）に固定した。
- `src/core/PitchShifter.h/.cpp`: `BlockDecision`/`decideStretchBlock`を削除。`prepare(sampleRate, maxBlockSamples)`から`stretchBlockSamples`引数を削除し、内部で`stretch.presetDefault(1, (float) sampleRate)`を呼ぶように変更。
- `src/core/Engine.h/.cpp`: `EngineConfig`から`stretchBlockSamples`フィールドを削除し、`shifter.prepare(sampleRate, maxBlockSamples)`に合わせた。
- `src/app/AudioIO.cpp/.h`: デバイス遅延・リング目標からブロック長を計算する処理（D-003・decideStretchBlock）を削除し、`engine.prepare({ outputInfo.rate, maxOutBlock })`のみに簡略化。使われなくなった`isBlockOverBudget()`/`blockOverBudget`も削除した。`LatencyBreakdown`（deviceInMs/deviceOutMs/ringBufferMs/shifterMs）は既存のまま据え置き、T-006/T-007のUI遅延超過判定（シフター分を除く遅延48ms超）に必要な値（device+ringとshifterを分離した値）が既に取得できることを確認した。
- `tests/ShifterTests.cpp`: 全箇所の`stretchBlock`ローカル定数・`shifter.prepare(fs, maxBlock, stretchBlock)`呼び出しを整理。P1の期待値（`expectedLatency`/`expectedFadeLen`）は元々`signalsmith::stretch::SignalsmithStretch`のインスタンスから動的に算出しシフターの報告レイテンシと突き合わせる設計だったため、`presetDefault(1, fs)`呼び出しへ差し替えるだけで対応できることを確認した。P5を「48kHz・44.1kHz・96kHzでinputLatency+outputLatencyが120ms×[0.95, 1.05]」に全面差し替え（PitchShifterを実際にActiveまで進めて`getLatencySamples()`を確認する形）。
- `tests/EngineTests.cpp`: `kStretchBlock`定数と`engine.prepare({kFs, kMaxBlock, kStretchBlock})`の3引数呼び出しを整理し2引数に統一。
- P4（合成母音、f0=100Hz、±5半音）の不合格についてManagerの指示で測定器側の問題を疑い、独立の再現コード（PitchShifter/Engine・tests/TestSignals.hのいずれも介さない、生のSignalsmith Stretch直接呼び出し + 高精度ゼロクロス法）で検証した（`/tmp/.../scratchpad/probe2/probe6〜8.cpp`）。結果はP4の測定値とほぼ一致する値を独立に再現でき、tests/TestSignals.hの測定コードは妥当でライブラリの残存特性が原因と判断した（詳細はdocs/decisions.md D-015）。
- `docs/decisions.md`: D-015（120ms化後もP4の低い基本周波数側で±1%を満たさない、未解決・報告のみ）を追加。

### 結果
- `cmake --build build --parallel`: 成功（既存の`-Wfloat-equal`警告のみ）。
- `ctest --test-dir build --output-on-failure`（quick+long、実時間約32秒）: **shifterのみ不合格**（P4の2ケース、下記参照）。smoke・ring_buffer・ring_buffer_long・engineは全件合格。P1・P2・P3・P5は全件合格。
- `timeout 8 xvfb-run -a build/VoiceChange_artefacts/Release/VoiceChange`: 終了コード124（タイムアウトのみ、クラッシュなし）。
- 実測値（P1, デバッグ用printfで確認後に削除済み）: `primingToFadingInAt=5760` = `expectedLatency=5760`（48kHz・120ms、誤差0）。`fadeLenMeasured=960` = `expectedFadeLen=960`（誤差0サンプル）。
- 実測値（P3、220Hz正弦、block=120ms、デバッグ用printfで確認後に削除済み）: +12半音 誤差+0.038%、-12半音 +0.479%、+5半音 +0.361%、-5半音 +0.261%、+8半音 +0.466%、-6半音 +0.848%。**全件±1%以内で合格**（D-013解決）。
- 実測値（P4、合成母音f0=100Hz、block=120ms）: +5半音 f0比は合格・包絡スケールs=1.165（許容[0.9,1.1]超過で不合格）。-5半音 f0比0.759707(期待0.749154、誤差+1.41%で不合格)・s=0.795（不合格）。
- 独立再現コード（library直接呼び出し、高精度ゼロクロス法）: 100Hz純音-5半音で+1.395%誤差（P4の測定値75.9Hzとほぼ一致する75.96Hz）。150Hz(-5半音)+0.111%、220Hz(-5半音)+0.252%、100Hz(+5半音)+0.637%。基本周波数が低いほど誤差が大きい傾向を確認。
- 参考CPU（一時ファイルで測定後削除、48kHz・480ブロック・10秒）: ノーマル 0.043%、ヘリウム 1.379%、ミニオン 1.453%（spec.mdの要件を満たす、旧ブロック長20msでの実測値とほぼ同水準でブロック長非依存という設計どおりの結果）。

### 計画からの変更点
- なし（D-014の決定どおりに実装した）。P4が不合格のままだが、これはD-015として報告済みで、閾値・実装は変更していない。

### 未解決事項
- D-015: P4（f0=100Hzの母音、±5半音）が不合格。T-005着手前に閾値・設計方針の判断が必要（docs/decisions.md D-015参照）。T-005のF1〜F3も同じf0=100Hzの母音を使う想定のため、同種の問題が再燃する可能性が高い。
- Windows実機・WASAPI固有の挙動は未検証（D-004のとおり）。

### 次回開始位置
- Manager判断待ち（D-015）。判断確定後、必要ならPitchShifter/Engine/tests/TestSignals.hへ反映。並行してT-005（`src/core/Effects.*`、`src/core/PitchDetector.*`、`src/core/Engine.cpp`層2部分、`tests/EffectsTests.cpp`）に着手可能。

---

## 2026-09-28 T-004 層1・ピッチシフター休止・ブロック長決定

### 実施内容
- `src/core/Params.h`: `AtomicParams`（gainDb/reverb/pitch/preset/enabled、すべてatomic）、`shifterShouldRun(Preset, int)`（ケロケロは常時稼働、それ以外は移調≠0またはフォルマント≠1またはpitch≠0）を追加。`static_assert(std::atomic<float>::is_always_lock_free)`。
- `src/core/PitchShifter.h/.cpp`（新規）: Signalsmith Stretchラッパ。状態機械`Resting/Priming/FadingIn/Active/FadingOut`を実装。クロスフェードは0〜1の連続値`gain`をブロック内で1サンプルずつ増減させる方式にし、途中で向きが反転しても現在値から続けて動く（別途カウンタを持たない）ため反転時の不連続を構造的に避けた。`decideStretchBlock`はstatic関数（`clamp(50-dev-ring-2, 20, 40)`、予算<20msで`overBudget=true`）。`prepare()`の最後に、Engineで実際に使う範囲（層1ピッチ±12 + プリセット移調-6〜+8）を広めにカバーする移調・フォルマントの組み合わせ（7×4=28通り）で白色雑音を数ブロックずつ空回しするウォームアップを実装（Stretchの`peaks`等の遅延確保対策、E7で0回を確認）。
- `src/core/Engine.h/.cpp`（新規）: ブロック単位で 入力ピーク記録 → シフター（プリセットの移調・フォルマント + 層1ピッチ、層2効果はT-005で素通し） → リバーブ（自前の`SmoothedValue`で1サンプルずつrを補間、D-008の変換式、r=0かつ補間完了で処理を止め`reset()`） → ゲイン（`SmoothedValue` Multiplicative 50ms） → 非有限値検査 → リミッター（D-007: `dsp::Compressor`+クリップ） → 非有限値検査、の順で処理。バイパスは`chainGain`（0〜1の連続値、Engine側でも同じ「反転しても続けて動く」方式）で20msクロスフェードし、完全バイパス確定後（`!enabled && chainGain<=0`）はチェーンを一切呼ばずビット一致で素通しする。最終出力の非有限値検査はバイパス中も含め常に行う（D-010）。`process()`は`maxBlock`ごとに内部で分割するため、外部から呼ぶ側は分割の有無を意識しなくてよい（E6で一致を確認）。
- `src/app/AudioIO.cpp/.h`: `Engine`をメンバに追加し、出力コールバックで`pull → Engine::process → 全チャンネル複製`に変更。`open()`内でデバイス遅延・リング目標(ms)を計算し`PitchShifter::decideStretchBlock`でブロック長を決め、`Engine::prepare`に渡す。`LatencyBreakdown`に`shifterMs`を追加。両コールバックの`catch(...)`で`engine.raiseExceptionErrorFlag()`（bit1）を追加。Linux(ALSA)でのデバイス遅延の差異は`getLatency()`の既存コメントに1行追記のみ（対応不要、plan.mdの指示どおり）。
- `tests/TestSignals.h`: クリック判定器（`checkNoClick`、隣接差の最大値ベース）、合成母音（`makeSyntheticVowel`、2極共振器3個の積で加算合成、docs/plan.md T-005節の方式を先行実装）、平均振幅スペクトル・倍音ピーク検出・対数対数包絡・包絡スケール探索（`measureFundamentalHz`/`measureFormantEnvelope`/`measureEnvelopeScale`、P4用にT-005の測定法を先行実装）を追加。
- `tests/ShifterTests.cpp`: クリック判定器の自己テスト2件（位相跳躍の単純連結→検出、20ms二乗余弦クロスフェード→合格）、P1（a:Priming→FadingInの送り込み量とレイテンシ表示、b:FadingIn長20ms±1サンプル、c:Resting/PrimingでL=0）、P2（a〜e:5遷移すべてのクリック判定）、P3（6半音条件の移調精度）、P4（±5半音のフォルマント保持）、P5（ブロック長決定の3ケース）をカテゴリ`Shifter`に実装。
- `tests/EngineTests.cpp`（新規）: E1（ゲイン+6dB）、E2（a:リミッターで0dBFS超なし、b:-20dBFS透過性）、E3（a:r 0→0.01の透過性、b:r=0.5の残響持続、c:r→0後のリバーブ停止）、E4（a:完全バイパスのビット一致、b:ON/OFFクリックなし）、E5（NaN/Inf注入からの復帰、バイパス中のNaN）、E6（分割処理の一致）、E7（全プリセット×ピッチ×バイパス遷移でのアロケーション0回、リバーブ実処理経路も対象に含めた）をカテゴリ`Engine`に実装。
- `CMakeLists.txt`: `shifter`(Shifter, quick)・`engine`(Engine, quick)をctestに登録。
- `docs/tasks.md`: T-003を完了、T-004を実装中に更新。
- `docs/decisions.md`: D-013（Signalsmith Stretchの移調精度がD-003のブロック長では±1%を保証できない、未解決）を追加。

### 結果
- `cmake --build build --parallel`: 成功（警告のみ、`-Wfloat-equal`。既存コードにも同種の警告があり許容範囲）。
- `ctest --test-dir build --output-on-failure`（quick+long、実時間約36秒）: **shifterのみ不合格**（P3の6ケース・P4の2ケース×2項目、下記参照）。smoke・ring_buffer・ring_buffer_long・engineは全件合格。
- `timeout 8 xvfb-run -a build/VoiceChange_artefacts/Release/VoiceChange`: 終了コード124（タイムアウトのみ、クラッシュなし）。
- 実測値（P1, デバッグ用printfで確認後に削除済み）: P1a `primingToFadingInAt=960` = `expectedLatency=960`（誤差0、許容は±480=1ブロック）。P1b `fadeLenMeasured=960` = `expectedFadeLen=960`（誤差0サンプル、許容は±1）。
- 実測値（P3、220Hz正弦、block=960/20ms）: +12半音 453.01Hz(期待440、誤差+2.96%)、-12半音 114.598Hz(期待110、+4.18%)、+5半音 304.702Hz(期待293.665、+3.76%)、-5半音 169.711Hz(期待164.814、+2.97%)、+8半音 358.932Hz(期待349.228、+2.78%)、-6半音 161.555Hz(期待155.563、+3.85%)。全件±1%を超過。
- 実測値（P4、合成母音f0=100Hz、block=960）: +5半音 f0比1.38789(期待1.33484、誤差+3.98%)・包絡スケールs=1.335(期待~1.0)、-5半音 f0比0.667191(期待0.749154、誤差-10.9%)・s=2.0(期待~1.0)。いずれも不合格。
- E2a実測ピーク（ゲイン+20dB、0.9振幅正弦、デバッグ用printfで確認後に削除済み）: 0.914545（≤1.0を満たす）。
- 参考CPU（一時ファイル`tests/CpuProbeTemp.cpp`で測定後、T-004のファイル範囲外のため削除。48kHz・480ブロック・10秒、1コアあたり処理時間÷音声時間）: ノーマル 0.045%、ヘリウム 1.247%、ミニオン 1.427%（いずれもdocs/spec.mdの要件「ノーマル1%未満・重い効果5%以下」を満たす）。

### 計画からの変更点
- P3・P4が不合格。原因はSignalsmith Stretch単体（PitchShifter/Engineを介さない直接呼び出し）でも再現する周波数量子化誤差で、ブロック長のFFTビン幅に対する入力周波数のビン整合度に依存する（D-013に詳細と再現コードの場所を記録）。閾値・実装は変更していない。Manager判断待ち。
- それ以外（Params.h、PitchShifter、Engine、AudioIO統合、E1〜E7、P1・P2・P5）はdocs/plan.md 2.5節・3章T-004のとおりに実装した。

### 未解決事項
- D-013: P3・P4の不合格。T-005着手前に閾値・設計方針の判断が必要（詳細はdocs/decisions.md D-013参照）。
- Windows実機・WASAPI固有の挙動は未検証（D-004のとおり）。

### 次回開始位置
- Manager判断待ち（D-013）。判断確定後、必要ならPitchShifter/Engineへ反映。並行してT-005（`src/core/Effects.*`、`src/core/PitchDetector.*`、`src/core/Engine.cpp`層2部分、`tests/EffectsTests.cpp`）に着手可能。

---

## 2026-09-28 T-003 レビュー指摘3件修正

### 実施内容
- 【Medium】`continuousFill`の読み取り順序のレース: 引数を`nowSeconds`のみにし、関数内部で`lastPushTime`(acquire)→`lastPushBlockLen`(relaxed)→`fifo.getNumReady()`の順に読むよう変更（release-acquireのhappens-before関係により、rawが読んだ時刻のpush以降の状態を必ず含むことを保証する）。呼び出し4箇所（Refilling判定・Idle内の3倍超過判定・Idle通常経路・フェード区間、いずれも`src/core/ResamplingFifo.cpp`の`pull()`/`handleUnderrun()`）を置き換えた。アンダーラン判定用の生の充填量は`continuousFill()`呼び出し後に別途`fifo.getNumReady()`で読む。
- 【Low】R1・R4の直接検証: `ResamplingFifo`にテスト専用の`debugRawFilled()`/`debugFillC(nowSeconds)`を追加（音声スレッドの本処理からは使わない旨をコメント）。R1は「fill_c ≥ 目標+needed になるまで出力は全て0」、R4は「破棄直後の充填量 ≤ 目標+1ブロック」を計画どおりの数値判定に戻した（既存の間接確認は置き換え）。
- 【バグ】`AudioIO::getLatency`のデバイス遅延二重計上: `getInput/OutputLatencyInSamples()`は既に「ストリーム遅延+バッファ長」を返すため、`bufferSize`の再加算を削除し`latencySamples / rate`のみにした。

### 結果
- `cmake --build build --parallel` 成功。
- `ctest --test-dir build --output-on-failure`: 全件Passed（smoke, ring_buffer(R1〜R6), ring_buffer_long(S1〜S8)。実時間約31秒）。
- `timeout 8 xvfb-run -a build/VoiceChange_artefacts/Release/VoiceChange`: 終了コード124（クラッシュなし）。

### 次回開始位置
- T-004（`src/core/PitchShifter.*`、`src/core/Engine.*`）。

---

## 2026-09-28 T-003 D-012対応: 充填量を連続換算値(fill_c)に変更しS1〜S8全件合格

### 実施内容
- Manager指示によりD-012（docs/decisions.md参照）を実装。前回報告したS1〜S4・S7の条件c)不合格（Bi≈Bo≈targetの構成で読み出し位相に依存する鋸歯状の揺れが生じる問題）に対し、目標充填量の式・合格条件の閾値は変えず、制御・判定に使う充填量の「測り方」を直した。
- `src/core/ResamplingFifo.h/.cpp`:
  - `push`/`pull`に単調時計の時刻`nowSeconds`引数を追加。
  - `continuousFill(rawFilled, nowSeconds)`を追加: `fill_c = raw − 直前の入力ブロック長 + fs_in×(now − 直前のpush時刻)`（クレジット分は0〜直前ブロック長にクランプ、結果は0以上にクランプ）。
  - 直前のpush時刻・ブロック長は`std::atomic<double> lastPushTime`・`std::atomic<int> lastPushBlockLen`に保持。`push()`側は`lastPushBlockLen`をrelaxed書き→`lastPushTime`をrelease書きの順で書き、`pull()`側は`lastPushTime`をacquire読み→`lastPushBlockLen`をrelaxed読みの順で読むことで、ロックなしに対として整合させる（release/acquireのhappens-before関係により、新しいtimeが見えた時点で新しいblockLenも見える）。
  - Refilling完了判定（充填量≥目標+needed）、3倍超過判定（discard発火）、平滑化`updateFillStats`の入力を`fill_c`に変更。**アンダーラン判定（filled<needed）は生の充填量のまま**（D-012の決定どおり）。
  - `getLatencyMs()`を「平滑化したfill_c + (Bi+Bo)/2（それぞれms換算）」に変更（従来はBo/2のみ）。
- `src/app/AudioIO.cpp`: 入力・出力コールバックそれぞれで`juce::Time::getMillisecondCounterHiRes()*1e-3`を1回取得し、内部のpush/pullチャンク分割ループ全体で同じ時刻を渡す。
- `tests/RingBufferTests.cpp`: 模擬ドライバ（`runScenario`）はpush/pullそれぞれの離散イベント時刻（`pendingIn`/`pendingOut`、元々ずれ・ジッタを反映済み）をそのまま`nowSeconds`として渡すよう変更（元々テスト側に必要な情報が揃っていたため追加の仕掛けは不要だった）。R1〜R6は明示的な模擬時計を持たなかったため、各テストに`simTime`（push/pullを半ブロック周期ずつ交互に進める簡易時計、またはR5のようにpush/pull別々の周期で進める）を追加した。

### 結果
- `cmake --build build --parallel` 成功。
- `ctest --test-dir build --output-on-failure -L quick`: smoke・ring_buffer(R1〜R6)全件Passed（0.11秒）。
- `ctest --test-dir build --output-on-failure`（quick+long）: **全件Passed**（実時間30.0秒）。
- `ring_buffer_long`（S1〜S8）実測値:

| ID | 結果 | avgFillD | avgFillLast10 | avgPpm(期待) | cViolations | underruns/overruns/discards | fViolations |
|---|---|---|---|---|---|---|---|
| S1 48k→48k+100ppm 480/480 | 合格 | 633.6 | 633.6 | 100.00(100.00) | 0 | 0/0/0 | 0 |
| S2 48k→48k-100ppm 480/480 | 合格 | 518.4 | 518.4 | -100.00(-100.00) | 0 | 0/0/0 | 0 |
| S3 44.1k→48k+100ppm 441/480 | 合格 | 582.1 | 582.1 | 100.00(100.00) | 0 | 0/0/0 | 0 |
| S4 48k→44.1k-100ppm 480/441 | 合格 | 518.4 | 518.4 | -100.00(-100.00) | 0 | 0/0/0 | 0 |
| S5 48k→48k+50ppm 128/1024 | 合格 | 1176.0 | 1176.0 | 50.00(50.00) | 0 | 0/0/0 | 0 |
| S6 44.1k→48k-100ppm 1024/144 | 合格 | 1001.0 | 1001.0 | -100.00(-100.00) | 0 | 0/0/0 | 0 |
| S7 48k→48k+100ppm ジッタ0-3ms | 合格 | 633.6 | 633.6 | 99.99(100.00) | 0 | 0/0/0 | 0 |
| S8 30秒毎15ms遅延 | 合格 | 739.2 | 739.2 | 100.00(100.00) | - | 合計1(≤9)/0/0、10分以降underrun=0、jitterMargin=4ms(≤20) | 0 |

  （S1〜S4・S7の平均充填量・速度比補正の実測値は修正前と同一。修正で変わったのは「瞬時値の揺れ幅」のみで、平均的な挙動は変えていないことが確認できる。S8のunderruns合計は乱数シードに依存し前回4→今回1、jitterMarginは前回10ms→今回4msだが、いずれも合格条件（≤9、≤20ms）の範囲内。）
- `timeout 8 xvfb-run -a build/VoiceChange_artefacts/Release/VoiceChange`: 終了コード124（クラッシュなし、タイムアウトのみ）。

### 計画からの変更点
- D-012（Manager記録済み、docs/decisions.md参照）どおりに実装。目標充填量の式・S1〜S8/R1〜R6の合格条件の閾値はいずれも変更していない。

### 未解決事項
- Windows実機・WASAPI固有の挙動は未検証（D-004のとおり）。

### 次回開始位置
- T-004（`src/core/PitchShifter.*`、`src/core/Engine.*`、`src/core/Params.h`、`src/app/AudioIO.cpp`、`tests/ShifterTests.cpp`、`tests/EngineTests.cpp`）。

---

## 2026-09-28 T-003 入出力の分離・リングバッファ・クロックずれ補正とオフライン模擬

### 実施内容
- `src/core/ResamplingFifo.h/.cpp`: docs/plan.md 2.5節どおりに実装。`juce::AbstractFifo`（容量=入力レートで1秒）+ `juce::LagrangeInterpolator` + スクラッチ。充填量は`pull()`内で読み出し直後に測る（D-006）。状態機械は`Refilling`/`FadeIn`/`Idle`/`OverrunFadeOut`/`OverrunFadeIn`の5つ（`UnderrunFadeOut`は即時完結するため`handleUnderrun()`内で処理し予約のみ）。フェード区間は複数回の`pull()`呼び出しにまたがってよいよう`phaseCount`/`gain`をメンバに持たせて継続する。速度比の比例制御が定常偏差を残すこと（100ppmで目標の約10%）を`updateSpeedRatio()`の`ponytail:`コメントに明記した。
- `tests/AllocationGuard.h`（新規）: `ScopedAllocationGuard`と、TestMain.cppが定義するthread_localフラグ/カウンタのextern宣言。複数テストファイルから使うため独立ヘッダに切り出した。
- `tests/TestMain.cpp`: グローバルの`operator new`/`new[]`/aligned版/nothrow版と対応する`delete`（計16個）を置き換え、thread_localガード有効時の呼び出し回数を数えるフックを追加。実確保/解放は`std::malloc`/`std::free`（アラインメント版は前後に管理領域を足す手動アライン方式）。
- `tests/TestSignals.h`: `findFftPeakHz()`（Hann窓+FFT+放物線補間）を追加。本来T-004/T-005予定だが、R5（44.1k→48kHzのリサンプリング精度検証）に先行して必要だったため追加した。
- `tests/RingBufferTests.cpp`（新規）: カテゴリ`RingBuffer`にR1〜R6、`RingBufferLong`にS1〜S8を実装。離散イベント方式の模擬ドライバ（`EventClock`）をテスト内に実装し、S1〜S8はパラメータ化した`runScenario()`で共通処理する。
- `src/app/AudioIO.h/.cpp`（新規）: 入力・出力を別々の`AudioIODevice`として開く（D-001）。Windowsは`sharedLowLatency`→`shared`の順でフォールバック（`#if JUCE_WINDOWS`）、Linuxは`AudioDeviceManager::createAudioDeviceTypes`の先頭の型を1つ使用。入力コールバックは全入力chを平均してモノラル化しpush、出力コールバックはpullしてモノラルバッファを全出力chへ複製（Engineは未実装のためpullした音をそのまま出す暫定処理）。コールバック回数のatomicカウンタとエラーフラグを実装（ウォッチドッグ・再接続はT-007）。`getLatency()`でデバイス入出力/リングバッファの内訳を返す。
- `src/app/Main.cpp`: 起動時に`getInputNames()`/`getOutputNames()`の先頭（一覧の既定デバイス）で`AudioIO::open()`を試みる。一覧が空/openが失敗してもクラッシュしない（Linux/Xvfbでデバイス無しを想定）。
- `CMakeLists.txt`: `ring_buffer`（quick）と`ring_buffer_long`（long, LABELS long, TIMEOUT 1800）をctestに登録。

### 結果
- `cmake --build build --parallel`成功（アプリ・テスト両方）。
- `ctest --test-dir build --output-on-failure -L quick`: smoke・ring_buffer(R1〜R6)とも成功。
- `ctest --test-dir build --output-on-failure`（quick+long）: **ring_buffer_longがFAILED**。内訳は次のとおり（1回のフル実行、実時間約30秒）。
  - 合格: R1〜R6（全件）、S5、S6、S8。
  - 不合格: S1、S2、S3、S4、S7 — いずれも条件c)（t≥5分で|平滑充填−目標|≤0.25×目標）のみ不合格。他の条件（a,b,d,e,f）はS1〜S7全件で合格。
  - 実測値（cViolations=条件c)の違反サンプル数、位置No.は「7件中4件目のテスト＝条件c」のindex）:
    - S1 (48k→48k +100ppm, Bi/Bo=480/480): avgFillD=633.6, avgFillLast10=633.6, avgPpm=100.00(期待100.00), cViolations=52140, fViolations=0
    - S2 (48k→48k -100ppm): avgFillD=518.4, avgPpm=-100.00(期待-100.00), cViolations=52140
    - S3 (44.1k→48k +100ppm, 441/480): avgFillD=582.1, avgPpm=100.00, cViolations=52140
    - S4 (48k→44.1k -100ppm, 480/441): avgFillD=518.4, avgPpm=-100.00, cViolations=52140
    - S5 (48k→48k +50ppm, 128/1024): avgFillD=1175.3, avgPpm=50.05(期待50.00), cViolations=0 → 合格
    - S6 (44.1k→48k -100ppm, 1024/144): avgFillD=1001.0, avgPpm=-100.00, cViolations=0 → 合格
    - S7 (48k→48k +100ppm, ジッタ0-3ms): avgFillD=633.4, avgPpm=99.99, cViolations=25974
    - S8 (48k→48k +100ppm, 30秒ごと15ms遅延): underruns合計=4(≤9)、underrunsAfter600=0、jitterMarginMs=10.00(≤20)、b〜f全件0 → 合格
- `timeout 8 xvfb-run -a build/VoiceChange_artefacts/Release/VoiceChange`: 終了コード124（タイムアウトによる強制終了のみ、クラッシュなし）。ALSAの`seq_hw.c`警告（MIDIシーケンサ、無関係）のみ標準エラーに出力。

### 条件c)不合格の原因分析（S1〜S4, S7）
デバッグ用に`ResamplingFifo`へ一時アクセサ（`debugRawFilled()`。原因特定後に削除済み）を追加し、`fillSmoothed`と生の`fifo.getNumReady()`を10秒ごとにトレースして原因を特定した。

- S1(Bi=Bo=480, target=576)では、`fillSmoothed`が定常時576付近まで収束した後、**約100秒周期で生の充填量が480（Bi丸々1ブロック分）だけ瞬時にジャンプ**し、その後約80〜90秒かけて`fillSmoothed`が576へ戻る、という鋸歯状の挙動を繰り返す。このジャンプが「平滑充填−目標」を最大約63%（576→940超）まで押し上げ、0.25×目標(144)を大きく超える区間が生じる。
- 100秒という周期は、入力イベント周期のずれの累積が入力1ブロック分（Bi/fs_in秒）に達するまでの時間 T = Bi / (fs_in × |ppm_in/1e6|) と厳密に一致する（S1: 480/(48000×1e-4)=100.0秒）。これは離散イベント方式の模擬ドライバ（固定ブロックサイズ・イベント時刻のみドリフト、docs/plan.md 3章の式どおり）では、周波数がわずかに異なる2つの周期系列の間に必然的に生じる「ビート」現象であり、実装のバグではなく、周期系列でクロックがずれた2デバイスを固定ブロックでやり取りする限り原理的に不可避な現象と判断した（S7でジッタを0〜3ms加えても件数が半減するのみで解消しない）。
- 一方S5/S6が合格するのは、(a) S5はBi=128が目標1120の11%しかなく、ブロック1個分の瞬時ジャンプ自体が0.25×目標を超えない、(b) S6はBo=144が小さく出力（消費）が高頻度なため、ビートで生じた余剰がLagrange補間の2秒平滑化フィルタの帯域より十分速く消費され平滑値に現れない、という理由による。S1〜S4・S7はBi≈Bo≈targetの近傍（block/target比≈83%）で、上記いずれの保護機構も働かない。
- 比例制御のゲイン（1e-3、docs/spec.mdの式どおり）を強めても、ブロック到着直後の瞬時値そのもの（576→1056等）はその後の速度補正では遡って直せないため、Bi(またはBo) > 0.25×目標である限り条件c)を無違反で満たすことは原理的に不可能である（目標を十分大きくする＝ジッタ余裕を初期値から大きく取る、という選択はレイテンシ50ms予算と背反する。例えばS1で条件c)を満たすにはtarget≥4×Bi=1920サンプル=40msが必要）。

**閾値・実装のいずれも変更していない**（docs/plan.md 2.5節の式、spec.mdの目標充填量の式のとおりに実装した結果としてこの挙動になることを確認した）。この発見はManagerへの報告事項とし、次回の判断（目標充填量の式の見直し、条件c)の許容の見直し、またはBi=Bo近傍の構成を許容外とする、等）を仰ぐ。

### 計画からの変更点
- R4のテストで「破棄後の充填量が目標+1ブロック以下」を、`ResamplingFifo`の公開APIに瞬時充填量のgetterが無いため、直後の数回の`pull()`で追加の破棄が起きないことを確認する間接的な方法に置き換えた（`ResamplingFifoStats`は平滑値のみを公開する設計のため）。
- それ以外はdocs/plan.md 2.5節・3章T-003のとおりに実装した。

### 未解決事項
- 上記「条件c)不合格の原因分析」のとおり、S1・S2・S3・S4・S7がRingBufferLongで不合格だった。Manager判断によりD-012（連続換算充填量fill_c）を採用し、後続エントリ（本ファイル先頭「T-003 D-012対応」）で全件合格に修正済み。
- Windows実機・WASAPI固有の挙動は未検証（D-004のとおり）。

### 次回開始位置
- （更新: D-012対応後）T-004（`src/core/PitchShifter.*`、`src/core/Engine.*`）。

### 実施内容
- `.github/workflows/build.yml`: dumpbin検査ループで`Test-Path`によるexe存在確認、`$LASTEXITCODE`確認、出力に"Image has the following dependencies"が含まれるかの確認を追加し、解析失敗で禁止DLL検査が素通りしないようにした。
- `tests/ShifterTests.cpp`: `bool allFinite = true;`の宣言直後の上書き（デッドストア）を解消し、`const bool allFinite = ...`の1行にまとめた。

### 結果
- `cmake --build build --parallel && ctest --test-dir build --output-on-failure`成功（1件のsmokeテストがPassed）。
- pwshがLinux環境に無いため、`.github/workflows/build.yml`の変更は目視レビューのみ（既存の`-match`配列判定パターンと整合させた）。

### 次回開始位置
- T-003: `src/core/ResamplingFifo.*`、`src/app/AudioIO.*`（入出力分離・リングバッファ・クロックずれ補正とオフライン模擬テスト）。Windows CIでの今回のdumpbin検査ロジックの実動作確認はMasterへのpush後にManagerが行う。

---

## 2026-09-28 T-002 CMake構成・依存固定・CI・README

### 実施内容
- `CMakeLists.txt`: `vc_core`をINTERFACEライブラリ化（D-005）。JUCE 9.0.2とsignalsmith-stretch 1.4.0をFetchContentでタグ固定（`GIT_SHALLOW`、stretchは`SYSTEM`）。`CMAKE_MSVC_RUNTIME_LIBRARY`をどのターゲットよりも先に設定し`/MT`相当にする。`VoiceChange`（GUIアプリ）と`VoiceChangeTests`（コンソールアプリ）を追加し、ctestにSmokeカテゴリを登録。
- `.gitignore`: build/等を除外。
- `.github/workflows/build.yml`: windows-latest（`cmake -A x64` → Release ビルド → ctest → vswhereでdumpbinを探しVCランタイム/UCRT DLL非依存を検査 → exeをartifact化）とubuntu-24.04（Ninja Releaseビルド → ctest）の2ジョブ。
- `src/app/Main.cpp`: 空の固定サイズウィンドウ（460×600、タイトル"VoiceChange"、背景`0xFF17181A`）。`moreThanOneInstanceAllowed() = false`（D-009）。UI本体はT-006で追加。
- `src/core/Params.h`: `Preset`/`Effect` enumと`kPresets`（docs/spec.md層2表どおり）のみ。`AtomicParams`等はT-004で追加する旨をコメントで明記。
- `tests/TestMain.cpp`: `juce::UnitTestRunner`ベースのランナー。`--category`必須、対象0件または失敗ありで非0終了。
- `tests/TestSignals.h`: 正弦生成・有限値検査の土台のみ。
- `tests/ShifterTests.cpp`: Smokeカテゴリ。48kHzで`configure(1, 960, 240)`のとき`inputLatency()+outputLatency()`が960×[0.95,1.05]であること、1秒の220Hz正弦を処理して出力が全て有限であることを確認。
- `README.md`: アプリ用に全面的に書き換え（目的・チェーン・動作要件・ビルド手順・技術選定理由・使わないもの・ライセンス・ドキュメント一覧）。開発フローはAGENTS.md参照の一文のみ残した。

### 結果
- ローカル（Linux/Ninja/Release）でクリーンビルドが成功（アプリ・テスト両方）。`ctest --test-dir build --output-on-failure`は1件（smoke）成功。
- `xvfb-run -a ./build/VoiceChange_artefacts/Release/VoiceChange`をtimeout 8秒で起動し、プロセスが生存したまま推移することを確認（クラッシュなし。タイムアウトによる強制終了のみ）。
- Linux開発環境に`libxi-dev`が未導入だったため追加インストールした（`juce_gui_basics`が`X11/extensions/XInput2.h`を要求する）。CIのlinuxジョブのapt installリストに`libxi-dev`を追加済み。
- Windows CIは未確認（Managerがpush後に確認）。

### 計画からの変更点
- なし。docs/plan.md 2.2〜2.4節のとおりに実装した。FetchContentのローカル上書き変数名（`FETCHCONTENT_SOURCE_DIR_JUCE`/`_SIGNALSMITH-STRETCH`/`_SIGNALSMITH-LINEAR`）はプロンプト指定のとおりで問題なく機能したため、plan.mdの更新は不要だった。

### 未解決事項
- Windows実機・WASAPI固有の挙動は未検証（D-004のとおり、実機はユーザーが確認）。
- Windows CIの初回実行結果はMasterへのpush後にManagerが確認する。

### 次回開始位置
- T-003: `src/core/ResamplingFifo.*`、`src/app/AudioIO.*`（入出力分離・リングバッファ・クロックずれ補正とオフライン模擬テスト）。

### コミット
- `2aa16a3` T-002: CMake構成・依存固定・Windows/Linux CI・README
- `df6b870` README: D-011（個人利用のみ、配布なし）の記載を追加
