# Contributing

🌐 [日本語](CONTRIBUTING.ja.md)

## Development environment

Every tool this project needs — compilers, Emscripten, PlatformIO, linters, formatters — is pinned in `flake.nix`. Nix is the only prerequisite you install yourself; do not install the rest through Homebrew or a system package manager, because the versions would drift from what CI uses.

### 1. Install Nix with flakes enabled

If you do not already have Nix:

```sh
curl --proto '=https' --tlsv1.2 -sSf -L https://install.determinate.systems/nix | sh -s -- install
```

The Determinate Systems installer enables flakes and `nix-command` by default, and it is the same installer CI uses. With an existing Nix installation, add this to `~/.config/nix/nix.conf` instead:

```
experimental-features = nix-command flakes
```

### 2. Enter the development shell

```sh
nix develop
```

This drops you into a shell with everything on `PATH`. To have it load automatically whenever you `cd` into the repository, install [direnv](https://direnv.net/) and run:

```sh
direnv allow
```

`.envrc` is a one-line `use flake`, so direnv and `nix develop` give you the identical environment.

### 3. Verify the setup

```sh
just doctor
```

This prints the versions of `clang++`, `emcc`, `pio`, `just`, and `lefthook`. If any of them is missing you are outside the dev shell.

### What the shell sets up for you

Entering the shell runs a `shellHook` that does three things beyond putting tools on `PATH`:

- Installs the lefthook pre-commit hooks (`lefthook install --force`), so you get gitleaks, clang-format, actionlint, and pinact checks before every commit.
- Populates a writable Emscripten cache under `~/.cache/emscripten-<version>`, because the Nix store copy is read-only.
- Exports `GB_CLANG_CXX_INCLUDE` and `GB_CLANG_RESOURCE_INCLUDE`. `just clang-tidy` compiles with `-nostdinc++` and points clang-tidy at exactly these headers; without them a stray libstdc++ on the host gets mixed into a libc++ build and produces hundreds of spurious errors in `<math.h>`. This is why `just clang-tidy` must run inside the dev shell.

## Task runner

All commands go through [just](https://github.com/casey/just). Run `just` with no arguments to list every recipe with its description.

### Everyday loop

```sh
just check   # justfile format, host syntax check, and reference-versus-embedded parity
just fmt     # format C++, Python, Nix, and the justfile in place
```

`just check` is fast and is what you should run while iterating. `just fmt-check` is the read-only counterpart that CI runs.

### Before pushing

```sh
just ci
```

This runs exactly what GitHub Actions runs: secret scanning, action pinning verification, actionlint, format checks, host parity, ROM parity, the Chromatic audio verification, the YM2151 optimization comparison, clang-tidy, the WebAssembly build, and the CoreS3 firmware build. It takes about five minutes.

### Building

```sh
just build-web   # WebAssembly → web/gbc.js + web/gbc.wasm
just build       # M5Stack CoreS3 firmware
```

The CoreS3 build first runs `m5stack/scripts/fetch-rom.sh`, which downloads the KANTAN GB PLAY ROM from the commit pinned in `m5stack/rom.lock` and embeds it only if the SHA-256 matches. The ROM binary is deliberately not committed to this repository.

### Working with a device

```sh
just flash <port> yes            # replace the firmware on a connected CoreS3
just logs <port> [seconds] [yes] # capture JSONL diagnostics to .stackchan/diagnostics/
```

`flash` requires the literal `yes` because it overwrites whatever is on the device. A trailing `yes` on `logs` resets the board first so you capture from boot.

If `just logs` produces a zero-byte file, the USB-Serial/JTAG interface is usually still in download mode from the preceding flash rather than the firmware being broken. Reset it and try again:

```sh
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --no-stub --port <port> --after hard_reset chip_id
```

## Code style

Formatting and linting are enforced, so match what the tools produce rather than hand-formatting:

| Language | Formatter | Linter |
| --- | --- | --- |
| C++ | clang-format (LLVM base, 4-space indent, 120 columns) | clang-tidy (`WarningsAsErrors: '*'`) |
| Python | ruff format | ruff |
| Nix | nixfmt-rfc-style | `nix flake check` |
| justfile | `just --fmt` | every recipe needs a description comment above it |
| GitHub Actions | — | actionlint, pinact (SHA-pinned, minimum age 1 day) |

Two conventions the linters cannot check:

- **Name your conditions.** Write `const bool isRhythmic = ...; if (isRhythmic)` rather than burying the expression in the `if`.
- **Comments carry "why not".** The code already shows what it does; a comment earns its place by recording the alternative that was rejected and the reason. `// Why not an adaptive resampler rate: it turns frame jitter into audible pitch wobble.`

## Commit messages

Follow [Semantic Commit Messages](https://www.conventionalcommits.org/): `fix(audio): ...`, `feat(ppu): ...`, `docs(knowledge): ...`.

The body explains **why** the change was made — what was observed, what the measurement showed, and why the chosen approach beat the alternatives. Reviewers can read the diff for what changed; they cannot recover the reasoning from it.

## Verification and the knowledge base

This project treats measurement as part of the change, not a follow-up. The core is shared between the browser and CoreS3 builds, so a plausible-looking optimization can silently alter audio or video output on one target only.

- `just test` and `just test-rom` compare the reference build against the `GB_EMBEDDED` build frame by frame and require identical CPU, WRAM, VRAM, OAM, framebuffer, and audio CRCs.
- `just test-chromatic-audio` replays deferred YM2151/ADPCM events against the synchronous renderer and requires `max_diff=0`.

`knowledge/` holds the design decisions and the measurement record as OKF (Open Knowledge Format) documents — Markdown with YAML frontmatter that states what was verified and how. Before starting work, read `knowledge/index.md` and the relevant document; several approaches that look obvious have already been tried and withdrawn, with measurements explaining why.

When your work produces a measurement or overturns an earlier conclusion, record it there. Keep failed attempts: "this was tried and made things worse, here is the number" is the most valuable kind of entry, and deleting it invites the next person to repeat it. Tags come from the controlled vocabulary in `knowledge/tags.yml`; add new ones there with a description before using them.

## Pull requests

Before opening one, confirm `just ci` passes locally. Changes that affect timing, audio, or rendering on the device should include the device measurement that supports them — a log excerpt or the counters from `just logs` is enough.
