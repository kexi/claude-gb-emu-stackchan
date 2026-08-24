// Cartridge — no-MBC, MBC1, MBC2, MBC3(+RTC), MBC5
#include "gb.h"

namespace gb {

bool Cartridge::load(const uint8_t* data, size_t size) {
    if (size < 0x8000) return false;
    rom.assign(data, data + size);
    return configureLoadedRom();
}

bool Cartridge::load(ByteStorage&& data) {
    const bool isTooSmall = data.size() < 0x8000;
    if (isTooSmall) return false;
    rom = std::move(data);
    return configureLoadedRom();
}

bool Cartridge::configureLoadedRom() {
    type = rom[0x147];
    battery = false; hasRtc = false;
    switch (type) {
    case 0x00: mbc = 0; break;
    case 0x01: mbc = 1; break;
    case 0x02: mbc = 1; break;
    case 0x03: mbc = 1; battery = true; break;
    case 0x05: mbc = 2; break;
    case 0x06: mbc = 2; battery = true; break;
    case 0x08: case 0x09: mbc = 0; battery = (type == 0x09); break;
    case 0x0F: mbc = 3; battery = true; hasRtc = true; break;
    case 0x10: mbc = 3; battery = true; hasRtc = true; break;
    case 0x11: mbc = 3; break;
    case 0x12: mbc = 3; break;
    case 0x13: mbc = 3; battery = true; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: mbc = 5; break;
    case 0x1B: case 0x1E: mbc = 5; battery = true; break;
    default: mbc = 1; break; // unknown: assume MBC1-ish
    }
    int romSizeCode = rom[0x148];
    romBanks = 2 << (romSizeCode <= 8 ? romSizeCode : 0);

    static const int RAM_SIZES[] = {0, 2048, 8192, 32768, 131072, 65536};
    int ramCode = rom[0x149];
    int ramSize = (ramCode < 6) ? RAM_SIZES[ramCode] : 0;
    if (mbc == 2) ramSize = 512;   // built-in 512x4bit
    ram.assign(ramSize, 0xFF);
    ramBanks = ramSize > 0 ? (ramSize + 8191) / 8192 : 0;
    ramGeneration = 0;

    ramEnable = false; romBank = 1; ramBank = 0; mbc1Mode = 0; mbc1Hi = 0;
    rtcSelect = -1; rtcLatchPrev = 0xFF; rtcAccum = 0;
    memset(rtcReg, 0, sizeof(rtcReg));
    memset(rtcLive, 0, sizeof(rtcLive));
    return true;
}

void Cartridge::tickRtc(double seconds) {
    if (!hasRtc || (rtcLive[4] & 0x40)) return;  // halted
    rtcAccum += seconds;
    while (rtcAccum >= 1.0) {
        rtcAccum -= 1.0;
        if (++rtcLive[0] >= 60) { rtcLive[0] = 0;
            if (++rtcLive[1] >= 60) { rtcLive[1] = 0;
                if (++rtcLive[2] >= 24) { rtcLive[2] = 0;
                    int days = (rtcLive[3] | ((rtcLive[4] & 1) << 8)) + 1;
                    if (days > 511) { days = 0; rtcLive[4] |= 0x80; } // carry
                    rtcLive[3] = days & 0xFF;
                    rtcLive[4] = (rtcLive[4] & 0xFE) | ((days >> 8) & 1);
                }
            }
        }
    }
}

uint8_t Cartridge::read(uint16_t addr) const {
    if (addr < 0x4000) {
        // MBC1 mode 1: bank 0 area can map upper banks
        if (mbc == 1 && mbc1Mode == 1) {
            int bank = (mbc1Hi << 5) % romBanks;
            return rom[bank * 0x4000 + addr];
        }
        return rom[addr];
    }
    if (addr < 0x8000) {
        int bank = romBank;
        if (mbc == 1) bank = ((mbc1Hi << 5) | (romBank & 0x1F)) % romBanks;
        else bank %= romBanks;
        size_t off = (size_t)bank * 0x4000 + (addr - 0x4000);
        return off < rom.size() ? rom[off] : 0xFF;
    }
    // 0xA000-0xBFFF: external RAM / RTC
    if (!ramEnable) return 0xFF;
    if (mbc == 3 && rtcSelect >= 0) return rtcReg[rtcSelect];
    if (ram.empty()) return 0xFF;
    if (mbc == 2) return ram[addr & 0x1FF] | 0xF0;
    int bank = (mbc == 1 && mbc1Mode == 0) ? 0 : ramBank;
    size_t off = (size_t)(bank % (ramBanks ? ramBanks : 1)) * 0x2000 + (addr - 0xA000);
    return off < ram.size() ? ram[off] : 0xFF;
}

void Cartridge::write(uint16_t addr, uint8_t v) {
    if (addr < 0x8000) {
        switch (mbc) {
        case 0: break;
        case 1:
            if (addr < 0x2000) ramEnable = (v & 0xF) == 0xA;
            else if (addr < 0x4000) { romBank = v & 0x1F; if (romBank == 0) romBank = 1; }
            else if (addr < 0x6000) { mbc1Hi = v & 3; ramBank = v & 3; }
            else mbc1Mode = v & 1;
            break;
        case 2:
            if (addr < 0x4000) {
                if (addr & 0x100) { romBank = v & 0xF; if (romBank == 0) romBank = 1; }
                else ramEnable = (v & 0xF) == 0xA;
            }
            break;
        case 3:
            if (addr < 0x2000) ramEnable = (v & 0xF) == 0xA;
            else if (addr < 0x4000) { romBank = v & 0x7F; if (romBank == 0) romBank = 1; }
            else if (addr < 0x6000) {
                if (v <= 0x03) { ramBank = v; rtcSelect = -1; }
                else if (v >= 0x08 && v <= 0x0C) rtcSelect = v - 0x08;
            } else { // latch clock
                if (rtcLatchPrev == 0 && v == 1) memcpy(rtcReg, rtcLive, sizeof(rtcReg));
                rtcLatchPrev = v;
            }
            break;
        case 5:
            if (addr < 0x2000) ramEnable = (v & 0xF) == 0xA;
            else if (addr < 0x3000) romBank = (romBank & 0x100) | v;
            else if (addr < 0x4000) romBank = (romBank & 0xFF) | ((v & 1) << 8);
            else if (addr < 0x6000) ramBank = v & 0x0F;
            break;
        }
        return;
    }
    // external RAM / RTC
    if (!ramEnable) return;
    if (mbc == 3 && rtcSelect >= 0) {
        rtcLive[rtcSelect] = v;
        rtcReg[rtcSelect] = v;
        return;
    }
    if (ram.empty()) return;
    if (mbc == 2) {
        ram[addr & 0x1FF] = v & 0x0F;
        ramGeneration++;
        return;
    }
    int bank = (mbc == 1 && mbc1Mode == 0) ? 0 : ramBank;
    size_t off = (size_t)(bank % (ramBanks ? ramBanks : 1)) * 0x2000 + (addr - 0xA000);
    if (off < ram.size()) {
        ram[off] = v;
        ramGeneration++;
    }
}

} // namespace gb
