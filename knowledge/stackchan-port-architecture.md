---
type: Decision
title: CoreS3 への Game Boy エミュレータ移植設計
description: ブラウザ向け GB/GBC コアを M5Stack CoreS3 上で安全かつ検証可能に動かすための構造と性能予算。
tags: [architecture, embedded, emulator, performance, stackchan]
status: draft
generated: { by: codex/gpt-5, at: 2026-08-24T00:00:00+09:00 }
verified: { by: process:codex-host-crc-and-cores3-runtime-measurement, at: 2026-08-24T18:30:00+09:00 }
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
  - id: ym2151-claude
    resource: https://github.com/GOROman/YM2151-Claude-3.7-Sonnet
    title: C++17 YM2151 FM sound emulator
    author: person:GOROman
  - id: ymfm
    resource: ../core/ymfm
    title: Aaron Giles ymfm YM2151 implementation (BSD-3-Clause)
    author: person:Aaron-Giles
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

Core 1 の Arduino loop がエミュレーション、PSG音声リングへの投入、LCD DMA の起動、SD 操作を直列に所有する。LCD と microSD は SPI バスを共有するため、SD 操作前に必ず未完了 DMA を join する。Core 0 にはGrove I2C / GPIO入力、YM2151合成、音声mix/resample、speaker queue投入を置く。Core 1はatomicな入力1 byteをフレーム境界で読み、Core 0とはSPSC PCM ringと時刻付きFM event ringだけを共有する。[^cores3-docs]

ROM 選択メニュー中はエミュレーションと DMA バンド転送を停止し、通常の同期描画と SD 操作へ所有権を切り替える。ゲーム中に別タスクから SD や LCD を触らせない。

## メモリ配置

| データ | 形式・概算 | 配置 | 理由 |
| --- | --- | --- | --- |
| ネイティブ画面 | 160×144×2 = 46,080 B | internal DMA-capable SRAM | PPU の書き込みを軽くし、色変換を除く |
| LCD snapshot | 160×144×2 = 46,080 B | internal SRAM | 4 bandの送信中にPPUが次フレームを書いても表示内容を固定する |
| LCD band × 1 | 240×54×2 = 25,920 B | internal DMA-capable SRAM | 次フレーム冒頭で前回DMAをjoinしてから再利用する |
| 音声リング | 8,192×int16 = 16,384 B | internal SRAM | サンプル単位の高頻度アクセスを PSRAM に逃がさない |
| ROM | 初期版は最大 4 MiB | PSRAM、単一所有 | 8 MiB PSRAM 上で読み込み時の二重保持と枯渇を避ける |
| 現在の ROM bank | 16 KiB 単位の窓 | 初期版は ROM 上の直接窓、計測後に cache を判断 | 推測で 32 KiB を予約せず、PSRAM 待ちを先に計測する |
| SRAM | 最大 128 KiB | PSRAM 可 | ROM fetch よりアクセス頻度が低い |

NES 例の「DMA 元は PSRAM に置かない」「巨大 ROM 全体より頻繁な窓を優先する」という観測を引き継ぐが、GB の ROM bank cache は実機プロファイルで PSRAM fetch が支配的だと確認してから追加する。[^nes-stackchan-port]

# 表示設計

GB の 10:9 を保ち、各軸 3:2 の nearest-neighbor で 240×216 にする。320×240 内では左右 40 px、上下 12 px の余白になる。整数 1 倍の 160×144 より読みやすく、全画面 5:3 拡大より計算が単純で、画面バッファも小さい。

PPU は 160×144 のネイティブ画面だけを生成する。4エミュレーションフレームに1度、native framebufferをsnapshotし、240×54の単一band bufferへ3:2拡大する。1フレームにつき1 bandを送るため、CPU/APUは約59.7 fpsのまま、LCDの完成画面は約14.9 fpsで更新される。1 bandは `240 * 54 * 2 = 25,920 B` でESP32-S3 SPIの32 KiB未満に収まる。

M5GFXの`pushImageDMA`は単独呼び出しでは内部のtransaction終了まで待ち、実測約5.5 msをフレーム経路へ残した。外側で`startWrite()`してから呼び、次フレームのband buffer再利用前に`endWrite()`することで転送とエミュレーションを重ねる。メニュー・SD操作への遷移時にも必ず`endWrite()`して、共有SPI busの所有権を同期的に戻す。この構造ではbufferを1本にでき、DMA起動区間は実測約0.33 msになった。

