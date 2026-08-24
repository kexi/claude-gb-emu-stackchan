---
type: Decision
title: CoreS3 への Game Boy エミュレータ移植設計
description: ブラウザ向け GB/GBC コアを M5Stack CoreS3 上で安全かつ検証可能に動かすための構造と性能予算。
tags: [architecture, embedded, emulator, performance, stackchan]
status: draft
generated: { by: codex/gpt-5, at: 2026-08-24T00:00:00+09:00 }
verified: { by: process:codex-build-host-and-device-check, at: 2026-08-24T14:48:47+09:00 }
sources:
  - id: local-gb-core
    resource: ../core
    title: 現行 Game Boy / Game Boy Color C++ コア
    author: team:kexi
    last_modified: 2026-08-24T00:00:00+09:00
  - id: nes-stackchan-port
    resource: https://github.com/kexi/cluade-famicom-emu-stackchan
    title: M5Stack CoreS3 向け NES エミュレータ移植例
    author: team:kexi
    last_modified: 2026-08-24T00:00:00+09:00
  - id: cores3-docs
    resource: https://docs.m5stack.com/en/core/CoreS3
    title: M5Stack CoreS3 documentation
    author: team:m5stack
  - id: kantan-gb-play
    resource: https://github.com/GOROman/kantan-gb-play/tree/9ae7b2d6e3cbdd1fb5904087f976d22b8429bad5
    title: KANTAN GB PLAY pinned source and ROM
    author: person:GOROman
  - id: m5unified-stackchan
    resource: https://github.com/m5stack/M5Unified/releases/tag/0.2.15
    title: M5Unified 0.2.15 StackChan support release
    author: team:m5stack
    last_modified: 2026-05-15T05:36:53Z
---

# 目的と完了条件

既存の WebAssembly 向け GB/GBC コアを壊さず、M5Stack CoreS3 の LCD、スピーカー、microSD、タッチ、Grove 入力を使って単体動作させる。初期移植の完了条件は次のとおり。

- 内蔵KANTAN GB PLAYをmicroSDなしで電源投入から直接起動できる。
- `.gb` / `.gbc` をmicroSDの `/roms` から選択して起動できる。
- 160×144 の映像をアスペクト比を保った 240×216 RGB565 として表示できる。
- 本体スピーカーから途切れにくい音声を再生できる。
- Grove ジョイスティック、Dual Button Unit、本体下部タッチで全 8 ボタンを操作できる。
- Web 版と組込み最適化版を同じホスト ROM・入力列で比較し、CPU・メモリ・PPU 状態と画面が一致する。
- 実機ログからフレーム時間、表示転送、音声リング量、空き heap / PSRAM を追跡できる。

# 採用する構造

## コアとフロントエンドの境界

`core/` は Web と CoreS3 で共有し、`GB_EMBEDDED` は表現とホットパスだけを切り替える。ハードウェア API、FreeRTOS、M5Unified、SD は `m5stack/` から外へ漏らさない。これによりホスト上で参照版と組込み版を同じ入力に対して比較できる。[^nes-stackchan-port]

コアの公開面は `GB::loadRom`、`reset`、`runFrame`、ボタン状態、フレームバッファ、音声サンプル、SRAM に限定する。WASM の C API はこの公開面を包むだけとし、CoreS3 は C++ API を直接使う。

## タスクと所有権

Core 1 の Arduino loop がエミュレーション、音声リングへの投入、LCD DMA の起動、SD 操作を直列に所有する。LCD と microSD は SPI バスを共有するため、SD 操作前に必ず未完了 DMA を join する。Core 0 には Grove I2C / GPIO 入力ポーリングだけを置き、Core 1 は atomic な 1 byte をフレーム境界で読む。[^cores3-docs]

ROM 選択メニュー中はエミュレーションと DMA バンド転送を停止し、通常の同期描画と SD 操作へ所有権を切り替える。ゲーム中に別タスクから SD や LCD を触らせない。

## メモリ配置

