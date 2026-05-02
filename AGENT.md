# DawCast - VST Plugin + 録画アプリ

## プロジェクト概要

DAW のマスターに挿すだけで、DAW の音声＋画面録画を同時に行える VST プラグイン。
BlackHole / Loopback / VB-Cable 等の仮想オーディオデバイスが一切不要。
配信者・DTMer・YouTuber 向けに「DAW に挿すだけで録画できる」革命的な UX を提供する。

**macOS Apple Silicon（M1 以降）専用。**
Windows は WASAPI Loopback / Game Bar で DAW 音声を標準で録画に含められるため、このプラグインの需要は macOS にしかない。
GPU（Metal）を前提とした設計のため Intel Mac は非対応。

## 技術スタック

- **言語**: C++
- **フレームワーク**: JUCE（VST プラグイン + 録画アプリ両方）
- **ビルドシステム**: CMake
- **動画エンコード**: FFmpeg（VideoToolbox H.264/H.265 / libfdk_aac / ProRes）
- **GPU**: Metal（Apple Silicon 内蔵 GPU）
- **画面キャプチャ**: ScreenCaptureKit（macOS 12.3+）
- **ハードウェアエンコード**: VideoToolbox（Apple Silicon Media Engine）
- **プロセス間通信**: WebSocket（ローカル）+ 共有メモリ（リングバッファ）
- **対応OS**: macOS Apple Silicon 専用（M1 / M2 / M3 / M4 以降）
- **最低OS**: macOS 12.3 Monterey
- **プラグインフォーマット**: VST3 / AU（AAX は将来対応可）
- **アーキテクチャ**: arm64 のみ（Universal Binary 不要）

## システム構成（2プロセス構成）

このプロジェクトは **2つのバイナリ** で構成される：

### 1. VST プラグイン（DAW 内で動作）

- DAW のマスターチャンネルに挿入
- `processBlock()` で DAW の最終出力音声をキャプチャ
- プラグイン UI に REC / STOP ボタン
- 録画アプリへの指示（IPC）
- DAW の transport 状態（再生/停止）を監視

### 2. 録画アプリ（別プロセス、JUCE スタンドアロン）

- 画面録画（GPU ネイティブキャプチャ）
- プラグインから音声バッファを受け取る
- FFmpeg で音声＋映像を mux
- 動画ファイル書き出し

```
[DAW]
  └─ VST Plugin
        ├─ UI（REC / STOP）
        ├─ DAW の音声をキャプチャ（WAV / PCM）
        ├─ タイムスタンプ生成
        └─ 録画アプリに指示（IPC: WebSocket）

[録画アプリ（JUCE スタンドアロン）]
  ├─ ScreenCaptureKit で画面録画
  ├─ プラグインから音声バッファ受信（共有メモリ）
  ├─ VideoToolbox でハードウェアエンコード
  ├─ FFmpeg で音声 + 映像を mux
  └─ H.264 / ProRes で書き出し
```

## 重要な設計原則

### なぜ2プロセスか

- VST プラグイン内で画面録画をやると **DAW が落ちる**
- DAW はリアルタイム処理が命。録画のような重い処理をプラグイン内でやるのは厳禁
- 録画アプリが落ちても DAW は無傷
- GPU / FFmpeg / ScreenCaptureKit を安全に使える

### 録画アプリの起動ルール

- プラグインが起動されたら録画アプリを **自動で** バックグラウンド起動する
- ユーザーがプラグインを挿した＝録画の意図がある。準備は自動で行う
- プラグインが閉じられたら録画アプリも終了
- 常駐アプリにはしない（プラグイン不使用時にバックグラウンドプロセスが動くのは不自然）
- ユーザーは録画アプリの存在を意識しない。裏方に徹する

## UX フロー

1. ユーザーが DAW を開く
2. マスターにこの VST プラグインを挿す
3. → 録画アプリがバックグラウンドで自動起動
4. プラグイン UI の **REC** ボタンを押す
5. → DAW の音声キャプチャ＋画面録画が開始
6. **STOP** ボタンを押す
7. → 録画停止、動画ファイル書き出し
8. プラグインを外す → 録画アプリ終了

## プラグイン UI

```
[● REC]   [■ STOP]
Time: 00:03:12
FPS: 60
Audio: 48kHz
```