CoreS3 では PPU の pixel 型を byte-swapped RGB565 にし、M5GFX の `setSwapBytes(false)` で bounce buffer を避ける。Web / ホストでは従来の 32-bit RGBA を維持する。

# コア高速化

実機profileとホストCRC比較を交互に行い、次の最適化を組込みモードへ入れた。

1. APUのsample accumulatorを整数化し、CPU命令単位ではcycle数だけを保留する。音声register read/writeとframe末尾でまとめて追いつき、sample / frame-sequencer境界の順序は維持する。
2. PPUは次のmode境界へ達しない命令をevent fast pathで進める。background/windowはpixelごとのtile fetchではなくtile run単位で描画し、CGB paletteはregister write時だけ再計算する。
3. LCDへ送らない3フレームではPPUのLY、STAT、VBlank、windowLineを進めつつpixel生成を省く。ホスト既定では全frameを描画するため、共有コアの従来挙動を維持する。
4. RTCを持たないcartridgeではframeごとのRTC除算を行わない。

計測はESP32-S3のCPU cycle counterを用いる。命令ごとの`esp_timer_get_time()`は計測自体の負荷が大きく、profile結果を歪めたため採用しない。32 KiB KANTAN ROMをinternal SRAMへ置く候補も比較したが、約18.7 fpsのままで改善せず、free heapだけ116 KiBから83 KiBへ減ったので撤回した。CPU dispatch化、IRAM配置、ROM bank cacheは現在のcore処理が16.74 ms予算内に入ったため追加しない。[^nes-stackchan-port]

# 音声とペーシング

コアは44.1 kHz stereo floatを生成する。CoreS3フロントエンドで左右平均、saturating int16変換、DC blockerを行い、8,192 sampleのpower-of-two ringへ積む。ringの中央4,096 sampleを目標に、16.16固定小数点の位相連続linear resamplerで512 sampleの出力chunkを作る。M5Unifiedへ渡す再生rateは常に44.1 kHzに固定し、エミュレーション速度とI2S clockの微差はresamplerのsource stepだけで吸収する。再生中ポインタを上書きしないよう4 slotを回し、M5Unifiedの2 slot speaker queueが埋まっている間は`playRaw`を呼ばない。

GBの目標フレーム周期は70,224 / 4,194,304 = 約16.7427 ms（約59.7275 Hz）。処理がこれより速い場合はフレーム境界で待つ。遅い場合もspeaker clockをchunkごとに変更しない。実測sample生産率とring深さからresampler source rateだけをslew制限付きで追従させ、waveformの位相とspeaker queueの時間軸を連続に保つ。

CoreS3の内蔵speakerと内蔵micはBCLK/WSとI2S1を共有する。M5Unified 0.2.15は両者を別driverとして排他的に扱い、公式microphone exampleも録音時にspeakerを停止する。このため現行APIだけではspeakerの音響loopbackを内蔵micで同時収録できない。自動click評価を追加する場合は通常firmwareから分離した`audio-lab`環境で、ESP-IDFのI2S TX+RX full-duplex driverへ所有権を一本化して検証する。通常版へ未検証のcodec制御を混ぜない。

## Chromatic YM2151

FM backendは既存のBSD-3-Clause `core/ymfm`だけを用いる。Chromatic status-map v4としてFF28/FF29を全YM registerのaddress/data、FF2A writeを256 byte ADPCM FIFO、FF2B bit3を再生edge、FF2Eを`0x51`、FF2Fを`0x04`として実装する。PSG/FM選択はROM reset境界だけで適用し、FMを既定とする。PSG時はFF2Eが`0xFF`なのでROM自身がGame Boy APU driverを選ぶ。

同期ymfmはCore 1のAPUだけで約12.7 ms/frameを消費し、約31 fpsまで低下した。このためCore 1は44.1 kHz PSG PCMとsample cursor付きFM write eventだけを生成し、Core 0の低優先度workerがymfm、ADPCM、mix、DC blocker、resampler、M5Unified speaker queueを一括所有する。LCDは従来の非同期band DMAを維持する。非同期版はdrop/underrun/backpressureを解消したが、両coreのFlash/I-cache競合により約40 fpsであり、59 fps達成は未完了である。

