# claude-gb-emu-stackchan

ブラウザと M5Stack CoreS3（ESP32-S3）で動く Game Boy / Game Boy Color エミュレータ。共有コアは C++ で、Web 版は Emscripten で WebAssembly にコンパイルします。

**▶ Play: https://goroman.github.io/claude-gb-emu/**

「ROMを開く」から .gb / .gbc ファイルを開くか、画面にドロップしてください。「URL」ボタンで ROM の URL を直接指定することもできます(`?rom=<URL>` パラメータにも対応、CORS 許可が必要)。

🌐 [English](README.md)

## このフォークで追加したもの

これはブラウザ向けの [GOROman/claude-gb-emu](https://github.com/GOROman/claude-gb-emu) のフォークです。Web 版はそのまま動く状態を保ちつつ、同じコアを使った **M5Stack CoreS3 / Stack-chan 単体動作版**と、両方の正しさを保つための道具立てを追加しています。

- **CoreS3 ファームウェア**（`m5stack/`）— 実機上でエミュレータを動かします。LCD への 240×216 バンド DMA、内蔵スピーカーでの音声、microSD からの ROM 選択、`.sav` へのバッテリー SRAM 保存、Grove Joystick / Dual Button 入力に対応します。電源投入時は microSD なしで、ファームウェア内蔵の [KANTAN GB PLAY](https://github.com/GOROman/kantan-gb-play) を直接起動します。
- **共有コアの組込み向け高速化** — すべて `GB_EMBEDDED` の内側にあるため、ブラウザ版の出力は 1 バイトも変わりません。HALT 期限までの fast-forward、CGB パレットキャッシュ付きの PPU タイルラン描画、APU サンプルの整数化、LCD へ送らないフレームのピクセル生成省略などで、実機を約 9 fps から約 59.7 fps へ引き上げています。
- **実機での FM 音声の非同期化** — Core 1 がエミュレーションを行い、Core 0 が ymfm、ADPCM 復号、ミックス、リサンプル、I2S 投入を所有します。両者は PCM リングと時刻付き FM イベントリングだけを共有します。さらに音響アライナが ROM 側の `STOP → preload → PLAY` という ADPCM 投入順序を補正し、ドラムを拍に合わせます。
- **決定的な検証ハーネス**（`tools/`）— 同じ ROM を参照版と `GB_EMBEDDED` 版へ流し、CPU、WRAM、VRAM、OAM、framebuffer、音声の CRC が完全一致することを要求します。組込み向け最適化が出力を静かに変えることを防ぎます。
- **再現可能な環境と CI** — `flake.nix` によるツールの固定、`justfile` のコマンド面、lefthook の pre-commit hook、GitHub Actions での全検査。詳細は [CONTRIBUTING.ja.md](CONTRIBUTING.ja.md) を参照してください。
- **`knowledge/`** — 設計判断と計測記録。試したうえで撤回した最適化も、理由となる数値とともに残しています。

## Features

### エミュレーションコア (C++ / WASM)
- **SM83 (LR35902) CPU** — 全命令実装。blargg の cpu_instrs 全11項目・instr_timing をパス
- **PPU** — スキャンラインレンダラ。CGB のパレット RAM / VRAM バンク / BG 属性 / OBJ 優先度、DMG の OBJ X 座標優先度に対応。dmg-acid2 / cgb-acid2 をパス
- **APU** — パルス×2 + 波形 + ノイズの4ch。スイープ・エンベロープ・長さカウンタ対応。AudioWorklet 出力(非 HTTPS では ScriptProcessor にフォールバック)
- **MBC** — なし / MBC1 / MBC2 / MBC3(+RTC) / MBC5
- **CGB 機能** — 倍速モード (KEY1)、HDMA(汎用 / HBlank)、WRAM/VRAM バンク切り替え
- **Chromatic FM 拡張**(FM ボタン)— [ModRetro Chromatic](https://github.com/ModRetro/oss-chromatic-console-fpga) の YM2151 + MSM6258 ADPCM 拡張($FF28-$FF2F)をエミュレート(YM2151 は [ymfm](https://github.com/aaronsgiles/ymfm)、BSD-3-Clause)。対応 ROM 用。FM ボタンで ON/OFF
- バッテリーバックアップ SRAM は localStorage に自動保存

### フロントエンド
- キーボード: 十字キー=矢印 / A=X / B=Z / START=Enter / SELECT=Shift / R=リセット / F=フルスクリーン / D=デバッグパネル
- USB ゲームパッド対応 (Gamepad API)
- タッチデバイスでは画面上にタッチパッドを表示
- 左側に操作説明、右側にデバッグパネル(CPU レジスタ・APU レジスタ・メモリダンプ)。DEBUG ボタンか D キーで切り替え

## Build

### M5Stack CoreS3 / Stack-chan

電源投入すると、ファームウェア内蔵の [KANTAN GB PLAY](https://github.com/GOROman/kantan-gb-play) がSDカードなしで直接起動します。ビルド時に固定コミットからROMを取得してSHA-256を検証し、ROM自体はリポジトリへ含めません。任意の `.gb` / `.gbc` はFAT32 microSDの `/roms` に追加でき、BtnC長押しのメニューから選択できます。

```sh
direnv allow          # または nix develop
just check
just build
just flash <port> yes # 実機を接続し、既存 firmware を置換する場合だけ
```

ツール一式は `flake.nix` に固定してあるため、自分で入れるのは Nix だけです。環境構築、コマンド一覧、検証の進め方は [CONTRIBUTING.ja.md](CONTRIBUTING.ja.md) を参照してください。

画面、スピーカー、microSD、本体タッチに加え、Grove Joystick / Dual Button を使えます。配線、操作、性能ログは [m5stack/README.md](m5stack/README.md)、設計判断と検証条件は [knowledge/stackchan-port-architecture.md](knowledge/stackchan-port-architecture.md) を参照してください。

### WebAssembly

```sh
./build.sh   # 要 emscripten。web/gbc.js + web/gbc.wasm を生成
```

ローカル実行はWebサーバー経由で:

```sh
cd web && python3 -m http.server 8080
# → http://localhost:8080/
```

## Deploy (GitHub Pages)

`gh-pages` ブランチに `web/` の内容を置いて配信しています。更新は:

```sh
./build.sh
git subtree split --prefix web -b gh-pages-tmp
git push -f origin gh-pages-tmp:gh-pages
git branch -D gh-pages-tmp
```

## Structure

```
core/gb.h          共通定義
core/cpu.cpp       SM83 CPU
core/ppu.cpp       PPU (DMG + CGB)
core/apu.cpp       APU
core/cartridge.cpp MBC / カートリッジ
core/gb.cpp        バス・タイマー・DMA・WASM API
web/               フロントエンド (index.html / main.js / audio-worklet.js)
m5stack/           CoreS3 フロントエンド、PlatformIO 設定
tools/             参照版と組込み版の決定的比較 harness
knowledge/         設計判断、性能予算、検証記録
```

## License

MIT
