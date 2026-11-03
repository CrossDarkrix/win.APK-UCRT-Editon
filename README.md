# Win.apk (UCRT Edition)

**AndroidアプリをWindows 10 / 11（x64）上で、通常のWindowsアプリのように実行するためのWin.apk UCRTカスタムエディションです。**

> [!IMPORTANT]
> Win.apk (UCRT Edition)は、MSYS2 UCRT64環境に統一し、Windows上での安定動作、起動時間、マルチメディア、ストレージ連携、実機互換性を重視して調整したカスタムビルドです。上流Win.apkとの互換性を保ちながら、UCRT向けのネイティブブリッジとWindows統合を追加しています。

---
[English](README_EN.md)

---

## 概要

Win.apkは、`.apk`、`.apkm`、`.xapk`、`.apks`をインストールし、Androidアプリを個別のWindowsウィンドウで実行します。仮想マシンや一般的なAndroidエミュレーターを常駐させる方式ではなく、JavaコードはJava VM上で動作し、x86_64ネイティブライブラリはプロセスへ直接ロードされます。arm64-v8aライブラリにはx64向け変換経路があります。

UCRT Editionでは、WindowsのUniversal C Runtimeを利用するUCRT64ツールチェーンへネイティブ依存関係を揃え、異なるABIやランタイムの混在を避ける構成にしています。

## UCRT Editionの主な特徴

### UCRT64向け安定化

- MSYS2 **UCRT64**を正式なビルド環境として使用
- UCRT64版GStreamer、プラグイン、スキャナー、ランタイムDLLを同一ABIで配置
- MINGW64、CLANG64など、異なる環境の成果物を混在させない構成
- ネイティブブリッジ、JNI、Windowsホスト処理を含めたUCRT64一貫ビルド

### GStreamerによる描画・メディア処理

- GStreamerの`appsrc`、D3D11アップロード、`d3d11videosink`を使用する非同期描画経路
- GStreamer MediaExtractor PCMブリッジによる音声デコード経路
- GStreamerレジストリキャッシュを最終配置先で事前生成
- GStreamer初期化を最初のフレーム描画後に遅延し、起動時の白画面時間を短縮
- GStreamerが使用できない場合はWin32描画経路へフォールバック
- 全画面サイズプリセットで、縦横ともウィンドウのクライアント領域へストレッチフィット

グラフィックAPIについては、Android/OpenGL ES描画をANGLE経由でDirect3D 11へ接続し、最終フレームのWindows表示にはGStreamer D3D11プレゼンターまたはWin32フォールバックを使用します。

### Windowsオーディオ統合

- Windowsの既定出力デバイスとアクティブなオーディオエンドポイントの列挙
- Android側からの出力デバイス選択
- `AudioTrack.setPreferredDevice()`相当の出力先切り替え
- Windowsセッション音量との同期
- AudioTrack、SoundPool、MediaPlayer向けのWindows音声経路

### 音楽CD・光学ドライブ対応

- Windowsの光学ドライブをAndroid側へ公開するUCRT CDブリッジ
- ドライブ列挙、オープン、TEST UNIT READY、INQUIRYに対応
- READ TOCによるトラック情報取得
- READ CDによるCD-DAセクター読み取り
- 大きな読み取り要求を安全な単位へ分割するチャンク読み取り
- Windows SCSI pass-throughを使用したコマンド転送

光学ドライブやメディアの状態、ドライブ側の対応状況によって、利用可能な操作は異なります。

### Windowsユーザーフォルダ連携

Android側の共有ストレージから、Windowsの既知のユーザーフォルダへアクセスできるようにマッピングします。

- Downloads
- Pictures
- Documents
- Music
- Videos（Android側ではMovies）

Windowsのファイルピッカー、URI許可、共有ストレージ層も利用できるため、対応アプリではWindows上のファイルを開く、保存する、読み込むといった操作が可能です。ネットワーク共有や異なるボリュームでは、Windows側の制約に応じて通常フォルダへフォールバックする場合があります。

### Windowsとの統合

- APK、APKM、XAPK、APKSのインストール
- アプリごとのウィンドウ、アイコン、スタートメニューショートカット
- Windowsクリップボード
- Windowsファイルピッカー
- ゲームコントローラー
- カメラ、マイク入力、写真ライブラリのホストブリッジ
- WebView2ベースのWebView / ブラウザー連携
- ライブ実行ログとインストールログ
- 日本語、英語、ヘブライ語と右から左のUI
- ライトテーマ、ダークテーマ

## 画面サイズと表示

ランチャーの画面サイズ設定には、1270 x 720のFit screenと複数の端末プリセットがあります。UCRT Editionでは、すべてのプリセットでアスペクト比を固定せず、ウィンドウの縦横全体へ描画をフィットさせます。

- 左右・上下の黒帯を作らず、クライアント領域全体へ描画
- ウィンドウのリサイズと最大化へ追従
- マウス・タッチ座標と子ウィンドウも同じ全面矩形を使用
- プリセット解像度はAndroidアプリが認識する内部描画解像度として維持

## 対応状況

### 対応している主な機能