- REC → 録画アプリに `start` コマンドを送信
- STOP → `stop` コマンドを送信
- Time → プラグイン側で録画時間を計測・表示
- Audio → DAW のサンプルレートを表示

最小限の UI で十分。凝ったデザインは不要。

## 音声キャプチャ（VST プラグイン側）

- `processBlock()` で DAW のマスター出力バッファを取得
- リングバッファに書き込み
- サンプル精度のタイムスタンプを生成
- WAV（PCM）形式で保存、または共有メモリ経由で録画アプリに渡す
- **BlackHole 不要** — DAW 内部の生のオーディオバッファに直接アクセス

## 画面録画（録画アプリ側）

- **ScreenCaptureKit**（macOS 12.3+、Apple 公式の最新 API）
- 60fps 対応
- Apple Silicon の GPU で効率的にキャプチャ
- QuickTime と同じ方式
- `SCStreamConfiguration` で解像度・FPS を設定
- `CMSampleBuffer` でフレームを取得 → Metal テクスチャとして処理可能

## 音声と映像の同期

- プラグイン側で「録音開始タイムスタンプ」を生成
- 録画アプリ側で「録画開始タイムスタンプ」を合わせる
- FFmpeg で mux 時に同期を保証
- DAW のサンプル精度で同期可能（音ズレゼロ）

## IPC（プロセス間通信）

### WebSocket（指示・ステータス用、双方向）

```
プラグイン → 録画アプリ:
  {"cmd": "start", "timestamp": 123456789, "sampleRate": 48000}
  {"cmd": "stop"}

録画アプリ → プラグイン:
  {"status": "recording"}
  {"status": "done", "path": "/video/output.mp4"}
```

### 共有メモリ / リングバッファ（音声データ転送用）

- プラグインが音声バッファを共有メモリに書き込む
- 録画アプリが読み取って WAV に保存
- 高速・低レイテンシ

## 動画出力

### メイン出力: H.264（mp4）

- YouTube / Instagram / TikTok / SNS 向け
- VideoToolbox でハードウェアエンコード（Apple Silicon Media Engine）
- Apple Silicon の専用エンコーダで CPU 負荷ほぼゼロ
- ファイルサイズが小さい

### オプション出力: ProRes（mov）

- 編集用（Premiere / Final Cut / DaVinci Resolve）
- 高画質・低圧縮
- 編集時のデコードが軽い

### 音声コーデック

- AAC（libfdk_aac）: SNS 向け標準
- WAV（PCM）: 完全無劣化で中間保存
- MP3（libmp3lame）: 互換性重視の場合

## 録画アプリの起動方法

- JUCE の `ChildProcess` を使用
- `.app` を `open` コマンドまたは `NSWorkspace` で起動
- プラグインの `prepareToPlay()` または初期化時に起動

## モジュール構成

### VST プラグイン

1. **PluginProcessor** - `processBlock()` で音声キャプチャ、transport 監視
2. **PluginEditor** - REC / STOP UI、録画状態表示
3. **AudioRingBuffer** - 音声データの共有メモリ書き込み
4. **IPCClient** - 録画アプリへの WebSocket 通信
5. **RecorderAppLauncher** - 録画アプリの起動・終了管理

### 録画アプリ

1. **ScreenRecorder** - ScreenCaptureKit による画面録画
2. **AudioReceiver** - 共有メモリから音声データ受信
3. **FFmpegMuxer** - 音声 + 映像の mux、エンコード
4. **IPCServer** - プラグインからの指示受信
5. **OutputManager** - 動画ファイル書き出し管理

## macOS / Apple Silicon 固有の注意点

- Screen Recording Permission が必要
- Info.plist に `NSScreenCaptureUsageDescription` を設定
- ScreenCaptureKit は macOS 12.3（Monterey）以降
- VideoToolbox で Apple Silicon Media Engine を使ったハードウェアエンコード
- arm64 ビルドのみ（`CMAKE_OSX_ARCHITECTURES=arm64`）
- Hardened Runtime + Notarization が配布時に必要
- App Sandbox は録画アプリ側で無効（ScreenCaptureKit の制約）

## ビルド手順

```bash
cmake -B build -S .
cmake --build build --config Release
```

ビルド成果物:

- VST3 プラグイン（`.vst3`）
- AU プラグイン（`.component`）
- 録画アプリ（`.app`）

すべて arm64（Apple Silicon）ビルド。

