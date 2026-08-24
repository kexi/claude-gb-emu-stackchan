// GB system: bus, timer, DMA, and the WASM API
#include "gb.h"

#if defined(GB_PROFILE) && defined(ESP_PLATFORM)
#include <esp_timer.h>
#endif

namespace gb {

bool GB::loadRom(const uint8_t* data, size_t size) {
    if (!cart.load(data, size)) return false;
    uint8_t cgbFlag = cart.rom[0x143];
    cgb = (cgbFlag & 0x80) != 0;   // CGB-capable or CGB-only → run in CGB mode
    loaded = true;
    reset();
    return true;
}

bool GB::loadRom(ByteStorage&& data) {
    const bool cartridgeLoaded = cart.load(std::move(data));
    if (!cartridgeLoaded) return false;
    uint8_t cgbFlag = cart.rom[0x143];
    cgb = (cgbFlag & 0x80) != 0;
    loaded = true;
    reset();
    return true;
}

void GB::reset() {
    cpu.gb = this; ppu.gb = this; apu.gb = this;
    cpu.reset(cgb);
    ppu.reset(cgb);
    apu.reset();
    memset(vram, 0, sizeof(vram));
    memset(wram, 0, sizeof(wram));
    memset(oam, 0, sizeof(oam));
    memset(hram, 0, sizeof(hram));
    vramBank = 0; wramBank = 1;
    ifReg = 0xE1; ieReg = 0;
    joyp = 0xCF;
    divCounter = 0xABCC; tima = 0; tma = 0; tac = 0xF8;
    sb = 0; sc = 0x7E; serialCounter = 0;
    dmaReg = 0xFF;
    doubleSpeed = false; key1 = 0;
    hdmaActive = false; hdmaLen = 0;
    hdmaSrcH = hdmaSrcL = hdmaDstH = hdmaDstL = 0;
    cart.ramEnable = false;
    fm.reset();                    // keeps fm.enabled as set by the FM button
}

uint8_t GB::joypRead() {
    uint8_t v = 0xC0 | (joyp & 0x30) | 0x0F;
    if (!(joyp & 0x10)) v &= ~(buttons & 0x0F);          // directions
    if (!(joyp & 0x20)) v &= ~((buttons >> 4) & 0x0F);   // A/B/Select/Start
    return v;
}

uint8_t GB::read(uint16_t addr) {
    if (addr < 0x8000) return cart.read(addr);
    if (addr < 0xA000) return vram[vramBank][addr - 0x8000];
    if (addr < 0xC000) return cart.read(addr);
    if (addr < 0xD000) return wram[0][addr - 0xC000];
    if (addr < 0xE000) return wram[wramBank][addr - 0xD000];
    if (addr < 0xFE00) return read(addr - 0x2000);       // echo RAM
    if (addr < 0xFEA0) return oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;                      // unusable
    if (addr < 0xFF80) return readIO(addr & 0xFF);
    if (addr < 0xFFFF) return hram[addr - 0xFF80];
    return ieReg;
}

void GB::write(uint16_t addr, uint8_t v) {
    if (addr < 0x8000) { cart.write(addr, v); return; }
    if (addr < 0xA000) { vram[vramBank][addr - 0x8000] = v; return; }
    if (addr < 0xC000) { cart.write(addr, v); return; }
    if (addr < 0xD000) { wram[0][addr - 0xC000] = v; return; }
    if (addr < 0xE000) { wram[wramBank][addr - 0xD000] = v; return; }
    if (addr < 0xFE00) { write(addr - 0x2000, v); return; }
    if (addr < 0xFEA0) { oam[addr - 0xFE00] = v; return; }
    if (addr < 0xFF00) return;
    if (addr < 0xFF80) { writeIO(addr & 0xFF, v); return; }
    if (addr < 0xFFFF) { hram[addr - 0xFF80] = v; return; }
    ieReg = v;
}

uint8_t GB::readIO(uint8_t reg) {
    if (fm.enabled && reg >= 0x28 && reg <= 0x2F) return fm.read(reg - 0x28);
    switch (reg) {
    case 0x00: return joypRead();
    case 0x01: return sb;
    case 0x02: return sc | 0x7E;
    case 0x04: return divCounter >> 8;
    case 0x05: return tima;
    case 0x06: return tma;
    case 0x07: return tac | 0xF8;
    case 0x0F: return ifReg | 0xE0;
    case 0x40: return ppu.lcdc;
    case 0x41: return ppu.stat | 0x80;
    case 0x42: return ppu.scy;
    case 0x43: return ppu.scx;
    case 0x44: return ppu.ly;
    case 0x45: return ppu.lyc;
    case 0x46: return dmaReg;
    case 0x47: return ppu.bgp;
    case 0x48: return ppu.obp0;
    case 0x49: return ppu.obp1;
    case 0x4A: return ppu.wy;
    case 0x4B: return ppu.wx;
    case 0x4D: return cgb ? (key1 | 0x7E) : 0xFF;
    case 0x4F: return cgb ? (0xFE | vramBank) : 0xFF;
    case 0x51: return hdmaSrcH;
    case 0x52: return hdmaSrcL;
    case 0x53: return hdmaDstH;
    case 0x54: return hdmaDstL;
    case 0x55:
        if (!cgb) return 0xFF;
        if (!hdmaActive) return 0xFF;
        return (uint8_t)(hdmaLen - 1);
    case 0x68: return cgb ? ppu.bcps : 0xFF;
    case 0x69: return cgb ? ppu.bgPal[ppu.bcps & 0x3F] : 0xFF;
    case 0x6A: return cgb ? ppu.ocps : 0xFF;
    case 0x6B: return cgb ? ppu.objPal[ppu.ocps & 0x3F] : 0xFF;
    case 0x70: return cgb ? (0xF8 | wramBank) : 0xFF;
    default:
        if (reg >= 0x10 && reg <= 0x3F) return apu.read(reg - 0x10);
        return 0xFF;
    }
}

void GB::writeIO(uint8_t reg, uint8_t v) {
    if (fm.enabled && reg >= 0x28 && reg <= 0x2F) { fm.write(reg - 0x28, v); return; }
    switch (reg) {
    case 0x00: joyp = (joyp & 0xCF) | (v & 0x30); return;
    case 0x01: sb = v; return;
    case 0x02:
        sc = v;
        if ((v & 0x81) == 0x81) serialCounter = 512 * 8;  // internal clock transfer
        return;
    case 0x04: divCounter = 0; return;
    case 0x05: tima = v; return;
    case 0x06: tma = v; return;
    case 0x07: tac = v & 7; return;
    case 0x0F: ifReg = v & 0x1F; return;
    case 0x40: {
        bool wasOn = (ppu.lcdc & 0x80) != 0;
        ppu.lcdc = v;
        if (wasOn && !(v & 0x80)) { ppu.ly = 0; ppu.dot = 0; ppu.stat &= ~3; ppu.windowLine = 0; }
        return; }
    case 0x41:
        ppu.stat = (ppu.stat & 0x07) | (v & 0x78);
        ppu.checkStatIrq();
        return;
    case 0x42: ppu.scy = v; return;
    case 0x43: ppu.scx = v; return;
    case 0x44: return; // LY read-only
    case 0x45:
        ppu.lyc = v;
        ppu.stat = (ppu.stat & ~4) | (ppu.ly == ppu.lyc ? 4 : 0);
        ppu.checkStatIrq();
        return;
    case 0x46: dmaReg = v; doOamDma(v); return;
    case 0x47: ppu.bgp = v; return;
    case 0x48: ppu.obp0 = v; return;
    case 0x49: ppu.obp1 = v; return;
    case 0x4A: ppu.wy = v; return;
    case 0x4B: ppu.wx = v; return;
    case 0x4D: if (cgb) key1 = (key1 & 0x80) | (v & 1); return;
    case 0x4F: if (cgb) vramBank = v & 1; return;
    case 0x51: hdmaSrcH = v; return;
    case 0x52: hdmaSrcL = v & 0xF0; return;
    case 0x53: hdmaDstH = v & 0x1F; return;
    case 0x54: hdmaDstL = v & 0xF0; return;
    case 0x55: {
        if (!cgb) return;
        if (hdmaActive && !(v & 0x80)) { hdmaActive = false; return; } // cancel
        int blocks = (v & 0x7F) + 1;
        if (v & 0x80) {
            hdmaActive = true;
            hdmaLen = blocks;
        } else {
            // general purpose: copy all at once
            for (int i = 0; i < blocks; i++) { hdmaLen = 1; hdmaActive = true; doHdmaBlock(); }
            hdmaActive = false;
        }
        return; }
    case 0x68: if (cgb) ppu.bcps = v & 0xBF; return;
    case 0x69:
        if (cgb) {
            ppu.bgPal[ppu.bcps & 0x3F] = v;
            if (ppu.bcps & 0x80) ppu.bcps = 0x80 | ((ppu.bcps + 1) & 0x3F);
        }
        return;
    case 0x6A: if (cgb) ppu.ocps = v & 0xBF; return;
    case 0x6B:
        if (cgb) {
            ppu.objPal[ppu.ocps & 0x3F] = v;
            if (ppu.ocps & 0x80) ppu.ocps = 0x80 | ((ppu.ocps + 1) & 0x3F);
        }
        return;
    case 0x70:
        if (cgb) { wramBank = v & 7; if (wramBank == 0) wramBank = 1; }
        return;
    default:
        if (reg >= 0x10 && reg <= 0x3F) apu.write(reg - 0x10, v);
        return;
    }
}

void GB::doOamDma(uint8_t page) {
    uint16_t src = page << 8;
    for (int i = 0; i < 0xA0; i++) oam[i] = read(src + i);
}

void GB::doHdmaBlock() {
    if (!hdmaActive) return;
    uint16_t src = ((hdmaSrcH << 8) | hdmaSrcL) & 0xFFF0;
    uint16_t dst = (((hdmaDstH << 8) | hdmaDstL) & 0x1FF0);
    for (int i = 0; i < 16; i++) {
        uint8_t v = (src < 0x8000 || src >= 0xA000) ? read(src) : 0xFF;
        vram[vramBank][(dst + i) & 0x1FFF] = v;
        src++;
    }
    dst += 16;
    hdmaSrcH = src >> 8; hdmaSrcL = src & 0xF0;
    hdmaDstH = (dst >> 8) & 0x1F; hdmaDstL = dst & 0xF0;
    if (--hdmaLen <= 0) hdmaActive = false;
}

void GB::tickTimer(int cpuCycles) {
    static const int TAC_BIT[4] = {9, 3, 5, 7};
#ifdef GB_EMBEDDED
    const uint32_t start = divCounter;
    const uint32_t end = start + (uint32_t)cpuCycles;
    divCounter = (uint16_t)end;
    if (!(tac & 4)) return;

    const uint32_t period = 1u << (TAC_BIT[tac & 3] + 1);
    const uint32_t fallingEdges = end / period - start / period;
    for (uint32_t edge = 0; edge < fallingEdges; edge++) {
        if (++tima == 0) {
            tima = tma;
            requestInterrupt(INT_TIMER);
        }
    }
#else
    for (int i = 0; i < cpuCycles; i++) {
        uint16_t prev = divCounter;
        divCounter++;
        if (tac & 4) {
            int bit = TAC_BIT[tac & 3];
            if ((prev & (1 << bit)) && !(divCounter & (1 << bit))) {
                if (++tima == 0) {
                    tima = tma;
                    requestInterrupt(INT_TIMER);
                }
            }
        }
    }
#endif
}

void GB::tick(int cpuCycles) {
    // timer + serial run on the CPU clock (double in double-speed mode)
    tickTimer(cpuCycles);
    if (serialCounter > 0) {
        serialCounter -= cpuCycles;
        if (serialCounter <= 0) {
            sb = 0xFF;                 // no link partner
            sc &= 0x7F;
            requestInterrupt(INT_SERIAL);
        }
    }
    // PPU and APU run at the fixed 4.19 MHz dot clock
    int dots = doubleSpeed ? cpuCycles / 2 : cpuCycles;
#if defined(GB_PROFILE) && defined(ESP_PLATFORM)
    int64_t componentStartedUs = esp_timer_get_time();
#endif
    ppu.tick(dots);
#if defined(GB_PROFILE) && defined(ESP_PLATFORM)
    profilePpuUs += esp_timer_get_time() - componentStartedUs;
    componentStartedUs = esp_timer_get_time();
#endif
    apu.tick(dots);
#if defined(GB_PROFILE) && defined(ESP_PLATFORM)
    profileApuUs += esp_timer_get_time() - componentStartedUs;
#endif
    cart.tickRtc(dots / 4194304.0);
}

void GB::runFrame() {
    if (!loaded) return;
#if defined(GB_PROFILE) && defined(ESP_PLATFORM)
    profileCpuUs = profilePpuUs = profileApuUs = 0;
#endif
    ppu.frameDone = false;
    int budget = 70224 * 2 * 4;  // safety cap (in CPU cycles, double speed worst case)
    while (!ppu.frameDone && budget > 0) {
#if defined(GB_PROFILE) && defined(ESP_PLATFORM)
        const int64_t cpuStartedUs = esp_timer_get_time();
#endif
        int c = cpu.step();
#if defined(GB_PROFILE) && defined(ESP_PLATFORM)
        profileCpuUs += esp_timer_get_time() - cpuStartedUs;
#endif
        tick(c);
        budget -= c;
        if (!(ppu.lcdc & 0x80) && budget < 70224) break; // LCD off: just cap the frame
    }
}

} // namespace gb

