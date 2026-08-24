// Deterministic host harness for comparing the reference and GB_EMBEDDED cores.
// What it guarantees: event batching does not change CPU-visible state, memory,
// rendered pixels, or generated audio for the same ROM and frame-boundary input.
#include "../core/gb.h"
#include "../m5stack/src/kantan_autoplay.h"

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

bool verifyChromaticV4() {
    gb::ChromaticFM fm;
    fm.enabled = true;
    fm.reset();
    // What it guarantees: the synchronous renderer does not reserve the
    // producer-only event queue on the Core 0 audio worker stack.
    if (fm.events != nullptr) return false;
    const bool identityMatches = fm.read(6) == 0x51 && fm.read(7) == 0x04;
    if (!identityMatches) return false;

    for (int byte = 0; byte < 256; byte++) fm.write(2, static_cast<uint8_t>(byte));
    const bool fullFifoBlocksWrites = (fm.read(2) & 0x40) == 0 && fm.fifoCount == 256;
    fm.write(2, 0xAA);
    if (!fullFifoBlocksWrites || fm.fifoCount != 256) return false;

    fm.write(3, 0x8D);
    const bool risingEdgeStarts = (fm.read(2) & 0x20) != 0 && fm.fifoCount == 256;
    fm.write(3, 0x8D);
    const bool levelWriteDoesNotRestart = fm.fifoCount == 256;
    fm.write(3, 0x85);
    const bool fallingEdgeStopsAndClears = (fm.read(2) & 0x20) == 0 && fm.fifoCount == 0;
    if (!risingEdgeStarts || !levelWriteDoesNotRestart || !fallingEdgeStopsAndClears) return false;

    fm.write(3, 0x8D);
    for (int sample = 0; sample < 4; sample++) fm.generateSample(44100.0);
    if ((fm.read(2) & 0x20) != 0) return false;

    gb::ChromaticFM deferredFm;
    deferredFm.enabled = true;
    deferredFm.deferred = true;
    deferredFm.reset();
    // What it guarantees: the single-producer/single-consumer queue holds a
    // full register burst without overwriting or reordering any YM write.
    for (uint16_t index = 0; index < gb::ChromaticFM::EVENT_CAPACITY - 1; index++) {
        deferredFm.write(0, static_cast<uint8_t>(index));
        deferredFm.write(1, static_cast<uint8_t>(index ^ 0xA5));
    }
    if (deferredFm.events == nullptr || deferredFm.eventHighWater != gb::ChromaticFM::EVENT_CAPACITY - 1) {
        return false;
    }
    for (uint16_t index = 0; index < gb::ChromaticFM::EVENT_CAPACITY - 1; index++) {
        gb::ChromaticEvent event;
        if (!deferredFm.popDeferredEvent(event)) return false;
        const bool eventMatches = event.type == gb::ChromaticEventType::YmWrite &&
                                  event.address == static_cast<uint8_t>(index) &&
                                  event.value == static_cast<uint8_t>(index ^ 0xA5);
        if (!eventMatches) return false;
    }
    gb::ChromaticEvent unexpectedEvent;
    return !deferredFm.popDeferredEvent(unexpectedEvent);
}

bool verifyKantanDemoInput() {
    uint32_t risingEdges = 0;
    uint32_t pressedFrames = 0;
    uint8_t previous = 0;
    for (uint32_t frame = 0; frame < 1000; frame++) {
        const uint8_t input = stackchan::kantanDemoInput(frame);
        const bool pressesDemoGesture = input == (gb::BTN_SELECT | gb::BTN_A);
        const bool pressesDirection = (input & 0x0F) != 0;
        if (pressesDirection) return false;
        if (pressesDemoGesture) pressedFrames++;
        if ((input & gb::BTN_A) && !(previous & gb::BTN_A)) risingEdges++;
        previous = input;
    }
    return risingEdges == 1 && pressedFrames == stackchan::KANTAN_DEMO_PRESS_FRAME_COUNT && previous == 0;
}

struct RhythmTraceState {
    uint8_t previousControl = 0x03;
    uint32_t fifoHash = 2166136261u;
    uint32_t fifoBytes = 0;
};

