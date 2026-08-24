# コントリビューションガイド

🌐 [English](CONTRIBUTING.md)

## 開発環境

このプロジェクトが必要とするツール（コンパイラ、Emscripten、PlatformIO、linter、formatter）はすべて `flake.nix` に固定されています。自分でインストールするのは Nix だけです。Homebrew やシステムのパッケージマネージャーで個別に入れると、CI が使う版とずれるため避けてください。

### 1. flakes を有効にした Nix を入れる

Nix が未導入の場合:

```sh
curl --proto '=https' --tlsv1.2 -sSf -L https://install.determinate.systems/nix | sh -s -- install
```

Determinate Systems のインストーラは flakes と `nix-command` を既定で有効にし、CI が使っているものと同じです。既存の Nix がある場合は `~/.config/nix/nix.conf` に次を追記してください。

```
experimental-features = nix-command flakes
```

### 2. 開発シェルに入る

```sh
nix develop
```

これで必要なツールがすべて `PATH` に入ります。リポジトリに `cd` したときに自動で読み込ませたい場合は [direnv](https://direnv.net/) を入れて次を実行します。

```sh
direnv allow
```

`.envrc` は `use flake` の 1 行だけなので、direnv と `nix develop` は同一の環境になります。

### 3. セットアップを確認する

```sh
just doctor
```

`clang++`、`emcc`、`pio`、`just`、`lefthook` の版が表示されます。どれかが見つからない場合は開発シェルの外にいます。

### シェルが自動で行うこと

シェルに入ると `shellHook` が走り、`PATH` を通す以外に次の 3 つを行います。

- lefthook の pre-commit hook を導入します（`lefthook install --force`）。以後のコミット前に gitleaks、clang-format、actionlint、pinact が走ります。
- `~/.cache/emscripten-<version>` に書き込み可能な Emscripten キャッシュを用意します。Nix store 側は読み取り専用のためです。
- `GB_CLANG_CXX_INCLUDE` と `GB_CLANG_RESOURCE_INCLUDE` を export します。`just clang-tidy` は `-nostdinc++` でコンパイルし、clang-tidy にこれらのヘッダだけを見せます。これがないとホストの libstdc++ が libc++ ビルドへ混入し、`<math.h>` で大量の誤検出が出ます。`just clang-tidy` を開発シェル内で実行しなければならないのはこのためです。

## タスクランナー

すべてのコマンドは [just](https://github.com/casey/just) 経由です。引数なしで `just` を実行すると、説明付きで全レシピが一覧表示されます。

### 日常の作業

```sh
just check   # justfile の整形、ホスト構文検査、参照版と組込み版の一致検証
just fmt     # C++、Python、Nix、justfile をその場で整形
```

`just check` は速いので、実装中はこれを回してください。`just fmt-check` はその読み取り専用版で、CI が実行します。

### push の前に

```sh
just ci
```

GitHub Actions と同じ内容を実行します。secret 走査、action の pin 検証、actionlint、整形検査、ホスト一致、ROM 一致、Chromatic 音響検証、YM2151 最適化比較、clang-tidy、WebAssembly ビルド、CoreS3 ファームウェアビルドの順で、約 5 分かかります。

### ビルド

```sh
just build-web   # WebAssembly → web/gbc.js + web/gbc.wasm
just build       # M5Stack CoreS3 ファームウェア
```

CoreS3 のビルドは最初に `m5stack/scripts/fetch-rom.sh` を実行し、`m5stack/rom.lock` で固定したコミットから KANTAN GB PLAY の ROM を取得して、SHA-256 が一致した場合だけ埋め込みます。ROM バイナリは意図的にこのリポジトリへ含めていません。

### 実機を使う

```sh
just flash <port> yes            # 接続中の CoreS3 のファームウェアを置き換える
just logs <port> [秒数] [yes]     # JSONL の診断ログを .stackchan/diagnostics/ へ保存する
```

`flash` は現在のファームウェアを上書きするため、明示的な `yes` が必要です。`logs` の末尾に `yes` を付けると先にリセットし、起動から記録します。

`just logs` が 0 バイトのファイルを作る場合、ファームウェアの異常ではなく、直前の書き込み後に USB-Serial/JTAG が download モードに留まっているのが通例です。リセットしてからやり直してください。

```sh
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --no-stub --port <port> --after hard_reset chip_id
```

## コーディングスタイル

整形と lint は強制されるので、手で整えずツールの出力に合わせてください。

| 言語 | Formatter | Linter |
| --- | --- | --- |
| C++ | clang-format（LLVM ベース、インデント 4、120 桁） | clang-tidy（`WarningsAsErrors: '*'`） |
| Python | ruff format | ruff |
| Nix | nixfmt-rfc-style | `nix flake check` |
| justfile | `just --fmt` | 各レシピの直前行に説明コメントが必須 |
| GitHub Actions | — | actionlint、pinact（SHA pin、最小公開期間 1 日） |

linter が検査できない規約が 2 つあります。

- **条件式に名前を付ける。** `if` の中に式を埋め込まず、`const bool isRhythmic = ...; if (isRhythmic)` と書きます。
- **コメントには「なぜ他の方法を採らなかったか」を書く。** 何をしているかはコードが示すので、コメントは却下した代替案とその理由を残すことで価値を持ちます。例: `// Why not an adaptive resampler rate: it turns frame jitter into audible pitch wobble.`

## コミットメッセージ

[Semantic Commit Messages](https://www.conventionalcommits.org/) に従います（`fix(audio): ...`、`feat(ppu): ...`、`docs(knowledge): ...`）。

本文には**なぜ**その変更をしたかを書きます。何を観測し、計測が何を示し、なぜその方法が代替案より良かったかです。何を変えたかは差分から読めますが、理由は差分から復元できません。

## 検証と knowledge

このプロジェクトでは計測を変更の一部として扱い、後追いにしません。コアはブラウザ版と CoreS3 版で共有されているため、もっともらしい最適化が片方の出力だけを静かに変えることがあります。

- `just test` と `just test-rom` は参照版と `GB_EMBEDDED` 版をフレーム単位で比較し、CPU、WRAM、VRAM、OAM、framebuffer、音声の CRC が完全一致することを要求します。
- `just test-chromatic-audio` は deferred な YM2151 / ADPCM イベントを同期レンダラーと突き合わせ、`max_diff=0` を要求します。

`knowledge/` には設計判断と計測記録を OKF（Open Knowledge Format、Markdown + YAML frontmatter）で置いています。作業を始める前に `knowledge/index.md` と該当文書を読んでください。一見当然に見える手法のいくつかは既に試され、理由となる計測とともに撤回されています。

計測が得られたとき、または過去の結論が誤りだと分かったときは、そこへ記録してください。失敗した試みも残します。「試したが悪化した、数値はこれ」という記録が最も価値が高く、消すと次の人が同じ道を辿ります。tag は `knowledge/tags.yml` の統制語彙だけを使い、新しい tag は説明を添えて先に追加してください。

## プルリクエスト

開く前に `just ci` がローカルで通ることを確認してください。実機のタイミング、音声、描画に影響する変更には、根拠となる実機計測を添えてください。ログの抜粋や `just logs` のカウンタで十分です。