## コーディング規約

- JUCE の API・スタイルは MCP サーバー（`mcp_juce-docs`）経由で常に最新ドキュメントを参照して正しいコマンド・クラス名を確認する
- クラス名: PascalCase
- メソッド名: camelCase
- メンバ変数: camelCase
- macOS 専用のため OS 分岐は不要
- Objective-C++ (`.mm`) で Apple API を直接呼び出し可能
- リアルタイムスレッド（`processBlock`）ではメモリ確保・ロック・IO 禁止
- エラーハンドリングは `juce::Result` を使用

## QuickTime との比較（本プロジェクトの優位性）

| 機能           | QuickTime          | 本プロジェクト         |
| -------------- | ------------------ | ---------------------- |
| 画面録画       | ◎ GPU キャプチャ   | ◎ 同じ方式             |
| DAW の音声     | ❌ 直接録れない    | ◎ VST で直接キャプチャ |
| 音ズレ         | △ 起きることがある | ◎ サンプル精度で同期   |
| BlackHole 必須 | 必須               | 不要                   |
| DAW 内で完結   | ❌                 | ◎                      |

## なぜ macOS 専用か

- **Windows は問題が存在しない**: WASAPI Loopback / Game Bar で DAW 音声を標準で録画に含められる
- **macOS だけが BlackHole 等の仮想デバイスを必要とする**: CoreAudio にループバックキャプチャ機構がない
- **Apple Silicon の GPU / Media Engine をフル活用**: VideoToolbox + ScreenCaptureKit + Metal で CPU 負荷を最小化
- **Intel Mac 非対応の理由**: Metal Performance Shaders / Media Engine の効率が Apple Silicon と段違い。ターゲットを絞ることで最適化に集中できる

## 将来の拡張

- DAW transport 連動のオート REC モード（再生開始で自動録画）
- プラグイン UI のオーバーレイ表示（波形、パラメータ操作のキャプチャ）
- SNS 用の縦動画自動変換
- 自動ノーマライズ
- AU / AAX フォーマット対応

## 実装 TODO（現在の進捗）

ビルド骨格は完成済み。以下が未実装。

### 優先度 高

#### 1. ✅ PluginProcessor — 音声キャプチャ（実装済み）

- `prepareToPlay()` で `ringBuffer.initSharedMemory()` → `launcher.launch()` → `ipcClient.connect()` を順に実行
- `processBlock()` — `recordingActive` フラグが立っているときのみ `ringBuffer.write()` を呼ぶ（リアルタイムスレッド安全）
- `startRecording(StartParams)` — IPC 未接続なら再接続を試みてから `ipcClient.sendStart()` を送信
- `stopRecording()` — `ipcClient.sendStop()` を送信してフラグを落とす
- デストラクタで録画中なら stop を送信、IPC 切断、録画アプリを終了
- `ipcClient.setStatusCallback()` で録画アプリからの応答（`"done"` など）を受けて `recordingActive` を同期

#### 2. ✅ PluginEditor — REC / STOP ボタン接続（実装済み）

- `juce::Timer`（200ms）で timeLabel / statusLabel を更新、ボタン有効 / 無効を制御
- `recButton.onClick` → `processorRef.startRecording(params)` を呼ぶ
- `stopButton.onClick` → `processorRef.stopRecording()` を呼ぶ
- `captureModeBox`（ComboBox）で "Full Screen" / "DAW Window" / "Custom Region" を選択可能
  - 選択値が `StartParams.captureMode`（`"display"` / `"application"` / `"region"`）に変換されて送信される
- `formatElapsed()` で録画経過時間を `HH:MM:SS` 形式で表示
- statusLabel に DAW のサンプルレートと IPC 接続状態を表示

#### 3. ✅ IPC（JUCE InterprocessConnection）実装済み

- **ライブラリ**: `juce::InterprocessConnection` / `juce::InterprocessConnectionServer`（JUCE 標準、外部依存なし）
  - JUCE 8 に WebSocket クラスは存在しないため、JUCE ネイティブの TCP IPC を採用
  - JSON を `juce::MemoryBlock` に入れて送受信（JUCE の独自フレーミング）
  - **重要**: クライアントとサーバーのどちらも JUCE IPC を使う必要がある（生 TCP ではフレームが合わない）

