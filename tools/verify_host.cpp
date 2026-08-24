// Deterministic host harness for comparing the reference and GB_EMBEDDED cores.
// What it guarantees: event batching does not change CPU-visible state, memory,
// rendered pixels, or generated audio for the same ROM and frame-boundary input.
#include "../core/gb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

uint32_t crc32(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            const uint32_t low = crc & 1u;
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - low));
        }
    }
    return ~crc;
}

std::vector<uint8_t> makeTestRom() {
    std::vector<uint8_t> rom(0x8000, 0);
    const uint8_t program[] = {
        0xF3,   // DI
        0x31, 0xFE, 0xFF,   // LD SP,$FFFE
        0x21, 0x00, 0x80,   // LD HL,$8000
        0xAF,   // XOR A
        0x22, 0x3C, 0x20, 0xFC,   // fill 256 tile bytes
        0x21, 0x00, 0x98,   // LD HL,$9800
        0x06, 0x04,   // LD B,4
        0xAF,   // XOR A
        0x22, 0x3C, 0x20, 0xFC,   // fill 4 * 256 map bytes
        0x05, 0x20, 0xF9,   // DEC B; JR NZ to inner loop
        0x3E, 0xE4, 0xE0, 0x47,   // DMG palette 0,1,2,3
        0x3E, 0x91, 0xE0, 0x40,   // LCD and background on
        0xF0, 0x43, 0x3C, 0xE0, 0x43,   // increment SCX forever
        0x18, 0xF9,
    };
    memcpy(rom.data() + 0x100, program, sizeof(program));
    memcpy(rom.data() + 0x134, "CODEX GB TEST", 13);
    rom[0x143] = 0x00;   // DMG mode
    rom[0x147] = 0x00;   // no MBC
    rom[0x148] = 0x00;   // 32 KiB
    rom[0x149] = 0x00;   // no cartridge RAM
    return rom;
}

std::vector<uint8_t> loadRomFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

size_t countDistinctPixels(const gb::Pixel* pixels, size_t count) {
    std::vector<gb::Pixel> distinct;
    for (size_t index = 0; index < count; index++) {
        const bool pixelAlreadySeen = std::find(distinct.begin(), distinct.end(), pixels[index]) != distinct.end();
        if (!pixelAlreadySeen) distinct.push_back(pixels[index]);
    }
    return distinct.size();
}

}   // namespace

int runVerification(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 180;
    const bool isValidFrameCount = frames > 0 && frames <= 10000;
    if (!isValidFrameCount) {
        std::fprintf(stderr, "frames must be in 1..10000\n");
        return 2;
    }

    const bool usesExternalRom = argc > 2;
    const std::vector<uint8_t> rom = usesExternalRom ? loadRomFile(argv[2]) : makeTestRom();
    if (rom.empty()) {
        std::fprintf(stderr, "ROM file could not be read: %s\n", argv[2]);
        return 1;
    }
    auto system = std::make_unique<gb::GB>();
    system->apu.setSampleRate(44100.0);
    if (!system->loadRom(rom.data(), rom.size())) {
        std::fprintf(stderr, "%s ROM was rejected\n", usesExternalRom ? "external" : "synthetic");
        return 1;
    }

    uint32_t audioCrc = 0;
    for (int frame = 0; frame < frames; frame++) {
        const bool pressRight = frame >= 30 && frame < 60;
        const bool pressA = frame >= 90 && frame < 100;
        system->buttons = (pressRight ? gb::BTN_RIGHT : 0) | (pressA ? gb::BTN_A : 0);
        system->runFrame();
        audioCrc ^= crc32(system->apu.sampleBuf, (size_t)system->apu.sampleCount * 2 * sizeof(float));
        system->apu.sampleCount = 0;
    }

    const size_t distinctPixels = countDistinctPixels(system->ppu.framebuffer, sizeof(system->ppu.framebuffer) /
                                                                                   sizeof(system->ppu.framebuffer[0]));
    const bool externalRomRenderedImage = !usesExternalRom || distinctPixels > 1;

    std::printf("frames=%u pc=%04x sp=%04x regs=%02x%02x%02x%02x%02x%02x ", system->ppu.frameCount, system->cpu.pc,
                system->cpu.sp, system->cpu.a, system->cpu.f, system->cpu.b, system->cpu.c, system->cpu.d,
                system->cpu.e);
    std::printf("wram=%08x vram=%08x oam=%08x fb=%08x colors=%zu audio=%08x\n",
                crc32(system->wram, sizeof(system->wram)), crc32(system->vram, sizeof(system->vram)),
                crc32(system->oam, sizeof(system->oam)),
                crc32(system->ppu.framebuffer, sizeof(system->ppu.framebuffer)), distinctPixels, audioCrc);
    if (!externalRomRenderedImage) std::fprintf(stderr, "external ROM produced a uniform framebuffer\n");
    return externalRomRenderedImage ? 0 : 1;
}

int main(int argc, char** argv) {
    try {
        return runVerification(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "verification failed: %s\n", error.what());
        return 1;
    }
}
