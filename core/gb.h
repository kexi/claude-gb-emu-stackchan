// Game Boy / Game Boy Color emulator core
#pragma once
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
#include "chromatic.h"

namespace gb {

// The browser consumes little-endian RGBA words. On CoreS3 the panel consumes
// big-endian RGB565, so the embedded core stores byte-swapped 565 directly and
// lets the frontend DMA it without a per-frame colour conversion.
#if defined(GB_EMBEDDED) && defined(ESP_PLATFORM)
using Pixel = uint16_t;
#else
using Pixel = uint32_t;
#endif

#ifdef ESP_PLATFORM
template <typename T>
struct PsramAllocator {
    using value_type = T;

    PsramAllocator() = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) {}

    T* allocate(size_t count) {
        void* storage = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const bool allocationFailed = storage == nullptr;
        if (allocationFailed) throw std::bad_alloc();
        return static_cast<T*>(storage);
    }
    void deallocate(T* storage, size_t) { heap_caps_free(storage); }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) {
    return true;
}

template <typename T, typename U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) {
    return false;
}

using ByteStorage = std::vector<uint8_t, PsramAllocator<uint8_t>>;
#else
using ByteStorage = std::vector<uint8_t>;
#endif

// Joypad button bits (as passed from JS)
enum {
    BTN_RIGHT = 0x01, BTN_LEFT = 0x02, BTN_UP = 0x04, BTN_DOWN = 0x08,
    BTN_A = 0x10, BTN_B = 0x20, BTN_SELECT = 0x40, BTN_START = 0x80,
};

// Interrupt flag bits
enum { INT_VBLANK = 0x01, INT_STAT = 0x02, INT_TIMER = 0x04, INT_SERIAL = 0x08, INT_JOYPAD = 0x10 };

struct GB;

// ---------------------------------------------------------------- Cartridge
struct Cartridge {
    ByteStorage rom;
    ByteStorage ram;
    int type = 0;          // raw byte at 0x147
    int mbc = 0;           // 0=none 1=MBC1 2=MBC2 3=MBC3 5=MBC5
    bool battery = false;
    bool hasRtc = false;
    int romBanks = 2;
    int ramBanks = 0;
    uint32_t ramGeneration = 0; // increments on SRAM writes; frontend decides when to persist

    // banking state
    bool ramEnable = false;
    int romBank = 1;
    int ramBank = 0;
    int mbc1Mode = 0;      // MBC1 banking mode
    int mbc1Hi = 0;        // MBC1 upper 2 bits

    // MBC3 RTC
    uint8_t rtcReg[5] = {0};       // S,M,H,DL,DH (latched values)
    uint8_t rtcLive[5] = {0};
    int rtcSelect = -1;            // selected RTC register (0x08-0x0C) or -1 = RAM
    uint8_t rtcLatchPrev = 0xFF;
    double rtcAccum = 0;           // fractional seconds

    bool load(const uint8_t* data, size_t size);
    bool load(ByteStorage&& data);
    bool configureLoadedRom();
    uint8_t read(uint16_t addr) const;
    void write(uint16_t addr, uint8_t v);
    void tickRtc(double seconds);
};

// ---------------------------------------------------------------- PPU
struct PPU {
    GB* gb = nullptr;
    Pixel framebuffer[160 * 144];

    uint8_t lcdc = 0x91, stat = 0x85, scy = 0, scx = 0;
    uint8_t ly = 0, lyc = 0, bgp = 0xFC, obp0 = 0xFF, obp1 = 0xFF, wy = 0, wx = 0;
    uint8_t bcps = 0, ocps = 0;
    uint8_t bgPal[64];             // CGB background palette RAM
    uint8_t objPal[64];            // CGB object palette RAM

    int dot = 0;                   // dot counter within current line (0-455)
    int windowLine = 0;            // internal window line counter
    bool frameDone = false;
    uint32_t frameCount = 0;
    bool statLine = false;         // for STAT interrupt edge detection

    void reset(bool cgb);
    void tick(int dots);           // advance by PPU dots (4.19MHz)
    void renderScanline();
    void checkStatIrq();
    Pixel cgbColor(const uint8_t* pal, int palIdx, int colorIdx) const;
};

// ---------------------------------------------------------------- APU
struct APU {
    GB* gb = nullptr;
    static const int MAX_SAMPLES = 4096;
    float sampleBuf[MAX_SAMPLES * 2];  // interleaved L,R
    int sampleCount = 0;
    double sampleRate = 44100;
    double sampleAccum = 0;

