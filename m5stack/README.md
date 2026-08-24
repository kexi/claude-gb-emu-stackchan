# M5Stack CoreS3 / Stack-chan port

共有 C++ コアを M5Stack CoreS3 上で動かす Game Boy / Game Boy Color エミュレータです。

## 内蔵ROMと追加ROM

電源投入時はSDを初期化せず、ファームウェアに埋め込んだKANTAN GB PLAYを直接起動します。`just build` は固定コミットのROMを取得し、`m5stack/rom.lock` のSHA-256と一致した場合だけ埋め込みます。取得したROMはGit管理外です。

追加ROMを使う場合はFAT32 microSDの `/roms` に4 MiB以下の `.gb` または `.gbc` を置き、BtnCを長押しします。この時点で初めてSDを初期化し、先頭の内蔵KANTAN GBとSD上のROMを一覧表示します。タップ、またはGroveコントローラーの上下 + A/Startで選択します。ROMとSRAMはPSRAMに単一所有で確保し、読み込み時の全体コピーを避けます。

バッテリー対応カートリッジの SRAM は同じディレクトリの `<ROM名>.sav` に保存されます。書き込みは 5 秒間更新が止まった時点とメニューへ戻る時に行います。途中ファイルを `.part`、直前の正常な保存を `.bak` に分け、起動時に中断された置換を復旧します。

## 操作

| 入力 | Game Boy |
| --- | --- |
| PORT.B Joystick / Joystick2 | 十字キー、押し込み=Start |
| PORT.C Dual Button 赤 / 青 | A / B |
| CoreS3 BtnA / BtnB | Select / Start |
| CoreS3 BtnC 長押し | ROM メニュー |

## コマンド

ルートから `just build`、`just flash <port> yes`、`just monitor [port]` を使います。`flash` は現在のファームウェアを上書きするため、明示的な `yes` が必要です。計測版は `just flash-profile <port> yes`、指定秒数のログ保存は `just logs <port> [duration]` を使います。起動から取り直す場合は末尾に `yes` を付けます（例: `just logs <port> 30 yes`）。

性能ログは 1 秒ごとの JSONL で、`fps`、`frame_us`、音声リング、underrun、空き heap / PSRAM を出します。`just build-profile` の計測版では、さらに `cpu_us`、`ppu_us`、`apu_us` のフレーム平均を出します。組込み高速化の設計と検証条件は [knowledge/stackchan-port-architecture.md](../knowledge/stackchan-port-architecture.md) にあります。