// ================================================================ WASM API
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define API EMSCRIPTEN_KEEPALIVE
#else
#define API
#endif

static gb::GB* g_gb = nullptr;
static uint8_t g_romBuf[8 * 1024 * 1024];

extern "C" {

API void gb_init(double sampleRate) {
    if (!g_gb) g_gb = new gb::GB();
    g_gb->apu.setSampleRate(sampleRate);
}

API uint8_t* gb_rom_buffer() { return g_romBuf; }
API int gb_rom_buffer_size() { return (int)sizeof(g_romBuf); }

API int gb_load_rom(int size) {
    if (!g_gb || size <= 0 || (size_t)size > sizeof(g_romBuf)) return 0;
    return g_gb->loadRom(g_romBuf, (size_t)size) ? 1 : 0;
}

API void gb_reset() { if (g_gb && g_gb->loaded) g_gb->reset(); }
API void gb_frame() { if (g_gb) g_gb->runFrame(); }

API gb::Pixel* gb_framebuffer() { return g_gb ? g_gb->ppu.framebuffer : nullptr; }

API void gb_set_buttons(int buttons) {
    if (!g_gb) return;
    uint8_t nb = (uint8_t)buttons;
    if (nb & ~g_gb->buttons) g_gb->requestInterrupt(gb::INT_JOYPAD);
    g_gb->buttons = nb;
}

API float* gb_audio_buffer() { return g_gb ? g_gb->apu.sampleBuf : nullptr; }
API int gb_audio_sample_count() { return g_gb ? g_gb->apu.sampleCount : 0; }
API void gb_audio_clear() { if (g_gb) g_gb->apu.sampleCount = 0; }

API uint8_t* gb_sram() { return (g_gb && !g_gb->cart.ram.empty()) ? g_gb->cart.ram.data() : nullptr; }
API int gb_sram_size() { return g_gb ? (int)g_gb->cart.ram.size() : 0; }
API int gb_has_battery() { return (g_gb && g_gb->cart.battery) ? 1 : 0; }
API int gb_is_cgb() { return (g_gb && g_gb->cgb) ? 1 : 0; }
API uint32_t gb_frame_count() { return g_gb ? g_gb->ppu.frameCount : 0; }

API void gb_set_fm(int on) {
    if (!g_gb) return;
    bool en = on != 0;
    if (en != g_gb->fm.enabled) {
        g_gb->fm.enabled = en;
        g_gb->fm.reset();
    }
}
API int gb_get_fm() { return (g_gb && g_gb->fm.enabled) ? 1 : 0; }

// ---- debug ----

// side-effect-free memory read for the debugger (reads in this core have no
// side effects, so the normal bus read is safe to reuse)
API int gb_peek(int addr) {
    return (g_gb && g_gb->loaded) ? g_gb->read((uint16_t)addr) : 0xFF;
}

// packed CPU/system state:
// [0]PC.l [1]PC.h [2]SP.l [3]SP.h [4]A [5]F [6]B [7]C [8]D [9]E [10]H [11]L
// [12]IME [13]halted [14]doubleSpeed [15]cgb [16..19]frameCount
static uint8_t g_cpuRegs[20];
API uint8_t* gb_cpu_regs() {
    if (!g_gb) { memset(g_cpuRegs, 0, sizeof(g_cpuRegs)); return g_cpuRegs; }
    const auto& c = g_gb->cpu;
    g_cpuRegs[0] = c.pc & 0xFF; g_cpuRegs[1] = c.pc >> 8;
    g_cpuRegs[2] = c.sp & 0xFF; g_cpuRegs[3] = c.sp >> 8;
    g_cpuRegs[4] = c.a; g_cpuRegs[5] = c.f;
    g_cpuRegs[6] = c.b; g_cpuRegs[7] = c.c;
    g_cpuRegs[8] = c.d; g_cpuRegs[9] = c.e;
    g_cpuRegs[10] = c.h; g_cpuRegs[11] = c.l;
    g_cpuRegs[12] = c.ime ? 1 : 0;
    g_cpuRegs[13] = c.halted ? 1 : 0;
    g_cpuRegs[14] = g_gb->doubleSpeed ? 1 : 0;
    g_cpuRegs[15] = g_gb->cgb ? 1 : 0;
    uint32_t fc = g_gb->ppu.frameCount;
    g_cpuRegs[16] = fc & 0xFF; g_cpuRegs[17] = (fc >> 8) & 0xFF;
    g_cpuRegs[18] = (fc >> 16) & 0xFF; g_cpuRegs[19] = (fc >> 24) & 0xFF;
    return g_cpuRegs;
}

// APU registers FF10-FF3F as the CPU would read them (incl. wave RAM)
static uint8_t g_apuRegs[0x30];
API uint8_t* gb_apu_regs() {
    if (!g_gb) { memset(g_apuRegs, 0xFF, sizeof(g_apuRegs)); return g_apuRegs; }
    for (int i = 0; i < 0x30; i++) g_apuRegs[i] = g_gb->apu.read(i);
    return g_apuRegs;
}

// ROM title (up to 16 chars, NUL-terminated)
static char g_title[17];
API const char* gb_rom_title() {
    if (!g_gb || !g_gb->loaded) { g_title[0] = 0; return g_title; }
    for (int i = 0; i < 16; i++) {
        uint8_t ch = g_gb->cart.rom[0x134 + i];
        g_title[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : 0;
    }
    g_title[16] = 0;
    return g_title;
}

} // extern "C"
