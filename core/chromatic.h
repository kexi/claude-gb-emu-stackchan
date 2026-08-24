// ModRetro Chromatic FM expansion: YM2151 + MSM6258 ADPCM at $FF28-$FF2F.
// Register map follows Chromatic FM status-map v4:
//   FF28/FF29  unrestricted YM2151 address/data ports
//   FF2A       ADPCM FIFO write; status read (busy/ready/playing)
//   FF2B       control: bit3 ADPCM play edge, bits2:0 source enables,
//              bits7:4 ADPCM volume
//   FF2C/FF2D  YM left/right volume ($80 = reference level)
//   FF2E  expansion ID $51,  FF2F  status-map version $04
#pragma once
#include <atomic>
#include <cstdint>

namespace gb {

enum class ChromaticEventType : uint8_t { YmWrite, FifoWrite, Control, VolumeLeft, VolumeRight };

struct ChromaticEvent {
    uint32_t sample;
    ChromaticEventType type;
    uint8_t address;
    uint8_t value;
    uint8_t reserved = 0;
};
static_assert(sizeof(ChromaticEvent) == 8, "FM events must stay compact for internal SRAM");

struct ChromaticFM {
    bool enabled = false;          // FM button on the frontend
    bool deferred = false;         // CoreS3 records work for the Core 0 audio worker

    uint8_t ymAddrLatch = 0;
    uint8_t audioControl = 0x03;   // bit0 YM, bit1 GB APU, bit2 ADPCM
    uint8_t volumeLeft = 0x80, volumeRight = 0x80;
    uint8_t adpcmVolume = 8;

    // MSM6258 (OKI 4-bit ADPCM), 15625 Hz mono, 256-byte FIFO
    uint8_t fifo[256];
    int fifoRd = 0, fifoWr = 0, fifoCount = 0;
    bool adpcmActive = false, nibbleHi = false;
    int adpcmSignal = 0, adpcmStep = 0;
    uint64_t adpcmAcc = 0;
    float adpcmOut = 0;

    // YM2151 via ymfm (3.579545 MHz -> 55930.4 Hz sample rate)
    void* opm = nullptr;
    uint64_t ymAcc = 0;
    float ymL = 0, ymR = 0;

    static constexpr uint16_t EVENT_CAPACITY = 512;
    static constexpr uint16_t EVENT_MASK = EVENT_CAPACITY - 1;
    ChromaticEvent events[EVENT_CAPACITY];
    std::atomic<uint16_t> eventRead{0};
    std::atomic<uint16_t> eventWrite{0};
    uint32_t sampleCursor = 0;
    uint32_t eventBackpressureCount = 0;
    uint32_t ymBusyUntilSample = 0;

    ~ChromaticFM();
    void reset();
    uint8_t read(uint8_t reg);          // reg = io - 0x28 (0..7)
    void write(uint8_t reg, uint8_t v);
    // advance generators to produce the next output sample (called per
    // host-rate audio sample); results land in ymL/ymR/adpcmOut
    void generateSample(double sampleRate);
    bool peekDeferredEvent(ChromaticEvent& event) const;
    bool popDeferredEvent(ChromaticEvent& event);

private:
    void adpcmDecodeNibble();
    void adpcmStart();
    void adpcmStop();
    void pushDeferredEvent(ChromaticEventType type, uint8_t address, uint8_t value);
};

} // namespace gb