SM83→Xtensa JITはCPU区間が約5 msで主因でなく、ymfmはopcode interpreterではなくsample間feedbackを持つ整数DSPなので現フェーズでは採用しない。ESP32-S3の実行可能internal RAMとcache同期を使う複雑性に対し、必要な12.7 ms削減へ届かない。ymfm hot pathのIRAM配置はIRAM残量約1.5 KiBで起動しなかったため撤回し、ROM cache lockとinactive-channel clock省略も実測差がなかったため撤回した。

JITは将来候補から除外しないが、実装開始のgateを「FM workerを非同期化した後もSM83 CPUがp95で8 ms/frame以上」かつ「2倍高速化の上限見積も含めて目標59.7 fpsへ届く」とする。その時は先にbasic-block cache、MBC bankを含むcode key、cycle-accurate side exit、interrupt/HALT/CGB double-speed、RAM実行・自己書き換え時のinvalidate、実行可能internal RAM上限をhost testで設計する。ymfmについては動的JITを行わず、profileが示した固定hot pathのAOT特化、fixed-point、Xtensa intrinsicの順で比較する。

# ROM、保存、操作

既定ROMはKANTAN GB PLAYの32 KiB CGB ROMとする。upstreamのコミットSHAとROMのSHA-256を `m5stack/rom.lock` に固定し、ビルド時に取得・検証してPlatformIOの `embed_files` でfirmwareへ埋め込む。upstreamにライセンス宣言がないためROMバイナリはリポジトリへ含めず、生成firmwareの再配布にも権利者の許諾が必要である。[^kantan-gb-play]

起動経路ではmicroSDを初期化せず、内蔵ROMをflashからPSRAMへ一度コピーして直ちに実行する。これによりSD未挿入・マウント失敗・SPI競合を起動画面から切り離す。BtnC長押しでメニューへ遷移した時だけSDを初期化する。メニューは内蔵ROMを常に先頭へ置き、続けて `/roms` の `.gb` / `.gbc` を最大63本列挙する。追加ROMのサイズ上限は4 MiBとし、PSRAM allocatorを使う単一のvectorへSDから直接読む。

バッテリー SRAM は ROM ごとの `.sav` として SD に保存する。毎フレームは書かず、RAM write dirty を追跡し、メニュー遷移または一定の無変更時間で `*.sav.part` に書く。完成後は従来の `.sav` を `.bak` へ退避してから置換し、次回ロード時に中断状態を復旧する。電源断耐性のため完成前のファイルを `.sav` 名で見せない。

入力は既存の NES 実装と配線を共有し、GB bit 配置へ変換する。Joystick の上下左右、Dual Button の A/B、画面下部の Select/Start/メニュー長押しを使う。I2C の未接続・読み取り失敗はボタンを離した状態へ戻し、エミュレーションを止めない。

# 観測と検証

本番ログは 1 秒単位の JSONL とし、`event`、`frame`、`fps`、`frame_us`、`audio_ring_samples`、`audio_underruns`、`audio_dropped`、`playback_rate_hz`、`resample_source_rate_hz`、`heap_free_bytes`、`psram_free_bytes`、`display_mode`を一貫した名前で出す。frontend区間は`input_us`、`audio_us`、`display_us`、`save_us`を分離し、計測ビルドだけはコア内部のCPU / PPU / APU cycleも追加する。

検証は次の順序で行う。

1. 合成 ROM と決定的な入力列を参照ビルドと `GB_EMBEDDED` ホストビルドへ与え、各フレームの CPU、WRAM、VRAM、OAM、PPU、framebuffer CRC を比較する。
2. `just check` で Web / embedded の構文、host test、format、lint を実行する。
3. PlatformIO の通常ビルドと計測ビルドを通す。
4. 実機では ROM 起動、入力全ボタン、LCD の tearing、音声 underrun、SD 抜き差し、SRAM 復元を監督下で確認する。
5. 30 秒以上のログで p95 frame time と最小 audio ring を見てから、第 2 段階の CPU dispatch / IRAM / bank cache の要否を決める。

# 非目標

