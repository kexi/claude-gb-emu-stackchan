# Game Boy / Game Boy Color emulator tasks

set unstable := true

# 利用可能なレシピを表示する
default:
    @just --list

# 開発環境と hook を初期化する
setup:
    lefthook install --force
    just doctor

# 主要ツールの版を表示する
doctor:
    @clang++ --version | head -n 1
    @emcc --version | head -n 1
    @pio --version
    @just --version
    @lefthook version

# Nix の入力を明示的に更新する
update:
    nix flake update

# 生成物を確認付きで削除する
clean confirm='':
    #!/bin/bash
    set -euo pipefail
    if [[ "{{ confirm }}" != "yes" ]]; then
        echo "実行するには just clean yes を指定してください" >&2
        exit 2
    fi
    rm -rf m5stack/.pio .stackchan/diagnostics

# 新規 CoreS3 / host harness と Nix ファイルを整形する
fmt:
    clang-format -i core/chromatic.cpp core/chromatic.h
    rg --files m5stack/src tools -g '*.cpp' -g '*.h' | xargs clang-format -i
    ruff format tools/capture_serial.py
    nixfmt flake.nix
    just --fmt --unstable

# 新規 CoreS3 / host harness、Nix、justfile の整形を検査する
fmt-check:
    clang-format --dry-run --Werror core/chromatic.cpp core/chromatic.h
    rg --files m5stack/src tools -g '*.cpp' -g '*.h' | xargs clang-format --dry-run --Werror
    ruff format --check tools/capture_serial.py
    ruff check tools/capture_serial.py
    nixfmt --check flake.nix
    just --fmt --check

