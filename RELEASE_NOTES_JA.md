# Win.apk 統合リリース情報

このリリースでは、Windows上でのAPK実行体験を中心に、ランチャーUI、描画、音声・メディア、USB Direct、CDアクセス、アプリ互換性、ローカライズ、配布基盤をまとめて改善しました。個別フェーズで積み上げてきた修正を、一つのリリース情報として整理しています。

## 主なハイライト

### 同梱Java runtimeと配布基盤

- JDK 21環境を対象に、事前検査、`jdeps`モジュール解析、`jlink` runtime生成、配布ステージ統合までを段階化しました。
- 必要モジュールだけを含むカスタムruntimeを生成し、外部Java環境への依存を減らせる構成にしました。
- Win.apkは明示指定された`--jdk`を尊重しつつ、配布ルートの`runtime`、従来の`jre`、`WINAPK_JAVA_HOME`、`JAVA_HOME`の順にJava環境を探索します。
- 実際に読み込んだ`jvm.dll`のパス、`JNI_CreateJavaVM`の解決・生成結果をログで確認できるようにしました。
- runtimeのファイル集合、サイズ、SHA-256、モジュール一覧を検証し、安全に配布ステージへ統合する仕組みを追加しました。

### ランチャーとWindows統合

- Direct2DベースのUIを段階的に改善し、描画品質、レイアウト、スクロール、操作感を調整しました。
- AppsFolderとGet-StartAppsを利用したアプリ列挙を強化し、AUMID取得、Shellアイコン抽出、非同期アイコン処理、キャッシュ、進捗出力抑制を実装しました。
- 起動・終了状態の扱いを見直し、正常終了を起動失敗として扱わないよう修正しました。
- Windows標準の権限・関連付け・起動経路との互換性を改善しました。

### 日本語対応とUI

- ランチャー言語として日本語を追加し、Windows既定、英語、ヘブライ語、日本語を選択・保持できるようにしました。
- ライブラリ、設定、インストール、更新、削除、状態表示、ログなど主要画面の日本語化を進めました。
- 日本語の直接検索、ライブ切り替え、再描画、アクティベーションに関する問題を修正しました。
- 翻訳が存在しない項目は英語へ安全にフォールバックします。

### 表示・描画・フレームペーシング

- 複数の端末プリセットで、クライアント領域全体へ合わせるストレッチ表示を導入しました。
- 1270×720を含む表示フィット、全幅・全高ストレッチ、ウィンドウサイズ追従を改善しました。
- Direct2D描画経路を最適化し、最終的なUI磨き込みと描画安定化を実施しました。
- 60/120Hz環境を考慮したフレームペーシングを導入し、累積Sleepドリフトを避けるdeadline方式へ移行しました。
- `Display.getRefreshRate()`、Choreographer、ネイティブ描画周期の目標値を統一しました。

### 音声・メディア

- オーディオエンジンの最終フレーム排出とstall recoveryを改善し、終端付近の音切れや停止からの回復を強化しました。
- MediaPlayerのリピート、選択出力、再生状態遷移に関する問題を修正しました。
- GStreamerの起動キャッシュと遅延初期化を追加し、起動時負荷を抑えました。
- MediaExtractorの`content://`入力対応とPCM bridgeを追加しました。
- Windows側の物理出力選択、出力検証、音量制御を改善しました。

### USB Direct

- 仮想Isochronous URBからPCMへ橋渡しするUSB Direct経路を実装しました。
- USB DACのサンプルレート切り替え、alternate setting、接続単位セッション、URB pool、submit/reap識別、epoch管理を段階的に強化しました。
- デジタルゲインとソフトリミッターを追加し、実用音量を確保しながらクリッピングを抑制しました。
- URBのpending ownership、generation、submit ID、stale completion隔離、重複ポインタ回復を導入しました。
- DISCARD対象のcompletionをキャンセル完了として保持し、REAP可能にすることで、96 kHz→44.1 kHz→96 kHzの切り替え後も再生を継続できるようにしました。
- ioctl成功時のerrno衛生、unknown URB回復、soft boundary、track boundary処理を改善しました。

### CD・光学ドライブ

- SetupAPIによるCD-ROM列挙を主経路とし、ドライブレター経路をfallbackとして追加しました。
- 列挙結果の重複排除、JNI count/fill競合対策、bounded retry、非同期worker、キャッシュ返却を実装しました。
- SCSI/BOT経路を通じたドライブ照会、TOC取得、CDセクター読み出し、イジェクト処理を強化しました。
- CDアクセスとUSB DACのライフサイクルを分離し、片方のリセットや切断がもう片方へ影響しにくい構成にしました。
- 権限ルートと光学ドライブ列挙の重複処理を整理しました。

### アプリ互換性とランタイム

- Java互換層、JNI、ネイティブライブラリ読み込み、アクティビティ・ウィンドウ・ライフサイクル周辺を継続的に改善しました。
- アプリ固有の起動条件、言語切替、画面フィット、メディア、ストレージ、ネットワーク関連の互換修正を追加しました。
- UCRTビルドのcmath、thread header、言語適用処理など、ビルド障害を引き起こす問題を修正しました。

## 安定性と診断

- 重要な初期化・列挙・ロード・再生経路へ診断ログを追加しました。
- 非同期処理にはtimeout、単一worker、generation、identity gateなどを導入し、競合状態を抑制しました。
- USB Directではsubmit、reap、discard、reset、rate changeを追跡できるようにしました。
- Java runtimeでは選択したJava home、実際の`jvm.dll`パス、JVM生成結果を確認できます。

## 配布時の構成

推奨配布構成は次のとおりです。

```text
Win.apk-UCRT/
├─ bin/
│  └─ Win.apk.exe
├─ lib/
├─ runtime/
│  ├─ bin/
│  │  ├─ java.exe
│  │  ├─ javaw.exe
│  │  └─ server/
│  │     └─ jvm.dll
│  ├─ conf/
│  ├─ legal/
│  ├─ lib/
│  │  └─ modules
│  └─ release
└─ manifests/
```

## 更新時の注意

- Java JAR、patch class、使用JDKを変更した場合は、事前検査、`jdeps`解析、`jlink`生成、runtime統合を再実行してください。
- C++側だけを更新し、Java成果物とJDKに変更がない場合は、既存のjlink runtimeを再利用して配布ステージ統合のみを再実行できます。
- 同梱runtimeの確認時は、外部の`JAVA_HOME`と`WINAPK_JAVA_HOME`を外し、ログに`bundled runtime`と配布ルート配下の`runtime\bin\server\jvm.dll`が表示されることを確認してください。
- NSISへ渡すのは、runtime統合とmanifest生成が完了した`Win.apk-UCRT`配布ステージ全体です。

## 既知の制約

- Windows向けの同梱runtimeは、ビルドしたWin.apkと同じアーキテクチャのJDKから生成する必要があります。
- `--jdk`を明示した場合は、その指定が同梱runtimeより優先されます。
- 外部サービス、GPU、オーディオデバイス、USB DAC、光学ドライブを利用する機能は、対象環境やデバイスに依存します。