| データ | 形式・概算 | 配置 | 理由 |
| --- | --- | --- | --- |
| ネイティブ画面 | 160×144×2 = 46,080 B | internal DMA-capable SRAM | PPU の書き込みを軽くし、色変換を除く |
| LCD band × 2 | 240×54×2×2 = 51,840 B | internal DMA-capable SRAM | 変換とSPI転送を重ね、全拡大画面より約 52 KiB 小さい |
| 音声リング | 8,192×int16 = 16,384 B | internal SRAM | サンプル単位の高頻度アクセスを PSRAM に逃がさない |
| ROM | 初期版は最大 4 MiB | PSRAM、単一所有 | 8 MiB PSRAM 上で読み込み時の二重保持と枯渇を避ける |
| 現在の ROM bank | 16 KiB 単位の窓 | 初期版は ROM 上の直接窓、計測後に cache を判断 | 推測で 32 KiB を予約せず、PSRAM 待ちを先に計測する |
| SRAM | 最大 128 KiB | PSRAM 可 | ROM fetch よりアクセス頻度が低い |

NES 例の「DMA 元は PSRAM に置かない」「巨大 ROM 全体より頻繁な窓を優先する」という観測を引き継ぐが、GB の ROM bank cache は実機プロファイルで PSRAM fetch が支配的だと確認してから追加する。[^nes-stackchan-port]

# 表示設計

GB の 10:9 を保ち、各軸 3:2 の nearest-neighbor で 240×216 にする。320×240 内では左右 40 px、上下 12 px の余白になる。整数 1 倍の 160×144 より読みやすく、全画面 5:3 拡大より計算が単純で、画面バッファも小さい。

PPU は 160×144 のネイティブ画面だけを生成する。LCD更新時に240×54の2本のバンドバッファへ3:2拡大し、一方をSPI DMAで転送中に他方を変換する。1バンドは `240 * 54 * 2 = 25,920 B` でESP32-S3 SPIの32 KiB未満に収まる。M5GFXがStackChan向けに採用する40 MHzでは240×216を約60 fpsで全転送できないため、CPU/APUは約59.7 fpsのまま、LCDだけ2フレームに1回（約29.9 fps）更新する。次のバッファをDMAへ渡す直前と、メニュー・SD操作への遷移前に未完了DMAをjoinする。

CoreS3 では PPU の pixel 型を byte-swapped RGB565 にし、M5GFX の `setSwapBytes(false)` で bounce buffer を避ける。Web / ホストでは従来の 32-bit RGBA を維持する。

# コア高速化

最初から意味論を変えない次の二つだけを組込みモードへ入れる。

1. PPU の dot loop は mode 境界（1、81、253、456 dot）までまとめて進め、境界イベントだけ逐次処理する。CPU レジスタアクセス間の観測点は維持する。
2. APU の oscillator timer は次の出力サンプル境界までまとめて減算し、sample / frame-sequencer 境界では従来と同じ順序で処理する。

CPU の 256 関数 dispatch 化、IRAM 配置、ROM bank の internal SRAM cache は実測後の第 2 段階とする。NES では巨大 switch の I-cache miss が支配的だったが、SM83 の switch サイズと ROM fetch 比率が同じとは限らないためである。計測ビルドで `cpu_us`、`ppu_us`、`apu_us` を分離し、最大要因だけを変える。[^nes-stackchan-port]

# 音声とペーシング

コアは 44.1 kHz stereo float を生成する。CoreS3 フロントエンドで左右平均、saturating int16 変換、DC blocker を行い 8,192 sample の power-of-two ring へ積む。M5Unified へは 512 sample の固定 chunk で渡し、再生中ポインタを上書きしないよう 4 slot を回す。

GB の目標フレーム周期は 70,224 / 4,194,304 = 約 16.7427 ms（約 59.7275 Hz）。処理がこれより速い場合はフレーム境界で待つ。遅い場合は映像をさらに間引かず、まず LCD divisor を維持したまま音声再生 rate を実測 sample 生産率へ滑らかに追従させ、underrun を避ける。60 fps 固定値ではなく GB の実周波数を正本にする。

# ROM、保存、操作

既定ROMはKANTAN GB PLAYの32 KiB CGB ROMとする。upstreamのコミットSHAとROMのSHA-256を `m5stack/rom.lock` に固定し、ビルド時に取得・検証してPlatformIOの `embed_files` でfirmwareへ埋め込む。upstreamにライセンス宣言がないためROMバイナリはリポジトリへ含めず、生成firmwareの再配布にも権利者の許諾が必要である。[^kantan-gb-play]