- **IPCClient** (`Source/DSP/IPCClient.h/.cpp`) — プラグイン側
  - `connect()` — `127.0.0.1:9527` に接続（タイムアウト 2 秒）
  - `sendStart(StartParams)` — start コマンドを JSON 送信
  - `sendStop()` — stop コマンドを JSON 送信
  - `setStatusCallback(fn)` — 録画アプリからの応答を受け取るコールバック

- **IPCServer** (`Source/Recorder/IPCServer.h/.cpp`) — 録画アプリ側
  - `start(9527)` — `127.0.0.1:9527` で待ち受け開始
  - `setCommandCallback(fn)` — 受信コマンドを渡すコールバック
  - `sendStatus(json)` — プラグインへステータスを返信

- **JSON プロトコル（確定版）**:

  ```
  Plugin → Recorder:
    {"cmd":"start","timestamp":ms,"sampleRate":48000,"captureMode":"display"}
    {"cmd":"start","timestamp":ms,"sampleRate":48000,"captureMode":"application","applicationBundleId":"com.ableton.live"}
    {"cmd":"start",...,"captureMode":"region","regionX":0,"regionY":0,"regionWidth":1080,"regionHeight":1920}
    {"cmd":"stop"}

  Recorder → Plugin:
    {"status":"recording"}
    {"status":"done","path":"/Users/.../Movies/DawCast/DawCast_2026-05-01_143022.mp4"}
  ```

#### 4. ✅ 共有メモリ（音声データ転送）実装済み

- **共有レイアウト定義**: `Source/SharedAudio.h` — `DawCastShm::Layout` struct
  - `writePos`（`std::atomic<int>`）を単独キャッシュライン（64B）に隔離
  - `samples[2][65536]`（~512KB）
  - **設計**: writePos のみ共有。readPos は各プロセスがローカルに保持（shm は PROT_READ で十分）

- **AudioRingBuffer** (`Source/DSP/AudioRingBuffer.h/.cpp`) — プラグイン側
  - コンストラクタでローカルバッファを確保（テスト用フォールバック）
  - `initSharedMemory()` — `shm_open(O_CREAT|O_RDWR)` + `ftruncate` + `mmap` + placement-new で初期化
  - デストラクタで `munmap` + `shm_unlink`（作成者がリンクを解除）
  - `write()` — shm または localLayout に書き込む（リアルタイムスレッドセーフ）
  - `read()` — ローカル readPos で読み出す（テスト用）

- **AudioReceiver** (`Source/Recorder/AudioReceiver.cpp`) — 録画アプリ側
  - `open()` — `shm_open(O_RDONLY)` + `mmap(PROT_READ)` で読み取り専用マップ
  - `read()` — ローカル readPos を進めながら `samples[][]` を読み出す
  - `open()` 時点の writePos から読み始める（録画開始前のデータをスキップ）
  - `close()` — `munmap` + `close`（unlink しない、プラグインが管理）

- **共有メモリ名**: `"dawcast_audio_shm"` (`DawCastShm::kName`)
- **サイズ**: `sizeof(DawCastShm::Layout)` ≈ 512KB

#### 5. ✅ ScreenCaptureKit 画面録画（実装済み）

- `Source/Recorder/ScreenRecorder.mm` — `SCShareableContent` 取得 → `SCStream` 起動実装済み
- `DawCastStreamOutput`（`SCStreamOutput` デリゲート）で `CMSampleBuffer` を受け取り `FrameCallback` に流す
- `DawCastStreamDelegate`（`SCStreamDelegate`）でストリームエラーを監視
- 解像度・FPS は `SCStreamConfiguration` で設定（デフォルト: ディスプレイ物理解像度 / 60fps）
- `ScreenRecorder.mm` は `-fobjc-arc` で ARC 有効（CMakeLists.txt で per-file 設定済み）
- `Info.plist` に `NSScreenCaptureUsageDescription` 追加済み（CMake `PLIST_TO_MERGE` 経由）
- `LSUIElement=true`（Dock アイコン非表示）設定済み
- **注意**: `startRecording()` は非同期。`isRecording()` が `true` になれば開始完了

#### 5b. ✅ キャプチャモード選択（実装済み）

`CaptureMode` enum（`ScreenRecorder.h`）で 3 種類のモードを定義済み。IPC の `start` コマンドに `"captureMode"` フィールドとして含める。