- Java / Kotlin Androidアプリ
- x86_64ネイティブライブラリ
- arm64-v8aネイティブライブラリのx64変換経路
- JavaおよびネイティブコードからのOpenGL ES
- AudioTrack、SoundPool、MediaPlayer系の音声
- ファイル、SharedPreferences、SQLite、クリップボード
- ファイルピッカー、ゲームコントローラー
- Windowsユーザーフォルダ
- 音楽CD読み取り

### 制限事項

- 32ビットARM専用アプリ（armeabi-v7a）は未対応
- Google Play開発者サービスを前提とする機能は、アプリによって動作しない場合があります
- Android実機固有のセンサー、DRM、認証、ハードウェア機能は完全互換ではありません
- 本プロジェクトは開発中であり、アプリごとに互換性が異なります

## 必要環境

- Windows 10またはWindows 11、64ビット（x64）
- Direct3D 11対応GPUを推奨
- アプリ本体、Javaランタイム、GStreamer、ANGLE、インストールするアプリを格納できる空き容量

## インストールと使い方

1. リリースのインストーラー、またはポータブルZIPを入手します。
2. インストーラー版はセットアップを実行します。ポータブル版は任意のフォルダへ展開します。
3. `Win.apk.exe`を起動します。
4. APKまたは対応するAPKバンドルをウィンドウへドロップするか、追加ボタンから選択します。
5. インストール完了後、ライブラリのタイルからアプリを開きます。

初回インストールではAndroidコードの変換や準備に時間がかかる場合があります。アプリの起動や動作に問題がある場合は、ランチャーのログビューアーで`last-run.log`とインストールログを確認してください。

## データの保存先

| 内容 | 保存先 |
|---|---|
| Win.apk本体 | `%LOCALAPPDATA%\Programs\Win.apk`、またはポータブル版の展開先 |
| インストール済みAPK | `%LOCALAPPDATA%\Win.apk\library` |
| 変換済みコードとアプリ情報 | `%LOCALAPPDATA%\Win.apk\apps\<app>` |
| Androidアプリの保存データ | `%LOCALAPPDATA%\Win.apk\android` |
| ログ | `%LOCALAPPDATA%\Win.apk\logs` |
| ストア情報 | `%LOCALAPPDATA%\Win.apk\store` |
| 設定 | `HKEY_CURRENT_USER\Software\Win.apk` |

アプリを削除しても保存データを保持する操作があります。完全に削除する場合は、対象データの場所を確認してから削除してください。

## ソースからのビルド

### 前提条件

- MSYS2
- **MSYS2 UCRT64**シェル
- Git、Make、CMake、Ninja
- UCRT64 GCC / G++ツールチェーン
- UCRT64 Clang
- JDK
- UCRT64版GStreamer開発環境

### 基本ビルド

```bash
./build.sh
```

実行権限が失われている場合は次を使用します。

```bash
bash build.sh
```

`MSYSTEM`は必ず`UCRT64`であることを確認してください。

```bash
printf '%s\n' "$MSYSTEM"
```

CLANG64、MINGW64など別環境で作成したビルドディレクトリやDLLを再利用しないでください。ABIの異なる成果物の混在は、起動失敗、プラグイン読み込み失敗、音声・描画ブリッジの不整合につながります。

## 主な環境変数

| 変数 | 内容 |
|---|---|
| `WINAPK_PHONE_SIZE=1270x720` | Androidアプリが認識する画面サイズ |
| `MACAPK_LETTERBOX=0` | アスペクト比固定を解除し、ウィンドウ全面へストレッチ |
| `MACAPK_GSTREAMER_PRESENT=0` | GStreamerプレゼンターを無効化し、Win32経路を使用 |
| `GST_PLUGIN_PATH` | GStreamerプラグインの探索先 |
| `GST_PLUGIN_SYSTEM_PATH` | システムプラグイン探索先 |
| `GST_PLUGIN_SCANNER` | GStreamerプラグインスキャナー |
| `GST_REGISTRY` | 事前生成したGStreamerレジストリキャッシュ |
| `WINAPK_HEADLESS=1` | ウィンドウなしでテスト実行 |
| `WINAPK_HEADLESS_FRAME=frame.bmp` | 最終描画フレームを保存 |

## リポジトリ構成

| パス | 内容 |
|---|---|
| `windows/` | Windowsホスト、JNI、ランチャー、パッケージ、テスト |
| `windows/host/` | GStreamer、音声、CD、ユーザーフォルダ、入力などのWindowsブリッジ |
| `runtime/` | Android Javaフレームワークと共通ネイティブコード |
| `docs/` | 実装資料、検証記録、画像 |
| `.github/workflows/` | Windowsビルド、互換性検査、リリース処理 |

## クレジット

Win.apkは、Kaleb Simpson / VoltareによるMac.apkの移植を起点とし、Androidランタイムの一部を同プロジェクトから継承しています。本エディションはMac.apkプロジェクトから承認または推奨された製品ではありません。

主な外部コンポーネントには、ANGLE、Eclipse Temurin、GStreamerなどがあります。詳細は`THIRD_PARTY_NOTICES.txt`を確認してください。

## ライセンス

[AGPL-3.0](LICENSE)
