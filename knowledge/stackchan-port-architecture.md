---
type: Decision
title: CoreS3 への Game Boy エミュレータ移植設計
description: ブラウザ向け GB/GBC コアを M5Stack CoreS3 上で安全かつ検証可能に動かすための構造と性能予算。
tags: [architecture, embedded, emulator, performance, stackchan]
status: draft
generated: { by: codex/gpt-5, at: 2026-08-24T00:00:00+09:00 }
verified: { by: process:codex-host-sample-parity-and-cores3-runtime-measurement, at: 2026-08-25T01:20:00+09:00 }
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
  - id: mame-msm6258
    resource: https://github.com/mamedev/mame/blob/master/src/devices/sound/okim6258.cpp
    title: MAME OKI MSM6258 reference implementation
    author: team:mamedev
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

コアは44.1 kHz stereo floatを生成する。CoreS3フロントエンドで左右平均、saturating int16変換、DC blockerを行い、8,192 sampleのpower-of-two ringへ積む。16.16固定小数点の位相連続linear resamplerで512 sampleの出力chunkを作るが、sourceとM5Unifiedの再生rateはともに常時44.1 kHzとする。再生中ポインタを上書きしないよう4 slotを回し、M5Unifiedの2 slot speaker queueが埋まっている間は`playRaw`を呼ばない。

GBの目標フレーム周期は70,224 / 4,194,304 = 約16.7427 ms（約59.7275 Hz）。処理がこれより速い場合はフレーム境界で待つが、FM選択時はPCM ring＋mix ringの合計が14,336 samplesに達するまで待ちを省き、エミュレーションをwall clockより先行させて音声を貯金する。遅延はPCM ringとspeaker DMAで吸収し、wall-clock上の短期的なfps揺れをresampler source rateへ反映しない。1 emulated frameが生成するsample数はCPU cycle基準で決まるため、source rateを変えるとゲーム本来の音程を変調してしまう。先行貯金は音程・テンポを変えず、映像が音響より最大約325 ms先行するAV offsetだけを代償とする。

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

KANTANの自動演奏はフロントエンド側でコード進行を生成しない。固定ROMのソースが持つ内蔵デモを`SELECT+A`の単一edgeで開始し、4 emulated frame後に全自動入力を解放する。以後のコード、ベース、YM channel 7 noise、ADPCMドラムはROM自身の155 BPM・12 main-loop iteration/eighthのグリッドだけで進める。これによりwall clock、PPU frame、audio sampleからROMの拍位置を外挿する経路をなくす。

内蔵デモの累積ドリフトをなくしても、固定ROMの`adpcm_play()`は`STOP → 160 byte prime → PLAY`の順なので、同じgrid stepのYM chord key-onよりADPCM play edgeが遅れる。ROM改変案は診断だけに留め、固定ROMとCPU-visibleなregister/FIFO/status、生event timestampは変更しない。FM再生時だけCoreS3のPCM入力を2,048 samples（約46.4 ms）保持し、音響consumerが同じ窓の未来eventを確認する。`STOP → preload → PLAY`が窓内で完結するときは、立下りSTOPを保留し、受理済みpreloadとPLAYを新しいトランザクションの同じ音響sampleへまとめる。PLAY後のstreaming feedは同じ補正量だけ移すが、次のSTOPへ前回の補正量を持ち越さない。直後のserialized YM onset burstも新しいトランザクションに合わせる。これによりFIFO供給速度を保ったままコード、ADPCM、YM noiseハイハット、ベースを約2 ms内へ収める。2,048 samplesを超えるpreloadは生時刻のままfail-openし、PSG選択時は保持を行わない。

# 観測と検証

本番ログは 1 秒単位の JSONL とし、`event`、`frame`、`fps`、`frame_us`、`audio_ring_samples`、`audio_underruns`、`audio_dropped`、`playback_rate_hz`、`resample_source_rate_hz`、`heap_free_bytes`、`psram_free_bytes`、`display_mode`を一貫した名前で出す。frontend区間は`input_us`、`audio_us`、`display_us`、`save_us`を分離し、計測ビルドだけはコア内部のCPU / PPU / APU cycleも追加する。

検証は次の順序で行う。