| モード               | enum            | IPC フィールド値 | 実装状況                 |
| -------------------- | --------------- | ---------------- | ------------------------ |
| ディスプレイ全体     | `EntireDisplay` | `"display"`      | ✅ 実装済み              |
| DAW アプリウィンドウ | `Application`   | `"application"`  | ✅ 実装済み              |
| カスタム矩形領域     | `CustomRegion`  | `"region"`       | ⬜ UI 未実装（下記参照） |

**Application モード実装詳細**:

- `SCContentFilter(display:includingApplications:exceptingWindows:)` で DAW アプリの全ウィンドウをキャプチャ
- `ApplicationTarget.bundleID`（優先）または `.pid` でアプリを特定
- VST3/AU プラグインは通常 DAW プロセス内で UI を描画するため、DAW の bundleID 指定で**プラグインウィンドウも含まれる**
- 主要 DAW bundleID 例: Ableton Live = `"com.ableton.live"`, Logic Pro = `"com.apple.logic10"`

**CustomRegion モード（UI 選択は TODO #5c 参照）**:

- SCK の `SCStreamConfiguration.sourceRect`（macOS 14.2+）でトリミング実装済み
- macOS 14.2 未満は自動的に EntireDisplay にフォールバック
- **UI 選択フロー（ユーザーが矩形を手動指定）は未実装**

#### 5c. ✅ カスタム矩形選択 UI（実装済み）

- **`Source/Recorder/RegionSelectorWindow.h/.mm`** — 全画面半透明オーバーレイ NSWindow
  - `NSWindowStyleMaskBorderless` / `backgroundColor = clear` / `level = NSScreenSaverWindowLevel`（最前面）
  - `DawCastSelectionView`（`isFlipped=YES`）— 左上原点 pt 座標（SCK sourceRect と同一系）
  - マウスドラッグで矩形選択、選択エリアを punch-through で可視化、サイズ表示ラベル付き
  - ESC キーまたは最小サイズ未満でキャンセル → コールバックに `(0,0,0,0)` を渡す
  - 選択座標は H.264 制約（偶数）にアラインメント後、`juce::MessageManager::callAsync` でメッセージスレッドへデリゲート
- **`RecorderApp::startCapture()`** を非同期フローに分割:
  - `captureMode=="region"` → `RegionSelectorWindow::show(callback)` を呼んで即リターン（非同期）
  - 選択完了コールバック → `beginCapture(json, rx, ry, rw, rh)`
  - キャンセルコールバック → IPC エラー送信
  - `display` / `application` → `beginCapture(json, 0,0,0,0)` を直接呼ぶ（同期）
- **`RecorderApp::beginCapture()`** — 実際の録画開始処理（FFmpegMuxer::open → AudioReceiver → AudioPumpThread → ScreenRecorder::startRecording）
  - region モード時は選択サイズを muxer の出力解像度にも使用（sws_scale の入出力を一致させる）

#### 6. ✅ FFmpeg mux（実装済み）

- `Source/Recorder/FFmpegMuxer.cpp` — `open()` / `writeVideoFrame()` / `writeAudioSamples()` / `close()` を実装済み
- **ビデオ**:
  - 入力: BGRA（ScreenCaptureKit の `CVPixelBuffer` と一致）
  - `sws_scale` で BGRA → yuv420p（H.264）または yuv422p10le（ProRes）に変換
  - H.264 エンコーダ: `h264_videotoolbox`（VideoToolbox HW）→ `libx264`（SW）の順でフォールバック
  - ProRes エンコーダ: `prores_videotoolbox`（HW）→ `prores_ks`（SW）の順でフォールバック
- **オーディオ**:
  - 入力: `juce::AudioBuffer<float>`（float planar、JUCE の `processBlock` 形式）
  - `swr_convert` で float planar → AAC エンコーダのサンプルフォーマットに変換
  - `AVAudioFifo` でサイズ可変入力を frame_size（1024 samples）単位に整列
  - エンコーダ: `libfdk_aac`（高品質）→ 組み込み `aac` の順でフォールバック
  - ビットレート: 192 kbps
- **同期**: PTS はフレーム/サンプル単位でインクリメント（`timestampSeconds` は今後 TODO #10 で A/V 同期に利用）
- **`close()`**: FIFO に残った音声を最終フレームとしてフラッシュ → エンコーダドレイン → `av_write_trailer`
- CMakeLists.txt に `libswscale.dylib` リンクを追加済み

