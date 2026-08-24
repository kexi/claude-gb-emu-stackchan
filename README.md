# claude-gb-emu

A Game Boy / Game Boy Color emulator for browsers and the ESP32-S3-based M5Stack CoreS3. Its shared core is written in C++; the web build compiles it to WebAssembly with Emscripten.

**▶ Play: https://goroman.github.io/claude-gb-emu/**

Open a .gb / .gbc file with "Open ROM" or drop it onto the screen. The "URL" button loads a ROM directly from a URL (also via the `?rom=<URL>` parameter; the host must allow CORS).

🌐 [日本語](README.ja.md)

## Features

### Emulation core (C++ / WASM)
- **SM83 (LR35902) CPU** — all instructions implemented. Passes all 11 tests of blargg's cpu_instrs and instr_timing
- **PPU** — scanline renderer. CGB palette RAM / VRAM banks / BG attributes / OBJ priority, DMG OBJ X-coordinate priority. Passes dmg-acid2 / cgb-acid2
- **APU** — 4 channels (2 pulse + wave + noise) with sweep, envelope and length counters. AudioWorklet output (falls back to ScriptProcessor on non-HTTPS origins)
- **MBC** — none / MBC1 / MBC2 / MBC3 (+RTC) / MBC5
- **CGB features** — double speed mode (KEY1), HDMA (general / HBlank), WRAM/VRAM banking
- **Chromatic FM expansion** (FM button) — emulates the [ModRetro Chromatic](https://github.com/ModRetro/oss-chromatic-console-fpga) YM2151 + MSM6258 ADPCM extension mapped at $FF28-$FF2F (YM2151 via [ymfm](https://github.com/aaronsgiles/ymfm), BSD-3-Clause). For ROMs written for that hardware; toggled with the FM button
- Battery-backed SRAM is saved to localStorage automatically

### Frontend
- Keyboard: D-pad = arrows / A = X / B = Z / START = Enter / SELECT = Shift / R = reset / F = fullscreen / D = debug panel
- USB gamepads (Gamepad API)
- Virtual touch pad on touch devices
- Controls guide on the left; debug panel (CPU registers, APU registers, memory dump) on the right, toggled with the DEBUG button or the D key

## Build

### M5Stack CoreS3 / Stack-chan

Power-on boots the firmware-embedded [KANTAN GB PLAY](https://github.com/GOROman/kantan-gb-play) directly without a microSD card. The build fetches the ROM from a pinned commit and verifies its SHA-256 digest; the ROM itself is not committed here. Optional `.gb` / `.gbc` files can be placed in `/roms` on a FAT32 microSD card and selected from the menu by holding BtnC.

```sh
direnv allow          # or: nix develop
just check
just build
just flash <port> yes # only when a device is connected and its firmware should be replaced
```

All tooling is pinned in `flake.nix`, so Nix is the only prerequisite. See [CONTRIBUTING.md](CONTRIBUTING.md) for the full setup, the task list, and the verification workflow.

The frontend supports the display, speaker, microSD, built-in touch buttons, Grove Joystick, and Dual Button unit. See [m5stack/README.md](m5stack/README.md) for wiring, controls, and performance logs, and [knowledge/stackchan-port-architecture.md](knowledge/stackchan-port-architecture.md) for design decisions and verification criteria.

### WebAssembly

```sh
./build.sh   # requires emscripten; outputs web/gbc.js + web/gbc.wasm
```

Run locally through a web server:

```sh
cd web && python3 -m http.server 8080
# → http://localhost:8080/
```

## Deploy (GitHub Pages)

The site is served from the `gh-pages` branch, which holds the contents of `web/`. To update:

```sh
./build.sh
git subtree split --prefix web -b gh-pages-tmp
git push -f origin gh-pages-tmp:gh-pages
git branch -D gh-pages-tmp
```

## Structure

```
core/gb.h          shared definitions
core/cpu.cpp       SM83 CPU
core/ppu.cpp       PPU (DMG + CGB)
core/apu.cpp       APU
core/cartridge.cpp MBC / cartridge
core/gb.cpp        bus, timer, DMA, WASM API
web/               frontend (index.html / main.js / audio-worklet.js)
m5stack/           CoreS3 frontend and PlatformIO configuration
tools/             deterministic reference-versus-embedded harness
knowledge/         architecture, performance budgets, and verification records
```

## License

MIT