起動経路ではmicroSDを初期化せず、内蔵ROMをflashからPSRAMへ一度コピーして直ちに実行する。これによりSD未挿入・マウント失敗・SPI競合を起動画面から切り離す。BtnC長押しでメニューへ遷移した時だけSDを初期化する。メニューは内蔵ROMを常に先頭へ置き、続けて `/roms` の `.gb` / `.gbc` を最大63本列挙する。追加ROMのサイズ上限は4 MiBとし、PSRAM allocatorを使う単一のvectorへSDから直接読む。

バッテリー SRAM は ROM ごとの `.sav` として SD に保存する。毎フレームは書かず、RAM write dirty を追跡し、メニュー遷移または一定の無変更時間で `*.sav.part` に書く。完成後は従来の `.sav` を `.bak` へ退避してから置換し、次回ロード時に中断状態を復旧する。電源断耐性のため完成前のファイルを `.sav` 名で見せない。

入力は既存の NES 実装と配線を共有し、GB bit 配置へ変換する。Joystick の上下左右、Dual Button の A/B、画面下部の Select/Start/メニュー長押しを使う。I2C の未接続・読み取り失敗はボタンを離した状態へ戻し、エミュレーションを止めない。

# 観測と検証

本番ログは 1 秒単位の JSONL とし、少なくとも `event`、`frame`、`fps`、`frame_us`、`audio_ring_samples`、`audio_underruns`、`heap_free_bytes`、`psram_free_bytes`、`display_divisor` を一貫した名前で出す。計測ビルドだけはコア内部の CPU / PPU / APU 時間を追加する。

検証は次の順序で行う。

1. 合成 ROM と決定的な入力列を参照ビルドと `GB_EMBEDDED` ホストビルドへ与え、各フレームの CPU、WRAM、VRAM、OAM、PPU、framebuffer CRC を比較する。
2. `just check` で Web / embedded の構文、host test、format、lint を実行する。
3. PlatformIO の通常ビルドと計測ビルドを通す。
4. 実機では ROM 起動、入力全ボタン、LCD の tearing、音声 underrun、SD 抜き差し、SRAM 復元を監督下で確認する。
5. 30 秒以上のログで p95 frame time と最小 audio ring を見てから、第 2 段階の CPU dispatch / IRAM / bank cache の要否を決める。

# 非目標

初期移植では Wi-Fi、ブラウザからのミラー、UDP ROM 転送、リモート CLI、Chromatic FM 拡張の実機音声を対象にしない。これらはゲーム実行経路の性能・正確性を確立した後に独立して追加できる。Web 版の Chromatic FM 機能は維持する。

# 検証記録

2026-08-24 に次の trust signal を取得した。