    uint8_t regs[0x30] = {0};      // FF10-FF3F shadow (wave RAM at 0x20-0x2F)
    bool enabled = true;

    // frame sequencer
    int fsClock = 0;               // cycles toward next 512Hz step
    int fsStep = 0;

    // channel 1 (pulse + sweep)
    bool ch1On = false; int ch1Timer = 0, ch1Duty = 0, ch1Pos = 0;
    int ch1Vol = 0, ch1EnvTimer = 0, ch1Len = 0;
    int ch1Freq = 0, ch1SweepTimer = 0, ch1ShadowFreq = 0; bool ch1SweepOn = false;
    bool ch1SweepNegUsed = false;
    // channel 2 (pulse)
    bool ch2On = false; int ch2Timer = 0, ch2Duty = 0, ch2Pos = 0;
    int ch2Vol = 0, ch2EnvTimer = 0, ch2Len = 0;
    // channel 3 (wave)
    bool ch3On = false; int ch3Timer = 0, ch3Pos = 0, ch3Len = 0;
    // channel 4 (noise)
    bool ch4On = false; int ch4Timer = 0, ch4Vol = 0, ch4EnvTimer = 0, ch4Len = 0;
    uint16_t ch4Lfsr = 0x7FFF;

    void reset();
    void setSampleRate(double r) { sampleRate = r; }
    uint8_t read(uint8_t reg) const;
    void write(uint8_t reg, uint8_t v);
    void tick(int cycles);         // 4.19MHz cycles
    void stepFrameSequencer();
    void mixSample();
    void trigger(int ch);
};

// ---------------------------------------------------------------- CPU
struct CPU {
    GB* gb = nullptr;
    uint16_t pc = 0x0100, sp = 0xFFFE;
    uint8_t a = 0x01, f = 0xB0, b = 0, c = 0x13, d = 0, e = 0xD8, h = 0x01, l = 0x4D;
    bool ime = false;
    int imeDelay = 0;              // EI enables IME after the next instruction
    bool halted = false;
    bool haltBug = false;

    void reset(bool cgb);
    int step();                    // execute one instruction, return T-cycles (CPU clock)
    int handleInterrupts();
};

// ---------------------------------------------------------------- GB (bus + system)
struct GB {
    CPU cpu;
    PPU ppu;
    APU apu;
    Cartridge cart;
    ChromaticFM fm;
    bool cgb = false;              // running in CGB mode
    bool doubleSpeed = false;
    uint8_t key1 = 0;

    uint8_t vram[2][0x2000];
    uint8_t wram[8][0x1000];
    uint8_t oam[0xA0];
    uint8_t hram[0x7F];
    int vramBank = 0, wramBank = 1;

    uint8_t ifReg = 0xE1, ieReg = 0;
    uint8_t joyp = 0xCF;           // P1 select bits
    uint8_t buttons = 0;           // pressed buttons from JS

    // timer
    uint16_t divCounter = 0;
    uint8_t tima = 0, tma = 0, tac = 0xF8;
    int timaReloadDelay = 0;

    // serial (stub with timed completion)
    uint8_t sb = 0, sc = 0x7E;
    int serialCounter = 0;

    // OAM DMA
    uint8_t dmaReg = 0;

    // CGB HDMA
    uint8_t hdmaSrcH = 0, hdmaSrcL = 0, hdmaDstH = 0, hdmaDstL = 0;
    bool hdmaActive = false;       // hblank DMA in progress
    int hdmaLen = 0;               // remaining blocks - 1 semantics kept in reg read

    bool loaded = false;

#if defined(GB_PROFILE) && defined(ESP_PLATFORM)
    uint64_t profileCpuUs = 0;
    uint64_t profilePpuUs = 0;
    uint64_t profileApuUs = 0;
#endif

    bool loadRom(const uint8_t* data, size_t size);
    bool loadRom(ByteStorage&& data);
    void reset();
    void runFrame();

    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);
    uint8_t readIO(uint8_t reg);
    void writeIO(uint8_t reg, uint8_t v);

    void requestInterrupt(uint8_t bit) { ifReg |= bit; }
    void tick(int cpuCycles);      // advance peripherals by CPU T-cycles
    void tickTimer(int cpuCycles);
    void doOamDma(uint8_t page);
    void doHdmaBlock();            // one 16-byte block at hblank
    uint8_t joypRead();
};

} // namespace gb