void traceChromaticEvents(gb::ChromaticFM& fm, uint32_t frame, RhythmTraceState& state) {
    gb::ChromaticEvent event;
    while (fm.popDeferredEvent(event)) {
        const bool startsAdpcm =
            event.type == gb::ChromaticEventType::Control && !(state.previousControl & 0x08) && (event.value & 0x08);
        const bool changesControl = event.type == gb::ChromaticEventType::Control;
        const bool writesFifo = event.type == gb::ChromaticEventType::FifoWrite;
        const bool keysOnYm =
            event.type == gb::ChromaticEventType::YmWrite && event.address == 0x08 && (event.value & 0x78);
        if (writesFifo) {
            state.fifoHash = (state.fifoHash ^ event.value) * 16777619u;
            state.fifoBytes++;
        }
        if (startsAdpcm) {
            std::printf("rhythm,adpcm_start,frame=%u,sample=%u,control=%02x,bytes=%u,hash=%08x\n", frame, event.sample,
                        event.value, state.fifoBytes, state.fifoHash);
            state.fifoHash = 2166136261u;
            state.fifoBytes = 0;
        }
        if (keysOnYm) {
            std::printf("rhythm,ym_key_on,frame=%u,sample=%u,channel=%u,slots=%02x\n", frame, event.sample,
                        event.value & 7, event.value & 0x78);
        }
        if (changesControl) state.previousControl = event.value;
    }
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
    const bool enablesFm = argc > 3 && std::strcmp(argv[3], "--fm") == 0;
    const bool tracesRhythm = argc > 4 && std::strcmp(argv[4], "--trace-rhythm") == 0;
    if (!verifyKantanDemoInput()) {
        std::fprintf(stderr, "KANTAN demo input contract failed\n");
        return 1;
    }
    if (enablesFm && !verifyChromaticV4()) {
        std::fprintf(stderr, "Chromatic v4 register contract failed\n");
        return 1;
    }
    const std::vector<uint8_t> rom = usesExternalRom ? loadRomFile(argv[2]) : makeTestRom();
    if (rom.empty()) {
        std::fprintf(stderr, "ROM file could not be read: %s\n", argv[2]);
        return 1;
    }
    auto system = std::make_unique<gb::GB>();
    system->apu.setSampleRate(44100.0);
    system->fm.enabled = enablesFm;
    system->fm.deferred = tracesRhythm;
    if (!system->loadRom(rom.data(), rom.size())) {
        std::fprintf(stderr, "%s ROM was rejected\n", usesExternalRom ? "external" : "synthetic");
        return 1;
    }

    uint32_t audioCrc = 0;
    RhythmTraceState rhythmTrace;
    uint8_t previousInput = 0;
    for (int frame = 0; frame < frames; frame++) {
        if (tracesRhythm) {
            system->buttons = stackchan::kantanDemoInput(static_cast<uint32_t>(frame));
        } else {
            const bool pressRight = frame >= 30 && frame < 60;
            const bool pressA = frame >= 90 && frame < 100;
            system->buttons = (pressRight ? gb::BTN_RIGHT : 0) | (pressA ? gb::BTN_A : 0);
        }
        const bool startsChordInput = tracesRhythm && (system->buttons & gb::BTN_A) && !(previousInput & gb::BTN_A);
        if (startsChordInput) {
            std::printf("rhythm,chord_input,frame=%u,sample=%u,direction=%02x\n", frame, system->fm.sampleCursor,
                        system->buttons & 0x0F);
        }
        previousInput = system->buttons;
        system->runFrame();
        if (tracesRhythm) traceChromaticEvents(system->fm, static_cast<uint32_t>(frame), rhythmTrace);
        audioCrc ^= crc32(system->apu.sampleBuf, (size_t)system->apu.sampleCount * 2 * sizeof(float));
        system->apu.sampleCount = 0;
    }

    const size_t distinctPixels = countDistinctPixels(system->ppu.framebuffer, std::size(system->ppu.framebuffer));
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