- 合成 ROM を同じ入力列で 180 フレーム実行し、参照版と `GB_EMBEDDED` 版の CPU レジスタ、WRAM、VRAM、OAM、framebuffer、音声 CRC が一致した。結果は `fb=400e06c2`、`audio=ad3a4bb2`。
- WebAssembly 版、CoreS3 通常版、`GB_PROFILE` 計測版をそれぞれビルドした。入力・ログ修正後の通常版は静的 RAM 178,492 B（54.5%）、Flash 576,237 B。計測版は静的 RAM 178,540 B（54.5%）、Flash 576,865 B。
- CMake が生成した実 compile database に対する `clang-tidy`、`nix flake check`、`actionlint`、`pinact` の SHA / version 検証、Git 履歴に対する `gitleaks` を通した。
- 実機への書き込み、実 ROM、LCD / speaker / Grove / SD、動的確保する 72,000 B の表示バッファ、30 秒ログの p95 は未検証。このため文書 status は `draft` のままとする。
- 2026-08-24 の初回実機ログ取得で、`pio device monitor | tee` は非対話環境にTTYがなく `termios error 19` となった。ファームウェア障害ではない。ログ保存経路を、実TTYを要求しない時間制限付き pyserial reader へ訂正した。
- 入力・ログ修正版の再書き込み時、921,600 baud への切替後に `No serial data received` となった。460,800 baudでも同じ切替点で再現したため、USB転送の再現性を優先して PlatformIO の upload speed をbootloader既定の115,200 baudへ固定した。
- pyserialから通常起動リセットをかけるとnative USBが再列挙され、最初のreaderは古いfile descriptorで `Device not configured` となった。時間制限内で同じportを再openするreaderへ訂正した。
- 計測版の初回実機書き込みは成功した。旧入力実装の15秒ログでは `joystick_connected` 8件、`joystick_disconnected` 6件とJSON断片の混線を観測した。原因を「bare ACKだけでの機種判定」と「Core 0 / Core 1からの同時Serial書き込み」に分け、機種固有read成功後の接続確定とCore 1単一writerへ修正した。
- 修正版はホスト一致テストとCoreS3計測ビルドを通したが、USB uploaderがstub後に切断するため実機へ反映できていない。921,600、460,800、115,200 baudで連続再現したので再試行を停止した。ケーブルまたはUSB portを物理的に変更してから、修正版の入力安定性と整形式JSONLを再検証する。
- 黒画面報告を受け、SDメニュー待ちを既定起動にしていた設計を訂正した。KANTAN GB PLAYを固定SHAで内蔵し、SD初期化をBtnC長押し時まで遅延する。ROMのheaderは32 KiB、CGB対応、ROM-only、RAMなしであることを取得物のbyte列から確認した。実機表示は再書き込み後に検証する。
- KANTAN GB PLAYを180フレーム実行した結果、参照版と組込み版でCPU、WRAM、VRAM、OAM、framebuffer CRCが一致し、framebufferは7色だった。組込み専用APU一括処理だけは音声CRCが `feda8e20` 対 `01f61aa1` で不一致だったため撤回し、1サイクル版へ統一後に音声CRC `feda8e20` の一致を確認した。
- `esptool --no-stub` により、以前stub起動直後に切断していた同じ `/dev/cu.usbmodem2101` へKANTAN GB内蔵計測firmwareを書き込めた。bootloader、partition、applicationの全領域で書き込み後hash検証に成功した。計測版は静的RAM 178,804 B（54.6%）、Flash 610,505 B（9.3%）。USB CDCログはリセット有無の両方で0 byteのため、LCD表示と実機性能値は未検証のままである。
- 書き込み後も全面黒との目視報告を受け、拡大、snapshot、DMA、band転送を外した160×144同期転送へ切り替え、backlightを128へ明示設定した。USB-JTAGで両coreをhaltしてPCを確認すると、core 1は `lgfx::v1::Bus_SPI::writeBytes`、core 0はidleにあり、firmwareは実行中だった。さらに実機RAMのframebuffer 46,080 byteをdumpし、`3bad`、`8b39`、`b9b5`、`bef7`、`c718`、`d78b`、`ffff` の7画素値を確認した。したがってROM、CPU/PPU、framebuffer生成、SPI送信呼出しは生きており、残る切り分け対象はLCD panel/backlight電源または実機機種判定である。
- JTAG診断ではM5GFXが本体とLCDを`board_M5StackChan`（ID 27）、320×240、輝度255として認識する一方、M5Unified 0.2.2のPMICはunknownだった。公式タグを比較するとStackChanのPMIC、I2C、SD、speaker、入力対応は0.2.15で初めて追加され、同リリースの変更理由も`Add support StackChan`だった。0.2.15へ固定して実機へ書き込んだ結果、従来全面黒だったLCDに160×144のKANTAN GB PLAYが表示され、黒画面の原因を依存ライブラリの世代不整合と確定した。[^m5unified-stackchan]
- 表示確認後、3:2拡大用のDMA bandを2本へ変更し、LCD約29.9 fps、CPU/APU約59.7 fpsの分離設計にした。通常firmwareは静的RAM 230,772 B（70.4%）、Flash 625,749 B（9.5%）でビルド成功し、`just check`はKANTAN GB PLAY 180 frameで`fb=400e06c2`、`audio=ad3a4bb2`を確認した。音声実機切り分けのため、内蔵speakerを明示有効化し、起動時確認音とspeaker beginの構造化ログを追加した。拡大表示と実機音声は再書き込み後に目視・聴取確認する。

[^local-gb-core]: 現行 `core/` の 2026-08-24 時点のソース調査。
[^nes-stackchan-port]: NES 移植例の `core/`、`m5stack/`、ホスト比較 harness、性能関連コミットの調査。
[^cores3-docs]: CoreS3 の ESP32-S3、LCD、PSRAM、microSD、内蔵 speaker に関するハードウェア資料。
[^kantan-gb-play]: KANTAN GB PLAYのREADME、固定コミットのROM、および取得後のGame Boy headerとSHA-256の調査。
[^m5unified-stackchan]: M5Unified公式release 0.2.15と、同タグの`M5Unified.cpp`、`M5Unified.hpp`、`Power_Class.cpp`の調査。