# Web / 組込みコアを構文検査する
lint:
    clang++ -std=c++17 -fsyntax-only core/*.cpp core/ymfm/ymfm_opm.cpp
    clang++ -std=c++17 -DGB_EMBEDDED -fsyntax-only core/*.cpp core/ymfm/ymfm_opm.cpp

# 実コンパイル設定から database を生成し、共有組込みコアを clang-tidy で検査する
clang-tidy:
    #!/bin/bash
    set -euo pipefail
    cmake -S . -B .stackchan/cmake -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DCMAKE_CXX_FLAGS=${NIX_CFLAGS_COMPILE:-}" "-DCMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES=${GB_CLANG_CXX_INCLUDE:?run inside nix develop};${GB_CLANG_RESOURCE_INCLUDE:?run inside nix develop}"
    clang-tidy -p .stackchan/cmake core/apu.cpp core/cartridge.cpp core/chromatic.cpp core/cpu.cpp core/gb.cpp core/ppu.cpp tools/verify_host.cpp tools/verify_chromatic_audio.cpp

# 参照版と組込み高速化版を決定的に比較する
test frames='180':
    #!/bin/bash
    set -euo pipefail
    work_dir=$(mktemp -d)
    trap 'rm -rf "$work_dir"' EXIT
    clang++ -std=c++17 -O2 -o "$work_dir/ref" tools/verify_host.cpp core/*.cpp core/ymfm/ymfm_opm.cpp
    clang++ -std=c++17 -O2 -DGB_EMBEDDED -o "$work_dir/embedded" tools/verify_host.cpp core/*.cpp core/ymfm/ymfm_opm.cpp
    "$work_dir/ref" "{{ frames }}" > "$work_dir/ref.txt"
    "$work_dir/embedded" "{{ frames }}" > "$work_dir/embedded.txt"
    diff -u "$work_dir/ref.txt" "$work_dir/embedded.txt"
    cat "$work_dir/ref.txt"

# 日常的なホスト検査をまとめて実行する
check:
    just --fmt --check
    just lint
    just test

# 内蔵KANTAN GBを参照版と組込み版で実行し、描画と状態一致を検査する
test-rom frames='180': rom
    #!/bin/bash
    set -euo pipefail
    work_dir=$(mktemp -d)
    trap 'rm -rf "$work_dir"' EXIT
    clang++ -std=c++17 -O2 -o "$work_dir/ref" tools/verify_host.cpp core/*.cpp core/ymfm/ymfm_opm.cpp
    clang++ -std=c++17 -O2 -DGB_EMBEDDED -o "$work_dir/embedded" tools/verify_host.cpp core/*.cpp core/ymfm/ymfm_opm.cpp
    "$work_dir/ref" "{{ frames }}" m5stack/data/kantan-gb-play.gbc > "$work_dir/ref.txt"
    "$work_dir/embedded" "{{ frames }}" m5stack/data/kantan-gb-play.gbc > "$work_dir/embedded.txt"
    diff -u "$work_dir/ref.txt" "$work_dir/embedded.txt"
    cat "$work_dir/ref.txt"

# KANTAN GBのYM2151経路を参照版とCoreS3版で決定的に比較する
test-rom-fm frames='180': rom
    #!/bin/bash
    set -euo pipefail
    work_dir=$(mktemp -d)
    trap 'rm -rf "$work_dir"' EXIT
    clang++ -std=c++17 -O2 -o "$work_dir/ref" tools/verify_host.cpp core/*.cpp core/ymfm/ymfm_opm.cpp
    clang++ -std=c++17 -O2 -DGB_EMBEDDED -o "$work_dir/embedded" tools/verify_host.cpp core/*.cpp core/ymfm/ymfm_opm.cpp
    "$work_dir/ref" "{{ frames }}" m5stack/data/kantan-gb-play.gbc --fm > "$work_dir/ref.txt"
    "$work_dir/embedded" "{{ frames }}" m5stack/data/kantan-gb-play.gbc --fm > "$work_dir/embedded.txt"
    diff -u "$work_dir/ref.txt" "$work_dir/embedded.txt"
    cat "$work_dir/ref.txt"

# KANTAN内蔵デモの生イベント一致とCoreS3音響整列をsample単位で検証する
test-chromatic-audio frames='3500' maximum_chord_play_offset='-1': rom
    #!/bin/bash
    set -euo pipefail
    work_dir=$(mktemp -d)
    trap 'rm -rf "$work_dir"' EXIT
    clang++ -std=c++17 -O2 -DGB_EMBEDDED -o "$work_dir/verify-audio" tools/verify_chromatic_audio.cpp core/*.cpp core/ymfm/ymfm_opm.cpp
    "$work_dir/verify-audio" m5stack/data/kantan-gb-play.gbc "{{ frames }}" "{{ maximum_chord_play_offset }}"

# YM2151高速化版のPCMを全チャンネル更新の参照版とbit単位で比較する
test-ym2151-optimization frames='900' maximum_chord_play_offset='64': rom
    #!/bin/bash
    set -euo pipefail
    work_dir=$(mktemp -d)
    trap 'rm -rf "$work_dir"' EXIT
    sources=(tools/verify_chromatic_audio.cpp core/*.cpp core/ymfm/ymfm_opm.cpp)
    clang++ -std=c++17 -O2 -DGB_EMBEDDED -DYMFM_PARTIAL_CACHE_INVALIDATION=0 -DYMFM_SKIP_INACTIVE_CHANNEL_CLOCKS=0 -o "$work_dir/reference" "${sources[@]}"
    clang++ -std=c++17 -O2 -DGB_EMBEDDED -o "$work_dir/optimized" "${sources[@]}"
    "$work_dir/reference" m5stack/data/kantan-gb-play.gbc "{{ frames }}" "{{ maximum_chord_play_offset }}" > "$work_dir/reference.txt"
    "$work_dir/optimized" m5stack/data/kantan-gb-play.gbc "{{ frames }}" "{{ maximum_chord_play_offset }}" > "$work_dir/optimized.txt"
    diff -u "$work_dir/reference.txt" "$work_dir/optimized.txt"
    cat "$work_dir/optimized.txt"

# KANTAN GBのADPCM開始とYM2151キーオンをsample位置付きで記録する
trace-rhythm frames='700': rom
    #!/bin/bash
    set -euo pipefail
    work_dir=$(mktemp -d)
    trap 'rm -rf "$work_dir"' EXIT
    clang++ -std=c++17 -O2 -DGB_EMBEDDED -o "$work_dir/trace" tools/verify_host.cpp core/*.cpp core/ymfm/ymfm_opm.cpp
    "$work_dir/trace" "{{ frames }}" m5stack/data/kantan-gb-play.gbc --fm --trace-rhythm

# WebAssembly 版をビルドする
build-web:
    ./build.sh

# M5Stack CoreS3 ファームウェアをビルドする
build: rom
    cd m5stack && pio run -e m5stack-cores3

# SHA-256 を検証して内蔵 KANTAN GB ROM を取得する
rom:
    bash ./m5stack/scripts/fetch-rom.sh

# 計測付き M5Stack CoreS3 ファームウェアをビルドする
build-profile: rom
    cd m5stack && pio run -e m5stack-cores3-profile

# KANTAN GB PLAY の内蔵デモを自動開始する診断版をビルドする
build-autoplay: rom
    cd m5stack && pio run -e m5stack-cores3-autoplay

# KANTAN GB PLAY のFM内蔵デモを計測付きで自動開始する診断版をビルドする
build-profile-autoplay: rom
    cd m5stack && pio run -e m5stack-cores3-profile-autoplay

# M5Stack CoreS3 へ通常版を書き込む（既存 firmware を置換するため yes 必須）
flash port confirm='':
    #!/bin/bash
    set -euo pipefail
    if [[ "{{ confirm }}" != "yes" ]]; then
        echo "書き込むには just flash <port> yes を指定してください" >&2
        exit 2
    fi
    bash ./m5stack/scripts/fetch-rom.sh
    cd m5stack
    pio run -e m5stack-cores3 -t upload --upload-port "{{ port }}"

# M5Stack CoreS3 へ計測版を書き込む（既存 firmware を置換するため yes 必須）
flash-profile port confirm='':
    #!/bin/bash
    set -euo pipefail
    if [[ "{{ confirm }}" != "yes" ]]; then
        echo "書き込むには just flash-profile <port> yes を指定してください" >&2
        exit 2
    fi
    bash ./m5stack/scripts/fetch-rom.sh
    cd m5stack
    pio run -e m5stack-cores3-profile -t upload --upload-port "{{ port }}"

# KANTAN GB PLAY の内蔵デモを自動開始する診断版を書き込む（既存 firmware を置換するため yes 必須）
flash-autoplay port confirm='':
    #!/bin/bash
    set -euo pipefail
    if [[ "{{ confirm }}" != "yes" ]]; then
        echo "書き込むには just flash-autoplay <port> yes を指定してください" >&2
        exit 2
    fi
    bash ./m5stack/scripts/fetch-rom.sh
    cd m5stack
    pio run -e m5stack-cores3-autoplay -t upload --upload-port "{{ port }}"

# KANTAN GB PLAY のFM内蔵デモを計測付きで自動開始して書き込む（既存 firmware を置換するため yes 必須）
flash-profile-autoplay port confirm='':
    #!/bin/bash
    set -euo pipefail
    if [[ "{{ confirm }}" != "yes" ]]; then
        echo "書き込むには just flash-profile-autoplay <port> yes を指定してください" >&2
        exit 2
    fi
    bash ./m5stack/scripts/fetch-rom.sh
    cd m5stack
    pio run -e m5stack-cores3-profile-autoplay -t upload --upload-port "{{ port }}"

# M5Stack CoreS3 のシリアルログを表示する
monitor port='':
    pio device monitor -b 115200 {{ if port == '' { '' } else { '-p ' + port } }}

# CoreS3 のシリアルログを指定秒数だけ表示・保存する
logs port duration='30' reset='no':
    #!/bin/bash
    set -euo pipefail
    if [[ "{{ reset }}" != "yes" && "{{ reset }}" != "no" ]]; then
        echo "reset は yes または no で指定してください" >&2
        exit 2
    fi
    mkdir -p .stackchan/diagnostics
    output=".stackchan/diagnostics/device-$(date +%Y%m%d-%H%M%S).jsonl"
    python3 tools/capture_serial.py {{ if reset == 'yes' { '--reset' } else { '' } }} "{{ port }}" "{{ duration }}" 2>&1 | tee "$output"
    echo "保存先: $output"

# 診断情報と検査結果をローカルへ保存する
diagnose:
    #!/bin/bash
    set -euo pipefail
    mkdir -p .stackchan/diagnostics
    output=".stackchan/diagnostics/diagnose-$(date +%Y%m%d-%H%M%S).log"
    { git rev-parse HEAD; git status --short; just doctor; just check; } 2>&1 | tee "$output"

# staged secrets を検査する
gitleaks-staged:
    gitleaks git --pre-commit --staged --config .gitleaks.toml --redact --verbose

# Git 履歴全体の secrets を検査する
gitleaks:
    gitleaks git --config .gitleaks.toml --redact --verbose

# workflow の action pin を更新する
pinact:
    pinact run

# workflow の action pin をオフライン検査する
pinact-check:
    pinact run --fix=false --no-api

# workflow の version コメントと最小公開期間を検査する
pinact-verify:
    pinact run --fix=false --verify-comment --verify-min-age

# GitHub Actions workflow を検査する
actionlint:
    actionlint

# justfile の整形を検査する
justfile-fmt-check:
    just --fmt --check

# CI と同じ検査をまとめて実行する
ci:
    just gitleaks
    just pinact-verify
    just actionlint
    just fmt-check
    just check
    just test-rom
    just test-chromatic-audio 900 64
    just test-ym2151-optimization 900 64
    just clang-tidy
    just build-web
    just build
