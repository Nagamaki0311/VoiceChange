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

## 2026-09-28 T-004 D-015追加検証: setFormantBase(真のf0)の効果は否定

### 実施内容
- Manager指示により、P4不合格（D-015）の仮説「Signalsmith Stretchのフォルマント補償が内部の基本周波数推定（`estimateFrequency()`、自動）に依存しており、低いf0でその推定が外れている」を検証した。
- 一時テストファイル`tests/FormantBaseExperimentTemp.cpp`（ctest未登録、検証後に削除済み）を追加し、f0=80/100/150Hz×±5半音の組み合わせで、`stretch.setFormantBase(真のf0)`を与えた場合と与えない場合（既定の自動推定）のf0比・包絡スケールsを比較した。
- 結果は docs/decisions.md D-015に表として記録した。要点: f0比の誤差は`setFormantBase`の有無でほぼ変わらない（ピッチマッピングとフォルマント処理は別経路のため妥当）。包絡スケールsは自動推定の方が良好（150Hzでs=0.995〜1.010とほぼ完璧）で、`setFormantBase`に真のf0を与えるとf0によらずs≈0.75(-5半音)/1.31(+5半音)に張り付き、むしろ悪化した。
- 以上より当初の仮説を却下し、`PitchShifter`への`setFormantBaseHz`等のAPI追加は行わないことにした（T-005でピッチ検出器の結果をシフターへ配線する作業自体はケロケロ機能に必要なため別途実施するが、P4改善目的では行わない）。docs/decisions.md D-015へ実験結果と却下の判断を追記した。

### 結果
- 実装変更はテスト・コア側ともになし（一時実験ファイルは削除済み）。`cmake --build build --parallel && ctest --test-dir build --output-on-failure`は前回セッション（コミット297726e）から変化なし: shifterのみ不合格（P4の2ケース、原因はD-015のとおりライブラリの残存特性で解決せず）。
- 本セッションのコミット（d062d0d）には、前回セッション末に本ファイルへ追記した「コミット: 297726e」の1行（当時は`git add -A`前提の未コミット差分として残していたもの）も合わせて含まれている。以後は各エントリの本文にコミットハッシュを埋め込まず、コミット自体は`git log`で追える情報として扱う。

### 計画からの変更点
- なし。P4の閾値・実装は変更していない。`setFormantBase`案を検討したが効果がないと確認し不採用とした。

### 未解決事項
- D-015: P4（f0=100Hzの母音、±5半音）が引き続き不合格。原因はSignalsmith Stretchの低域ピッチ量子化誤差で、フォルマント補償の内部f0推定は無関係と判明した。T-005着手前に閾値・設計方針の判断が必要。

### 次回開始位置
- Manager判断待ち（D-015）。判断確定後、必要ならPitchShifter/Engine/tests/TestSignals.hへ反映。並行してT-005に着手可能。

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

### コミット
- `297726e` T-004: D-014対応（ピッチシフターのブロック長を120ms固定に変更）

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