### 優先度 中

#### 7. ✅ RecorderApp 統合（実装済み）

- `Source/Recorder/RecorderApp.h` — 全コンポーネントを `std::unique_ptr` で保持（forward declaration パターン）
- `Source/Recorder/RecorderApp.cpp` — `initialise()` / `shutdown()` / `startCapture()` / `stopCapture()` を実装済み

**データフロー**:

```
IPC "start" → startCapture()
  ├─ FFmpegMuxer::open()       (出力ファイル確保)
  ├─ AudioReceiver::open()     (共有メモリ接続)
  ├─ AudioPumpThread::start()  (5ms ポーリングで AudioReceiver→FFmpegMuxer に流す)
  └─ ScreenRecorder::startRecording()
       └─ frameCallback → CVPixelBufferLock → FFmpegMuxer::writeVideoFrame()

IPC "stop" → stopCapture()
  ├─ ScreenRecorder::stopRecording()   (フレーム callback 停止)
  ├─ AudioPumpThread::stopThread(2s)   (残余音声を書き終えるまで待機)
  ├─ AudioReceiver::close()
  ├─ FFmpegMuxer::close()             (FIFO フラッシュ + av_write_trailer)
  └─ IPCServer::sendStatus({"status":"done","path":"..."})
```

**スレッド設計**:

- IPC 受信スレッド → `MessageManager::callAsync` でメッセージスレッドへデリゲート（ロックフリー）
- `AudioPumpThread`（専用 `juce::Thread`）— `AudioReceiver::read()` → `FFmpegMuxer::writeAudioSamples()`
- ScreenCaptureKit キャプチャスレッド → `frameCallback` → `FFmpegMuxer::writeVideoFrame()`
- `muxerLock`（`juce::CriticalSection`）で音声ポンプとフレーム callback の競合を排除

**出力設定**: H.264 / 1920×1080 / 60fps / AAC 192kbps（ScreenRecorder に同じ解像度を渡すことで sws_scale の入出力サイズを一致させる）

#### 8. ✅ パッケージング（実装済み）

- `DawCastRecorder.app` を VST3 バンドル内（`DawCast.vst3/Contents/Resources/`）に同梱
- **CMakeLists.txt**: `add_dependencies(DawCastPlugin_VST3 DawCastRecorder)` + `add_custom_command(POST_BUILD)` で
  - ビルドアーティファクト (`build/DawCastPlugin_artefacts/Debug/VST3/DawCast.vst3/Contents/Resources/`) へコピー
  - インストール先 (`~/Library/Audio/Plug-Ins/VST3/DawCast.vst3/Contents/Resources/`) へもコピー（JUCE の copyDir 後に実行）
- **RecorderAppLauncher.cpp `findRecorderApp()`** — 優先: `CFBundleGetBundleWithIdentifier("com.auditive.dawcast")` でプラグインバンドルを特定 → `Contents/Resources/DawCastRecorder.app` を返す。フォールバック: `currentExecutableFile/../Resources/DawCastRecorder.app`
- **launch()** — `open --background /path/to/DawCastRecorder.app`（`.app` バンドルのパスを直接渡す形式に修正）

### 優先度 低（配布前）

#### 9. ✅ Hardened Runtime（実装済み）

- `HARDENED_RUNTIME_ENABLED TRUE`（DawCastPlugin / DawCastRecorder 両方に設定）
- `HARDENED_RUNTIME_OPTIONS "com.apple.security.device.screen-capture"` entitlement を追加（`flags=0x10002(adhoc,runtime)` で確認済み）
- App Sandbox は録画アプリ側で無効（デフォルト無効、明示的設定不要）
- FFmpeg dylib を `DawCastRecorder.app/Contents/Frameworks/` に埋め込み（`cmake/embed_ffmpeg.sh` + POST_BUILD）
  - `otool -L` で参照 dylib を動的検出、`install_name_tool` で `@executable_path/../Frameworks/` に書き換え
  - POST_BUILD 末尾に `codesign --options runtime --entitlements ... --deep` で再署名して Hardened Runtime を保持

#### 10. 音声・映像の同期検証

- start コマンドに含まれる `timestamp`（ホスト時刻）で A/V 同期オフセットを計算
- `FFmpegMuxer` の PTS 設定に反映
