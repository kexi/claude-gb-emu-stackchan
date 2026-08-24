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
    static constexpr uint8_t ADPCM_DECLICK_SAMPLES = 24;

    bool enabled = false;   // FM button on the frontend
    bool deferred = false;   // CoreS3 records work for the Core 0 audio worker

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
    float adpcmRawOut = 0;
    float adpcmOut = 0;
    float adpcmPresentationResidual = 0;
    uint8_t adpcmPresentationIndex = ADPCM_DECLICK_SAMPLES;

    // YM2151 via ymfm (3.579545 MHz -> 55930.4 Hz sample rate)
    void* opm = nullptr;
    uint64_t ymAcc = 0;
    float ymL = 0, ymR = 0;

    static constexpr uint16_t EVENT_CAPACITY = 2048;
    static constexpr uint16_t EVENT_MASK = EVENT_CAPACITY - 1;
    static_assert((EVENT_CAPACITY & EVENT_MASK) == 0, "FM event capacity must be a power of two");
    ChromaticEvent* events = nullptr;
    std::atomic<uint16_t> eventRead{0};
    std::atomic<uint16_t> eventWrite{0};
    uint32_t sampleCursor = 0;
    uint32_t eventBackpressureCount = 0;
    uint16_t eventHighWater = 0;
    uint32_t ymBusyUntilSample = 0;

    ChromaticFM() = default;
    ChromaticFM(const ChromaticFM&) = delete;
    ChromaticFM& operator=(const ChromaticFM&) = delete;
    ~ChromaticFM();
    void reset();
    uint8_t read(uint8_t reg);   // reg = io - 0x28 (0..7)
    void write(uint8_t reg, uint8_t v);
    // advance generators to produce the next output sample (called per
    // host-rate audio sample); results land in ymL/ymR/adpcmOut
    void generateSample(double sampleRate);
    // Apply an event immediately before generating event.sample. This is the
    // shared consumer path for CoreS3 and deterministic host replay.
    void applyDeferredEvent(const ChromaticEvent& event);
    bool peekDeferredEvent(ChromaticEvent& event) const;
    bool popDeferredEvent(ChromaticEvent& event);

  private:
    void adpcmDecodeNibble();
    void adpcmStart();
    void adpcmStop();
    void beginAdpcmPresentationTransition(float nextRawOutput);
    void updateAdpcmPresentation();
    void ensureDeferredEvents();
    void pushDeferredEvent(ChromaticEventType type, uint8_t address, uint8_t value);
};

// Holds raw bridge events until enough future input is known to distinguish
// STOP -> preload -> PLAY from PLAY -> streaming feed. CPU-visible state stays
// in ChromaticFM; this class only changes when the audio consumer hears the
// accepted FIFO writes and PLAY edge.
class ChromaticAudioAligner {
  public:
    static constexpr uint32_t LOOKAHEAD_SAMPLES = 2048;

    ChromaticAudioAligner();
    ChromaticAudioAligner(const ChromaticAudioAligner&) = delete;
    ChromaticAudioAligner& operator=(const ChromaticAudioAligner&) = delete;
    ~ChromaticAudioAligner();

    void reset();
    void setRhythmStabilization(bool enabled) { rhythmStabilizationEnabled = enabled; }
    bool ingest(const ChromaticEvent& event);
    bool advanceRawHorizon(uint32_t sample);
    bool popDueEvent(uint32_t acousticSample, ChromaticEvent& event);

    uint32_t compensatedStarts() const { return compensatedStartCount; }
    uint32_t failures() const { return failureCount; }
    uint32_t maximumPreloadSpan() const { return maximumPreloadSpanSamples; }
    uint32_t stabilizedStarts() const { return stabilizedStartCount; }
    uint32_t maximumRhythmCorrection() const { return maximumRhythmCorrectionSamples; }

  private:
    static constexpr uint16_t NORMAL_CAPACITY = ChromaticFM::EVENT_CAPACITY;
    static constexpr uint16_t NORMAL_MASK = NORMAL_CAPACITY - 1;
    static constexpr uint16_t SHIFTED_CAPACITY = 1024;
    static constexpr uint16_t SHIFTED_MASK = SHIFTED_CAPACITY - 1;
    static constexpr uint16_t PRELOAD_CAPACITY = 256;
    static constexpr uint8_t NOISE_CLUSTER_CAPACITY = 8;
    static constexpr uint32_t NOISE_CLUSTER_SAMPLES = 64;
    static constexpr uint32_t ADPCM_HIT_SPREAD_SAMPLES = 1024;
    static_assert((SHIFTED_CAPACITY & SHIFTED_MASK) == 0, "shifted event capacity must be a power of two");

    ChromaticEvent* normal = nullptr;
    ChromaticEvent* shifted = nullptr;
    ChromaticEvent* preload = nullptr;
    uint16_t normalRead = 0;
    uint16_t normalWrite = 0;
    uint16_t shiftedRead = 0;
    uint16_t shiftedWrite = 0;
    uint16_t preloadCount = 0;
    ChromaticEvent pendingStop{};
    bool hasPendingStop = false;
    bool rawPlaying = false;
    bool preloadArmed = false;
    uint32_t activeShiftSamples = 0;
    uint32_t companionShiftUntilRawSample = 0;
    uint32_t compensatedStartCount = 0;
    uint32_t failureCount = 0;
    uint32_t maximumPreloadSpanSamples = 0;
    bool rhythmStabilizationEnabled = false;
    bool percussionHasPeriod = false;
    bool percussionHasOnset = false;
    uint32_t percussionLastNaturalOnset = 0;
    uint32_t percussionLastEffectiveOnset = 0;
    uint32_t percussionPeriodQ8 = 0;
    uint64_t percussionCalibrationTotal = 0;
    uint8_t percussionCalibrationIntervals = 0;
    uint32_t lastShiftedEffectiveSample = 0;
    ChromaticEvent noiseCluster[NOISE_CLUSTER_CAPACITY];
    uint8_t noiseClusterCount = 0;
    uint32_t stabilizedStartCount = 0;
    uint32_t maximumRhythmCorrectionSamples = 0;

    bool hasShiftedCapacity(uint16_t count) const;
    bool enqueueNormal(const ChromaticEvent& event);
    bool enqueueShifted(const ChromaticEvent& event, uint32_t effectiveSample);
    bool scheduleRaw(const ChromaticEvent& event);
    bool scheduleShifted(const ChromaticEvent& event);
    bool flushPreload();
    bool scheduleCompensatedStart(const ChromaticEvent& playEvent);
    uint32_t stabilizePercussionOnset(uint32_t naturalSample, uint32_t latestSample);
    bool holdNoiseClusterEvent(const ChromaticEvent& event);
    bool flushNoiseCluster(uint32_t effectiveSample);
    bool releaseStaleNoiseCluster(uint32_t rawSample);
    void shiftRecentChordKeyOns(uint32_t naturalSample, int32_t correction);
};

}   // namespace gb