初期移植では Wi-Fi、ブラウザからのミラー、UDP ROM 転送、リモート CLIを対象にしない。これらはゲーム実行経路の性能・正確性を確立した後に独立して追加できる。Web 版の Chromatic FM 機能は維持する。

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
- 表示確認直後は3:2拡大用DMA bandを2本、LCD約29.9 fps、CPU/APU約59.7 fpsと見積もった。その後の実機計測で同期的なSPI待ちが支配的と分かり、snapshot 1本＋DMA band 1本、1 frame 1 band、完成画面約14.9 fpsへ訂正した。`just check`はKANTAN GB PLAY 180 frameで`fb=400e06c2`、`audio=ad3a4bb2`を確認した。
- 実機profileでは当初約9.3 fpsだった。APU event処理、PPU tile-run描画・palette cache・mode-boundary fast path、非表示frameのpixel生成省略によりcore処理を約15.8 msまで短縮した。32 KiB ROMのinternal SRAM配置候補はfree heapを約116 KiBから83 KiBへ減らした一方で約18.7 fpsから改善しなかったため撤回した。各段階でKANTAN GB PLAY 180 frameの`fb=400e06c2`、`audio=ad3a4bb2`一致を再確認した。
- `pushImageDMA`単独呼び出しは約5.5 ms待っていた。NES移植例と同じ外側transaction方式へ変更し、翌frameの`endWrite()`までDMAを重ねた結果、LCD区間は約0.33 ms、実機は58.5〜59.3 fpsになった。さらにゲーム中のM5本体button/touch pollを30 Hzへ落とし、Grove pollは125 Hzのまま維持した結果、59.4〜59.9 fps、input区間約0.24 msになった。
- chunkごとにspeaker playback rateを43 kHz台から44.1 kHzへ変える方式ではringのoverflowとrate揺れが残った。speakerを44.1 kHz固定、16.16位相連続linear resamplerへ変更し、ring中央feedbackを適用した。約7,600 frameまでの実機ログでringは概ね2,900〜3,500 samples、`audio_underruns=0`、`audio_dropped=0`、speaker rate 44,100 Hzを確認した。resampler追加後のaudio frontendは約0.37 ms、静的RAM 251,148 B（76.6%）、Flash 634,313 B（9.7%）。主観的なclick消失は利用者の最終聴取待ちなので文書statusは`draft`のままとする。
- 内蔵micによる自己録音を調査した。CoreS3はspeaker data GPIO13、mic data GPIO14で、BCLK GPIO34、WS GPIO33、I2S1を共有する。M5Unified 0.2.15のAPIと公式exampleは両者を排他利用するため、同時loopbackにはM5Unifiedの上で単に`Mic.begin()`を加えるのではなく、別診断環境でI2S TX+RX full-duplex所有者を実装する必要があると確認した。
- Chromatic v4へ更新し、KANTAN GB PLAYのFM経路を通常版と`GB_EMBEDDED`版で180 frame比較した。CPU・memory・framebuffer・audio CRCは一致し、`fb=2f1926c0`、`audio=446ea41d`だった。CoreS3計測版は静的RAM 259,436 B（79.2%）、Flash 653,117 B（10.0%）。
- 同期ymfm実機版は約31 fps、frame約30.4 ms、APU約12.7 ms、audio drop増加だった。Core 0非同期worker版は約40 fps、frame約24.0 msへ改善し、30秒ログで`audio_underruns=0`、`audio_dropped=0`、PCM/event backpressureとも0を確認した。一方、目標59 fpsには未達である。
- ymfm `generate`のIRAM配置は`.iram0.text`が64,771 Bとなり、起動ログが出ない状態になったため即時撤回した。I-cache lockとinactive channel clock省略は起動したが約40 fpsから有意に改善せず、ROM API依存と互換性リスクを残すため撤回した。これらは失敗した最適化として再採用しない。

[^local-gb-core]: 現行 `core/` の 2026-08-24 時点のソース調査。
[^nes-stackchan-port]: NES 移植例の `core/`、`m5stack/`、ホスト比較 harness、性能関連コミットの調査。
[^cores3-docs]: CoreS3 の ESP32-S3、LCD、PSRAM、microSD、内蔵 speaker に関するハードウェア資料。
[^kantan-gb-play]: KANTAN GB PLAYのREADME、固定コミットのROM、および取得後のGame Boy headerとSHA-256の調査。
[^m5unified-stackchan]: M5Unified公式release 0.2.15と、同タグの`M5Unified.cpp`、`M5Unified.hpp`、`Power_Class.cpp`の調査。