1. 合成 ROM と決定的な入力列を参照ビルドと `GB_EMBEDDED` ホストビルドへ与え、各フレームの CPU、WRAM、VRAM、OAM、PPU、framebuffer CRC を比較する。
2. `just check` で Web / embedded の構文、host test、format、lint を実行する。
3. PlatformIO の通常ビルドと計測ビルドを通す。
4. 実機では ROM 起動、入力全ボタン、LCD の tearing、音声 underrun、SD 抜き差し、SRAM 復元を監督下で確認する。
5. 30 秒以上のログで p95 frame time と最小 audio ring を見てから、第 2 段階の CPU dispatch / IRAM / bank cache の要否を決める。
6. KANTAN内蔵デモでは同期rendererとdeferred event replayを同じROM・入力で実行し、eventをtimestampのsample生成直前に適用したstereo PCMがsample単位で完全一致することを確認する。
7. 実機のFM autoplayを300秒連続で計測し、`speaker_queue_empty`が起動直後の1回から増えないこと、`audio_underruns`、`audio_dropped`、`audio_output_clips`、`adpcm_fifo_rejected`、`alignment_failures`、`speaker_queue_failures`、`event_backpressure`がすべて0であることを確認する。`audio_backpressure`は先行生成時の待機回数なので0を要求しない。
8. 固定KANTAN ROMの音響整列後は、CH2 key-onからADPCM playまで64 samples以下、ADPCM playからYM noiseハイハットまで64 samples以下、ベースまで128 samples以下、全one-shotでFIFO starvation 0、alignment failure 0とする。さらに連続打音では有効STOP→PLAY間隔を0 sample、再トリガー境界の出力段差を1 LSB以下とする。10分間の内蔵デモでoffset変動1 sample以下、FIFO reject、late event、audio underrun/drop/clip、PCM/event backpressureをすべて0とし、利用者の同一音量での聴取確認を最終gateにする。

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
- 8,192 sampleのPSG入力ringを4,096 sampleへ減らし、7,757 Bの`CPU::step()`だけをIRAMへ配置する逆方向のcache競合回避もビルドとhost FM CRCに成功した。しかし実機は書き込み後とreset後のどちらもUSB CDCログ0 byteで起動継続を確認できなかったため即時撤回した。CPU/PPUの大型関数を直接IRAM化する案も再採用しない。
- CoreS3のymfm翻訳単位だけをO3からO2へ変える実験では、object textが18,505 Bから14,739 Bへ約20%減りFM CRCも不変だった。しかし実機fpsは約40から38.5〜39.0へ悪化したため撤回した。cache footprintよりO3の演算最適化が効いている。
- プロファイル版全体へ`-flto`を付ける試行は、Arduino-ESP32 2.0.16のリンク手順がGCC LTO pluginをlinkerへ渡さず`plugin needed to handle lto object`と`app_main` undefinedで失敗したため撤回した。toolchain・framework更新なしに同じglobal LTOを再試行しない。
- Core 1の未計上区間とCore 0 workerのcycleを追加計測した。timer/serial/RTCを含むmiscは約4.5〜4.9 ms/frame、audio workerは約0.67〜0.69 s/s稼働だった。命令ごとのcycle計測自体がfpsを約40から38.5へ下げたためmisc計測は撤回し、低頻度のworker稼働率だけ残した。
- `output_4op`をCoreS3で強制inline化するAOT特化は、独立関数を消し`ym2151::generate`との合計を約2.57 KiBから1.33 KiBへ減らしFM CRCも不変だった。しかし実機は37.7〜38.2 fpsへ悪化したため撤回した。呼出し削減より大型関数化のI-cache局所性悪化が大きい。
- Game Boy timerの2の累乗除算を明示的なvariable shiftへ書き換えたが、CoreS3バイナリサイズと実機fpsは変わらなかった。GCC O3が元の2の累乗除算を既にshift化していたためで、意図が直接表現されるshift実装は維持した。
- YM3012 DACの10.3-bit量子化round-tripを省く実験は、実機fpsを約40.0から40.7へ、worker稼働を約0.67〜0.69 s/sから約0.64 s/sへ小幅改善した。一方で音色精度を落とし、FM event backpressureが0から最大67,666へ増えたため撤回した。精度を落とすfast FM profileとしても採用しない。
- 正確なYM3012処理へ戻した最終実機15秒計測は39.92〜40.20 fps、worker稼働0.687〜0.717 s/sで、audio underrun/dropとPCM/event backpressureはすべて0だった。Core 0のymfmだけで実時間の約70%を使うため、59.7 fps達成にはFM生成の5〜10%以上の削減に加え、Core 1側の約4.5〜4.9 ms/frameのmisc処理削減が必要である。
- `GB_PROFILE`なしの正確なFM autoplayを同じ実機で15秒計測すると、外れ値43.00 fpsを除き44.42〜45.91 fps、frame 20.78〜21.49 ms、resample source rate 31.8〜33.8 kHzだった。audio underrun/dropとPCM/event backpressureはすべて0である。命令単位cycle計測の摂動が約5 fpsあるため、40 fpsを本番性能として扱わず、以後の性能acceptanceは非計測版の同一シナリオで比較する。
- ymfmのsin/power各512 Bとenvelope increment 256 Bをinternal DRAMへ固定するA/Bでは、map上の`.dram1`配置とFM CRC不変を確認したが、RAMが1,288 B増えた一方で実機は外れ値42.96 fpsを除き44.47〜45.86 fpsとbaselineから改善しなかったため撤回した。sin表は初期化時だけ、power表はoperator出力、increment表はenvelope更新時に参照され、64 KiB data cacheで既に十分cacheされる規模だったと推定する。
- HALT deadline fast-forwardの初回実機15秒計測では、frame処理が約20.8〜21.5 msから10.4〜11.9 msへ短縮し、概ね58〜62 fpsへ達した。KANTAN FMはCPU stepの92.36%、emulated cycleの83.77%がHALT待機だったため、PPU次境界・TIMA overflow・serial完了まで既存`tick()`をまとめる効果が大きかった。一方、60 fpsのFM register burstで512件event queueが満杯になり、backpressure spinが最大366,552まで増えた。
- inline配列のままFM event queueを1,024件へ拡大する候補は、Core 1側queueだけでなくCore 0 worker stack上の非deferred rendererも4,096 B増やし、実機がUSB再列挙を繰り返したため撤回した。いったん512件へ戻し、実測backlog時の既定fallbackどおりspeaker priority 4、FM worker 3、Grove input 2でdrain性能を比較した。
- worker priority 3 / Grove priority 2だけの20秒比較でも512件queueは複数回満杯となり、backpressure spinは2,347,773まで累積した。平均44.1 kHz生成とaudio ringは追従しているため、原因はworker優先度ではなくregister write burst容量である。`ChromaticFM`のevent配列をdeferred時だけ動的確保する形へ分離し、非deferred worker rendererのtask stackを増やさずproducerだけ1,024件へする方針へ訂正した。
- deferred producerだけ1,024件のevent queueをheap確保する実装を実機で60秒連続計測した。59 samplesで57.54〜61.63 fps、frame処理9.245〜13.957 ms、audio ring 2,277〜4,402 samples、event high-water最大412/1,023だった。audio underrun/drop、PCM/event backpressureは全期間0、free heapは102,384 Bで不変だった。これによりHALT fast-forward後のFM register burstを余裕を持って吸収し、正確なYM3012処理のまま実時間再生を維持できることを確認した。
- 最終autoplay firmwareをCoreS3へ再書き込みし、hash検証後の15秒ログでも58.19〜61.26 fps、event high-water最大428/1,023、free heap 102,384 Bを確認した。audio underrun/dropとPCM/event backpressureはすべて0で、再書き込み後もFM自動演奏を継続した。
- 利用者から残るプツプツ音の報告を受け、出力直前clip、M5Unified virtual speaker queue空遷移、`playRaw`失敗、chunk境界最大差を追加観測した。20秒ログではclipと`playRaw`失敗は0、境界最大差3,793/65,535、queue空遷移は起動後1秒未満の4回だけで以後増えなかった。従来の`audio_underruns`は1秒時点のsource ringだけを見ており、短いspeaker側状態を保証していなかったため診断項目を訂正した。
- 無音2 chunkを即時投入する起動処理を撤回し、実音2 chunkぶんをmix ringへ蓄積してから最初のchunkをfade-inする方式へ変更した。virtual queue空遷移は4回から1回へ減った。2 chunkを同一worker周回で連続`playRaw`する候補は書き込み後にUSB CDCログが停止し、実行継続を確認できなかったため撤回した。
- M5Unified 0.2.15のspeaker taskを確認すると、virtual queueが空でも既定256 samples×8 DMA bufferをゼロで補いながら約46 ms再投入を待つため、`isPlaying()==0`は即時のI2S underrunではない。chunkとmix ringは512/2,048 samplesへ戻し、実際のI2S DMAを512 samples×8の約93 msへ拡張した。最終20秒ログは57.90〜61.57 fps、free heap 97,248 B一定、clip、drop、`playRaw`失敗、PCM/FM backpressureすべて0で、virtual queue空遷移も累積1から増えなかった。主観的なプツプツ消失は利用者の聴取待ちである。
- 利用者の音程不安定報告とログを照合すると、wall frame時間とring深さから算出したresampler source rateが約43.4〜44.7 kHzの範囲で動き、短期fps jitterをFMの音程へ変換していた。適応rate制御を削除してemulated cycle基準の44,100 Hzへ固定した。実機60秒・59 samplesでは平均59.728 fps、source rate最小/最大とも44,100 Hz、ring 1,639〜4,120 samples、free heap 97,296 B一定で、underrun、drop、clip、`playRaw`失敗、PCM/FM backpressureはすべて0だった。ringは先頭2,587、末尾3,665で周期的に上下し、一方向の枯渇・蓄積は観測しなかった。
- KANTANのドラム不足報告を受け、Core 0 rendererのADPCM FIFO受理/拒否、再生開始、active sample、ADPCM/YM個別peakを1秒窓で観測した。基準20秒ではFIFO拒否0、毎秒2〜3回のADPCM開始、ADPCM peak 10,014、YM peak 2,895〜3,223で、転送欠落ではなかった。本家通常パターンはキックとYMハイハットが中心で、スネア/クラッシュは8小節ごとのfillだけである。CoreS3のADPCM mix gainを0.5から0.75へ上げ、高頻度診断atomicを128 sample単位へまとめた候補の20秒ログでは57.54〜61.94 fps、FIFO拒否、audio underrun/drop/clip、speaker failure、PCM/FM backpressureがすべて0だった。主観的なドラムバランスは利用者の聴取確認待ちである。
- 自動演奏のコード変更をwall clockの2,000 ms周期で行うと、KANTAN内部の155 BPM・12 frame/eighth・96 frame/bar（実測約1.6秒）と一致せず、ドラムに対して継続的に位相がずれてリズムが崩れた。`PPU::frameCount`を基準にC–G–Am–Fを各96 emulated frameで切り替え、末尾12 frameだけAを離して次の入力edgeを作る方式へ変更した。CoreS3へ書き込み後の12秒ログでは57.09〜62.01 fps、ADPCM FIFO拒否、audio underrun/drop/clip、speaker failure、PCM/FM backpressureはすべて0であり、累積ドリフトを排除できた。主観的なグルーヴは利用者の聴取確認待ちである。
- 上記96 frame/barの推定は誤りだった。deferred Chromatic eventをホストでsample位置付き追跡すると、旧autoplayのコード発音間隔は約71,000 samplesだが、ROMのADPCMキック間隔は通常約18,459 samplesで、小節相当は約73,800〜74,600 samplesだった。さらに入力edgeからYM chord key-onまで約1,100〜2,000 samplesの遅延がある。autoplayを44.1 kHz emulated sample cursor基準の74,200 samples/chordへ訂正し、発音間隔73,827〜74,599 samplesとROMのグリッドが一致することを900 frameのhost traceで確認した。CoreS3へhash検証付きで再書き込みした15秒ログは56.49〜63.00 fps、source/playback rate 44,100 Hz、ADPCM FIFO拒否、audio underrun/drop/clip、speaker failure、PCM/FM backpressureがすべて0だった。主観的な位相一致は利用者の聴取確認待ちである。
- 上記の外部autoplay調整は、固定コミットのROMソース確認により設計ごと撤回した。ROMには`SELECT+A`で開始するデモがあり、コード、ベース、YM noiseハイハット、MSM6258キック/スネア/クラッシュを同じfree-running eighth-note gridで更新する。フロントエンドはframe 206〜209だけ`SELECT+A`を入力し、それ以後は方向・A/Bを一切自動入力しない。900 frameのevent traceではYM chord key-onから次のADPCM startまで約585 samplesで一定となり、累積ドリフトを観測しなかった。
- 同期Chromatic rendererとCoreS3用deferred producerのKANTAN内蔵デモを3,500 frame実行し、2,589,979 samples、160,240 eventsについてoffline replayとstereo float PCMを比較した。eventはtimestampと同じsampleを生成する直前に適用し、`max_diff=0`、late event 0だった。したがって非同期worker分割はYMとADPCMのsample位置を変えない。
- ROM内デモはfillで従来計測より大きなregister/FIFO burstを生成した。deferred producerだけのevent queueを2,048件へ拡大した実機35秒計測ではhigh-water 1,368/2,047、event backpressure 0、ADPCM FIFO rejected 0、audio underrun/drop/clip 0、speaker queue failure 0、source/playback rate 44,100 Hzだった。frame 1,609の窓でPSG入力ringへのbackpressure spinが18,840回発生したがsampleのdropやtimestamp変更はなく、その次の窓までにringをdrainした。これは音声欠落ではなくproducerを一時待機させるbounded flow controlとして記録し、主観的な位相一致は利用者の聴取確認待ちとする。
- 固定ROMソースと900 frame traceを突き合わせると、残る一定offsetはCoreS3のDMAではなくKANTANのADPCM投入順序だった。代表区間はYM chord CH0/CH1/CH2 key-onがsample 196,472/196,536/196,600、ADPCM playが197,057で、CH2後にも457 samples（約10.4 ms）、CH0後には585 samples（約13.3 ms）遅れる。`adpcm_play()`がgrid到達後に160 byteを同期投入してからplay edgeを出すためである。ROMのsmall-prime化は1/8/16 byteすべてFIFO starvationを起こしたため採用せず、固定ROMのSHA-256を維持する方針へ訂正した。
- 当初1,024 sample lookaheadで音響alignerを固定ROMの内蔵デモ3,500 frameへ適用した。生event replayは2,589,979 samples、160,240 eventsで同期rendererと`max_diff=0`、late event 0を維持した。補正後は137 one-shot、alignment failure 0、FIFO starvation 0で、CH2→ADPCM最大10 samples、ADPCM→YM noiseハイハット最大11 samples、ADPCM→ベース最大74 samplesだった。生eventのCH2→PLAYは445〜458 samplesのままであり、CPU-visible時系列を変えていない。その後、連打の拍補正に必要な先読みを含めるため2,048 samplesへ拡張した。
- 利用者の「連続したドラム音に弱い」という報告から、整列後のSTOP→PLAY間隔を追加計測した。旧実装は次のSTOPへ前回one-shotの補正量を適用し、最大451 samples（約10.2 ms）の無音を再トリガー直前へ作っていた。STOP、受理済みpreload、PLAYを新トランザクションの同一sampleへ配置するよう訂正し、3,500 frameの136組で有効STOP→PLAY間隔をすべて0 sampleにした。24 samples（約0.54 ms）のhalf-cosine residual de-clickはADPCMのraw predictor/outputと提示波形を分離し、新しいattackをfade-inせず直前波形の残差だけを減衰させる。再トリガー境界の最大段差は0 LSB、CH2→ADPCM最大9 samples、ADPCM→YM noiseハイハット最大11 samples、ベース最大74 samples、FIFO starvation/alignment failureは0だった。2,589,979 samples、160,240 eventsの同期比較は`max_diff=0`、`raw_pcm_hash=98f3333d`、`raw_adpcm_hash=eaadee88`で、YM2151の完全版と最適化版も一致した。CoreS3への最新版書き込み、10分実機連続ログ、利用者の聴取確認はまだ必要である。
- 上記修正後も利用者の聴取ではリズム不良が残った。固定ROMとbusy有無のA/Bを含む生event traceを再調査し、通常打音の正規化開始間隔が18,426〜20,306 samples（幅1,880、約42.6 ms）であること、同期・deferred実行のCPU/memory/framebuffer/audio CRCが一致することから、DMA、JIT、YM busyではなくROMのmain-loop内処理量に由来する発音jitterと判定した。ROMと生event timestampは維持し、最初の4区間で周期を学習するbounded PLLを自動演奏時の音響consumerだけへ追加した。2,048 sample lookahead、最大1,536 sample補正、周期EWMA 1/64、位相追従1/16で、3,500 frameの補正後間隔は18,779〜19,066 samples（幅287、約6.5 ms）となった。137 one-shot、FIFO starvation/alignment failure 0、STOP→PLAY 0 sample、CH2/ハイハット/ベースoffset最大147/11/74 samples、raw PCM/ADPCM hash不変、YM2151完全版とのbit一致を確認した。非計測autoplayをCoreS3へhash検証付きで書き込み済みだが、利用者の再聴取と10分実機連続ログは未完了である。
- bounded PLL版に対し、利用者からfillの「タン・タタ」の後半に無理があるとの指摘を受けた。ADPCM開始だけを補正し、その間を刻むYM CH7 noiseハイハットを生時刻に残していたため、ADPCM間隔だけの検証では見えない細分pulseの伸縮が生じていた。ADPCMと64 samples以内のCH7 key-onを同一打音にcluster化し、それ以外を含む全パーカッション間隔を3,500 frameで測定すると、旧補正は3,200〜6,878 samples（幅3,678、約83.4 ms）だった。CH7のkey-on、key-off、noise/operator設定をADPCM streaming feedと同じ音響shiftへ載せる候補は4,322〜5,371 samples（幅1,049、約23.8 ms）で、生eventの幅1,213 samplesも下回った。raw PCM/ADPCM hash、YM2151完全版とのbit一致、FIFO starvation/alignment failure 0を維持し、非計測autoplayをCoreS3へhash検証付きで書き込んだ。新候補の利用者聴取と10分実機連続ログは未完了である。
- 固定KANTAN ROMの`tools/adpcm.py`は時間順の2 codeを`first | (second << 4)`でpackし、ROM側`adpcm_feed()`は`uint8_t`配列を1 byteずつFIFOへ転送する。現行decoderも`nibbleHi=false`から下位4 bitを先に復号してから上位4 bitへ進む。MAMEのMSM6258参照実装もshift 0から4へ進むため、`0xAB`の時間順は`B → A`で一致する。16 bit以上の値をbyte列へ再解釈する経路はなく、CPUのエンディアンはこの転送へ影響しない。一方、既存の同期/deferred parityは同じdecoder同士の比較なので、ニブル順を独立に保証する既知ベクトルは今後追加する。
- 2026-08-25に`just test-chromatic-audio 3500`を再実行し、2,589,979 samples・160,240 eventsで`max_diff=0`、late event/FIFO starvation/alignment failure 0、補正後CH2→ADPCM最大9 samples、ADPCM→hat最大11 samples、ADPCM→bass最大74 samples、STOP→PLAY最大0 sample、再トリガー段差最大0 LSBを確認した。補正後パーカッション間隔は4,629〜4,902 samples（幅273）だった。続けて`just --fmt --check`、`just check`、CoreS3通常版`just build`を通し、firmwareは静的RAM 255,308 B（77.9%）、Flash 655,861 B（10.0%）だった。実機の再聴取と10分連続ログは未完了である。
- main push後のGitHub Actions run `32742629269`では`nix flake check`と音響検証まで通ったが、Linuxの`clang-tidy`が通常Clangのlibstdc++探索と明示したlibc++ headerを混在させ、標準`math.h`で160 errorsとなった。さらにhost検証の配列長計算が`bugprone-sizeof-expression`に違反した。devShellのcompilerを`llvmPackages.libcxxClang`へ統一し、配列長を`std::size`へ変更した候補で、macOS上の`just fmt-check`、`nix flake check`、`nix develop --command just clang-tidy`が成功した。Linux CIでの再検証は修正push後に行う。
- 修正run `32743108266`でもLinux clang-tidyはホストGCCの既定C++ headerを追加し、libc++との混在が残った。Nixのcompiler package変更だけではcompile databaseをclang-tidyが再解釈する際の既定探索を制御できないため、CMake生成時のcompile flagsへ`-nostdinc++`を追加し、`GB_CLANG_CXX_INCLUDE`で明示したlibc++だけを使わせる方針へ訂正した。この候補はmacOS Nix devShellで8 translation unitsのclang-tidyを通過した。Linux CIでの再検証は再push後に行う。
- mainへ再pushしたGitHub Actions run `32743523810`は4分55秒で成功した。Linux上の`nix flake check`と`nix develop --command just ci`がともに通過し、`-nostdinc++`によるlibc++ header分離が実CIでも有効であること、およびADPCM修正を含むmainの全CI検証が完了したことを確認した。
- 「ADPCM再生が追いつかない」という利用者報告を、修正前firmwareの実機ログ（`device-20260824-223034.jsonl`、GB_PROFILE + autoplay、243秒・14,612 frame）で切り分けた。`adpcm_fifo_rejected`、`alignment_failures`、`audio_underruns`、`audio_dropped`、`speaker_queue_failures`はすべて0であり、転送欠落でも整列失敗でもなかった。真の原因はCore 1の生成不足である。同ログのfpsは54.35〜62.75で、1秒窓ごとの生成sample数は最小40,441に落ち、44,100 samples/sに対する累積不足は最悪5,504 samples（約125 ms）に達した。`speaker_queue_empty`は起動直後の1回から62回まで増え続けており、speaker側が実際に枯渇していた。fpsが落ちる窓はcore時間も伸びており（cpu 4.0→7.5 ms、ppu 2.7→3.7 ms、apu 2.1→2.8 ms）、KANTAN内蔵デモのfill区間が16.74 ms予算をほぼ使い切ることが不足の実体だった。
- 適応resampler rateは音程を変調するため再採用しない（2026-08-24の訂正を維持）。代わりにwall-clock pacingを条件付きにし、FM選択時はPCM ring＋mix ringの合計が`AUDIO_TARGET_LEAD_SAMPLES`未満の間はフレーム末尾の`delayMicroseconds`を省いて`nextFrameUs`を実時刻へ引き戻す。speakerは44,100 Hz固定のまま消費し、貯金が満ちれば通常の待ちに戻る。mix ringは2,048から8,192 samplesへ拡大した。
- 目標貯金8,192 samplesの候補をCoreS3へ書き込み、300秒・18,872 frameを計測した。`speaker_queue_empty`は5分で6回まで減った（修正前は同じ長さで約20回相当、243秒で62回）が0にはならず、ringは2,917〜9,071 samplesで、1秒窓の最小生成は37,823 samplesだった。renderer が`ALIGNMENT_LOOKAHEAD`の2,048 samplesを常に保留するため、貯金8,192では利用可能分が出力chunk（512 samples）を割る窓が残ると判断した。
- 目標貯金を14,336 samplesへ引き上げた候補を書き込み、同条件で300秒・18,083 frameを計測した。`speaker_queue_empty`は起動直後の1回から一度も増えず、ringは6,009〜15,069 samples、`audio_underruns`、`audio_dropped`、`audio_output_clips`、`adpcm_fifo_rejected`、`alignment_failures`、`speaker_queue_failures`、`event_backpressure`はすべて0だった。1秒窓の最小生成は39,252 samplesと不足自体は残るが、貯金が吸収しきっている。`audio_backpressure`は1,879,720まで増えたが、これはCore 1が先行生成してPCM ringが満杯のときの待機回数であり、sample欠落ではなく意図した流量制御である。
- 同じ修正をホスト側の波形でも確認した。KANTAN内蔵デモ3,500 frameを同期rendererとCoreS3相当（deferred producer + aligner + DC blocker + 0.9 gain）で16-bit monoへ書き出し、演奏中に前後が有音のまま5 msのRMSが無音へ落ちる区間は両者とも0件だった。整列版の最大隣接sample差は8,517で、その近傍はADPCMアタックの立ち上がりであり不連続なクリックではない。秒ごとのRMS比（整列版/同期版）は1.173〜1.282、平均1.207で、ADPCM mix gain 0.75による意図した増分と一致する。
- 上記の実機計測は`GB_PROFILE`付きautoplayビルドで行った。過去の記録どおり命令単位cycle計測は約5 fpsの摂動を持つため、これらのfps値をそのまま製品性能として扱わない。非計測版autoplay firmware（静的RAM 267,596 B / 81.7%、Flash 656,013 B / 10.0%）を書き込み済みで、利用者の聴取確認は未完了である。
- 実機ログ取得中、`just logs`が3回連続で0 byteになった。修正前HEADのfirmwareでも同じだったため変更起因ではない。`esptool --no-stub --after hard_reset`でMACを読んでリセットすると復旧したので、原因はfirmwareではなく直前の書き込み後にUSB-Serial/JTAGがdownload状態に留まったことである。0 byteログを見たらまずhard resetをかける。

[^local-gb-core]: 現行 `core/` の 2026-08-24 時点のソース調査。
[^nes-stackchan-port]: NES 移植例の `core/`、`m5stack/`、ホスト比較 harness、性能関連コミットの調査。
[^cores3-docs]: CoreS3 の ESP32-S3、LCD、PSRAM、microSD、内蔵 speaker に関するハードウェア資料。
[^kantan-gb-play]: KANTAN GB PLAYのREADME、固定コミットのROM、および取得後のGame Boy headerとSHA-256の調査。
[^m5unified-stackchan]: M5Unified公式release 0.2.15と、同タグの`M5Unified.cpp`、`M5Unified.hpp`、`Power_Class.cpp`の調査。
