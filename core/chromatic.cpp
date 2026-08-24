// Chromatic FM expansion: YM2151 (ymfm) + MSM6258 ADPCM decoder.
#include "chromatic.h"
#include "ymfm/ymfm_opm.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#ifndef CHROMATIC_SHIFT_RHYTHM_NOISE
#define CHROMATIC_SHIFT_RHYTHM_NOISE 1
#endif

namespace gb {

namespace {

// ymfm interface with a minimal timer implementation so games that poll the
// YM2151 timer flags (tempo timing) keep working
struct OpmWrap : public ymfm::ymfm_interface {
    ymfm::ym2151 chip{*this};
    int64_t timer[2] = {-1, -1};   // remaining clocks, <0 = disabled
    int64_t busyClocks = 0;

    void ymfm_set_timer(uint32_t tnum, int32_t duration) override {
        if (tnum < 2) timer[tnum] = duration;   // negative disables
    }
    void ymfm_set_busy_end(uint32_t clocks) override { busyClocks = clocks; }
    bool ymfm_is_busy() override { return busyClocks > 0; }
    void stepClocks(int clocks) {
        if (busyClocks > 0) busyClocks -= clocks;
        for (int t = 0; t < 2; t++) {
            if (timer[t] >= 0) {
                timer[t] -= clocks;
                if (timer[t] < 0) {
                    timer[t] = -1;
                    m_engine->engine_timer_expired(t);
                }
            }
        }
    }
};

// OKI/MSM6258 step size table (49 entries)
const int STEP_SIZE[49] = {
    16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,   45,   50,   55,   60,   66,  73,
    80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209,  230,  253,  279,  307,  337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
};

// per-volume gain, output = (signal * gain) >> 10
const int VOLUME_GAIN[16] = {
    267, 410, 492, 656, 799, 1045, 1270, 1536, 2048, 2479, 2991, 3994, 5325, 6431, 8561, 10363,
};

constexpr uint64_t YM_CLOCK_HZ = 3579545;
constexpr uint64_t YM_CLOCKS_PER_SAMPLE = 64;
constexpr uint64_t ADPCM_SAMPLE_HZ = 15625;
constexpr uint16_t ADPCM_DECLICK_WEIGHT_Q15[ChromaticFM::ADPCM_DECLICK_SAMPLES] = {
    32767, 32614, 32159, 31411, 30382, 29092, 27566, 25832, 23921, 21870, 19717, 17502,
    15265, 13050, 10897, 8846,  6935,  5201,  3675,  2385,  1356,  608,   153,   0,
};

bool isYmNoiseWrite(const ChromaticEvent& event) {
    const bool writesYm = event.type == ChromaticEventType::YmWrite;
    if (!writesYm) return false;
    const bool writesKeyState = event.address == 0x08;
    if (writesKeyState) return (event.value & 0x07) == 7;
    const bool writesNoiseControl = event.address == 0x0F;
    if (writesNoiseControl) return true;
    const bool writesChannelSeven = event.address >= 0x20 && (event.address & 0x07) == 7;
    return writesChannelSeven;
}

}   // namespace

ChromaticFM::~ChromaticFM() {
    delete static_cast<OpmWrap*>(opm);
    delete[] events;
}

void ChromaticFM::ensureDeferredEvents() {
    if (!events) events = new ChromaticEvent[EVENT_CAPACITY];
}

void ChromaticFM::reset() {
    if (!enabled || deferred) {
        if (deferred) ensureDeferredEvents();
        delete static_cast<OpmWrap*>(opm);
        opm = nullptr;
        ymAddrLatch = 0;
        audioControl = 0x03;
        volumeLeft = volumeRight = 0x80;
        adpcmVolume = 8;
        memset(fifo, 0, sizeof(fifo));
        fifoRd = fifoWr = fifoCount = 0;
        adpcmActive = nibbleHi = false;
        adpcmSignal = adpcmStep = 0;
        adpcmAcc = ymAcc = 0;
        adpcmRawOut = adpcmOut = ymL = ymR = 0;
        adpcmPresentationResidual = 0;
        adpcmPresentationIndex = ChromaticFM::ADPCM_DECLICK_SAMPLES;
        eventRead.store(0, std::memory_order_relaxed);
        eventWrite.store(0, std::memory_order_relaxed);
        sampleCursor = 0;
        eventBackpressureCount = 0;
        eventHighWater = 0;
        ymBusyUntilSample = 0;
        return;
    }
    if (!opm) opm = new OpmWrap();
    OpmWrap* w = static_cast<OpmWrap*>(opm);
    w->chip.reset();
    w->timer[0] = w->timer[1] = -1;
    w->busyClocks = 0;
    ymAddrLatch = 0;
    audioControl = 0x03;
    volumeLeft = volumeRight = 0x80;
    adpcmVolume = 8;
    memset(fifo, 0, sizeof(fifo));
    fifoRd = fifoWr = fifoCount = 0;
    adpcmActive = false;
    nibbleHi = false;
    adpcmSignal = 0;
    adpcmStep = 0;
    adpcmAcc = 0;
    adpcmRawOut = adpcmOut = 0;
    adpcmPresentationResidual = 0;
    adpcmPresentationIndex = ChromaticFM::ADPCM_DECLICK_SAMPLES;
    ymAcc = 0;
    ymL = ymR = 0;
    eventRead.store(0, std::memory_order_relaxed);
    eventWrite.store(0, std::memory_order_relaxed);
    sampleCursor = 0;
    eventBackpressureCount = 0;
    eventHighWater = 0;
    ymBusyUntilSample = 0;
}

void ChromaticFM::adpcmStart() {
    adpcmActive = true;
    adpcmSignal = 0;
    adpcmStep = 0;
    adpcmAcc = 0;
    beginAdpcmPresentationTransition(0);
}

void ChromaticFM::adpcmStop() {
    adpcmActive = false;
    beginAdpcmPresentationTransition(0);
    fifoRd = fifoWr = fifoCount = 0;
    nibbleHi = false;
}

void ChromaticFM::beginAdpcmPresentationTransition(float nextRawOutput) {
    adpcmPresentationResidual = adpcmOut - nextRawOutput;
    adpcmPresentationIndex = adpcmPresentationResidual == 0 ? ChromaticFM::ADPCM_DECLICK_SAMPLES : 0;
    adpcmRawOut = nextRawOutput;
}

void ChromaticFM::updateAdpcmPresentation() {
    if (adpcmPresentationIndex >= ChromaticFM::ADPCM_DECLICK_SAMPLES) {
        adpcmOut = adpcmRawOut;
        return;
    }
    const float weight = ADPCM_DECLICK_WEIGHT_Q15[adpcmPresentationIndex++] / 32767.0f;
    adpcmOut = adpcmRawOut + adpcmPresentationResidual * weight;
}

uint8_t ChromaticFM::read(uint8_t reg) {
    switch (reg) {
    case 0: return ymAddrLatch;
    case 2: {
        OpmWrap* w = static_cast<OpmWrap*>(opm);
        const bool shadowBusy = deferred && sampleCursor < ymBusyUntilSample;
        uint8_t busy = (shadowBusy || (w && (w->chip.read_status() & 0x80))) ? 0x80 : 0x00;
        uint8_t ready = (fifoCount < 256) ? 0x40 : 0x00;
        uint8_t playing = adpcmActive ? 0x20 : 0x00;
        return busy | ready | playing;
    }
    case 3: return audioControl;
    case 4: return volumeLeft;
    case 5: return volumeRight;
    case 6: return 0x51;   // expansion ID
    case 7: return 0x04;   // full-register/FIFO status-map version
    default: return 0xFF;   // FF29 is write-only
    }
}

void ChromaticFM::write(uint8_t reg, uint8_t v) {
    switch (reg) {
    case 0: ymAddrLatch = v; break;
    case 1:
        if (deferred) {
            pushDeferredEvent(ChromaticEventType::YmWrite, ymAddrLatch, v);
            ymBusyUntilSample = sampleCursor + 1;
            break;
        }
        if (OpmWrap* w = static_cast<OpmWrap*>(opm)) {
            w->chip.write_address(ymAddrLatch);
            w->chip.write_data(v);
        }
        break;
    case 2:
        if (fifoCount < 256) {
            if (deferred) pushDeferredEvent(ChromaticEventType::FifoWrite, 0, v);
            fifo[fifoWr] = v;
            fifoWr = (fifoWr + 1) & 0xFF;
            fifoCount++;
        }
        break;
    case 3: {
        if (deferred) pushDeferredEvent(ChromaticEventType::Control, 0, v);
        const bool wasPlaying = (audioControl & 0x08) != 0;
        const bool willPlay = (v & 0x08) != 0;
        audioControl = v;
        adpcmVolume = v >> 4;
        if (!wasPlaying && willPlay) adpcmStart();
        if (wasPlaying && !willPlay) adpcmStop();
        break;
    }
    case 4:
        if (deferred) pushDeferredEvent(ChromaticEventType::VolumeLeft, 0, v);
        volumeLeft = v;
        break;
    case 5:
        if (deferred) pushDeferredEvent(ChromaticEventType::VolumeRight, 0, v);
        volumeRight = v;
        break;
    default: break;
    }
}

void ChromaticFM::adpcmDecodeNibble() {
    if (fifoCount == 0) {
        adpcmActive = false;
        nibbleHi = false;
        beginAdpcmPresentationTransition(0);
        return;
    }
    uint8_t byte = fifo[fifoRd];
    int code = nibbleHi ? (byte >> 4) : (byte & 0x0F);
    if (nibbleHi) {
        fifoRd = (fifoRd + 1) & 0xFF;
        fifoCount--;
    }
    nibbleHi = !nibbleHi;

    int step = STEP_SIZE[adpcmStep];
    int delta = (step >> 3) + ((code & 1) ? (step >> 2) : 0) + ((code & 2) ? (step >> 1) : 0) + ((code & 4) ? step : 0);
    adpcmSignal += (code & 8) ? -delta : delta;
    if (adpcmSignal > 2047) adpcmSignal = 2047;
    if (adpcmSignal < -2048) adpcmSignal = -2048;
    int low = code & 7;
    adpcmStep += (low < 4) ? -1 : (low == 4) ? 2 : (low == 5) ? 4 : (low == 6) ? 6 : 8;
    if (adpcmStep < 0) adpcmStep = 0;
    if (adpcmStep > 48) adpcmStep = 48;

    int scaled = (adpcmSignal * VOLUME_GAIN[adpcmVolume & 15]) >> 10;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    adpcmRawOut = scaled / 32768.0f;
}

void ChromaticFM::generateSample(double sampleRate) {
    if (deferred) {
        const uint64_t outputRate = static_cast<uint64_t>(std::lround(sampleRate));
        if (adpcmActive && outputRate > 0) {
            adpcmAcc += ADPCM_SAMPLE_HZ;
            while (adpcmAcc >= outputRate) {
                adpcmAcc -= outputRate;
                adpcmDecodeNibble();
            }
        }
        ymL = ymR = adpcmOut = 0;
        sampleCursor++;
        return;
    }
    OpmWrap* w = static_cast<OpmWrap*>(opm);
    const uint64_t outputRate = static_cast<uint64_t>(std::lround(sampleRate));
    if (!w || outputRate == 0) {
        ymL = ymR = adpcmOut = 0;
        return;
    }
    // YM2151: catch up to the host sample position
    ymAcc += YM_CLOCK_HZ;
    const uint64_t ymPhaseLimit = outputRate * YM_CLOCKS_PER_SAMPLE;
    while (ymAcc >= ymPhaseLimit) {
        ymAcc -= ymPhaseLimit;
        ymfm::ym2151::output_data out;
        w->chip.generate(&out, 1);
        w->stepClocks(64);
        // match the RTL: pcm = (x * volume) >> 10 at 16-bit full scale
        int l = (out.data[0] * volumeLeft) >> 10;
        int r = (out.data[1] * volumeRight) >> 10;
        ymL = l / 32768.0f;
        ymR = r / 32768.0f;
    }
    // ADPCM at 15625 Hz
    if (adpcmActive) {
        adpcmAcc += ADPCM_SAMPLE_HZ;
        while (adpcmAcc >= outputRate) {
            adpcmAcc -= outputRate;
            adpcmDecodeNibble();
        }
    }
    updateAdpcmPresentation();
}

void ChromaticFM::applyDeferredEvent(const ChromaticEvent& event) {
    switch (event.type) {
    case ChromaticEventType::YmWrite:
        write(0, event.address);
        write(1, event.value);
        return;
    case ChromaticEventType::FifoWrite: write(2, event.value); return;
    case ChromaticEventType::Control: write(3, event.value); return;
    case ChromaticEventType::VolumeLeft: write(4, event.value); return;
    case ChromaticEventType::VolumeRight: write(5, event.value); return;
    }
}

void ChromaticFM::pushDeferredEvent(ChromaticEventType type, uint8_t address, uint8_t value) {
    ensureDeferredEvents();
    uint16_t write = eventWrite.load(std::memory_order_relaxed);
    uint16_t next = (write + 1) & EVENT_MASK;
    while (next == eventRead.load(std::memory_order_acquire)) {
        eventBackpressureCount++;
    }
    const uint16_t read = eventRead.load(std::memory_order_acquire);
    const uint16_t queuedAfterWrite = (next - read) & EVENT_MASK;
    if (queuedAfterWrite > eventHighWater) eventHighWater = queuedAfterWrite;
    events[write] = {sampleCursor, type, address, value, 0};
    eventWrite.store(next, std::memory_order_release);
}

bool ChromaticFM::peekDeferredEvent(ChromaticEvent& event) const {
    const uint16_t read = eventRead.load(std::memory_order_relaxed);
    if (read == eventWrite.load(std::memory_order_acquire)) return false;
    event = events[read];
    return true;
}

bool ChromaticFM::popDeferredEvent(ChromaticEvent& event) {
    const uint16_t read = eventRead.load(std::memory_order_relaxed);
    if (read == eventWrite.load(std::memory_order_acquire)) return false;
    event = events[read];
    eventRead.store((read + 1) & EVENT_MASK, std::memory_order_release);
    return true;
}

ChromaticAudioAligner::ChromaticAudioAligner() {
    normal = new (std::nothrow) ChromaticEvent[NORMAL_CAPACITY];
    shifted = new (std::nothrow) ChromaticEvent[SHIFTED_CAPACITY];
    preload = new (std::nothrow) ChromaticEvent[PRELOAD_CAPACITY];
    if (!normal || !shifted || !preload) failureCount = 1;
}

ChromaticAudioAligner::~ChromaticAudioAligner() {
    delete[] normal;
    delete[] shifted;
    delete[] preload;
}

void ChromaticAudioAligner::reset() {
    normalRead = normalWrite = shiftedRead = shiftedWrite = preloadCount = 0;
    hasPendingStop = false;
    rawPlaying = false;
    preloadArmed = false;
    activeShiftSamples = 0;
    companionShiftUntilRawSample = 0;
    compensatedStartCount = 0;
    maximumPreloadSpanSamples = 0;
    percussionHasPeriod = false;
    percussionHasOnset = false;
    percussionLastNaturalOnset = 0;
    percussionLastEffectiveOnset = 0;
    percussionPeriodQ8 = 0;
    percussionCalibrationTotal = 0;
    percussionCalibrationIntervals = 0;
    lastShiftedEffectiveSample = 0;
    noiseClusterCount = 0;
    stabilizedStartCount = 0;
    maximumRhythmCorrectionSamples = 0;
    failureCount = (!normal || !shifted || !preload) ? 1 : 0;
}

bool ChromaticAudioAligner::hasShiftedCapacity(uint16_t count) const {
    const uint16_t used = (shiftedWrite - shiftedRead) & SHIFTED_MASK;
    return count <= SHIFTED_MASK - used;
}

bool ChromaticAudioAligner::enqueueNormal(const ChromaticEvent& event) {
    const uint16_t next = (normalWrite + 1) & NORMAL_MASK;
    if (next == normalRead) {
        failureCount++;
        return false;
    }
    normal[normalWrite] = event;
    normalWrite = next;
    return true;
}

bool ChromaticAudioAligner::enqueueShifted(const ChromaticEvent& event, uint32_t effectiveSample) {
    const uint16_t next = (shiftedWrite + 1) & SHIFTED_MASK;
    if (next == shiftedRead) {
        failureCount++;
        return false;
    }
    // Why not order against every shifted event: the ADPCM stream feed rides a
    // whole-transaction offset, so its samples run far ahead of the hat grid
    // and would veto every stabilizing pull-back on the next noise onset.
    const bool ordersAgainstNoise = isYmNoiseWrite(event);
    const uint32_t monotonicSample =
        ordersAgainstNoise ? std::max(effectiveSample, lastShiftedEffectiveSample) : effectiveSample;
    if (ordersAgainstNoise) lastShiftedEffectiveSample = monotonicSample;
    shifted[shiftedWrite] = event;
    shifted[shiftedWrite].sample = monotonicSample;
    shiftedWrite = next;
    return true;
}

bool ChromaticAudioAligner::scheduleRaw(const ChromaticEvent& event) { return enqueueNormal(event); }

bool ChromaticAudioAligner::scheduleShifted(const ChromaticEvent& event) {
    const uint32_t effectiveSample = event.sample >= activeShiftSamples ? event.sample - activeShiftSamples : 0;
    return enqueueShifted(event, effectiveSample);
}

bool ChromaticAudioAligner::flushPreload() {
    bool success = true;
    if (hasPendingStop) {
        success = scheduleRaw(pendingStop) && success;
        hasPendingStop = false;
    }
    for (uint16_t index = 0; index < preloadCount; index++) success = scheduleRaw(preload[index]) && success;
    preloadCount = 0;
    return success;
}

uint32_t ChromaticAudioAligner::stabilizePercussionOnset(uint32_t naturalSample, uint32_t latestSample) {
    constexpr int32_t MAXIMUM_CORRECTION_SAMPLES = 1536;
    constexpr uint8_t CALIBRATION_INTERVALS = 4;
    // Why not keep a per-source PLL: ADPCM hits and CH7 hats alternate on the
    // same sixteenth grid, so two independent loops each see half the pulses
    // and cannot lock the subdivision they do not observe.
    if (!rhythmStabilizationEnabled) return naturalSample;
    if (!percussionHasOnset) {
        percussionHasOnset = true;
        percussionLastNaturalOnset = naturalSample;
        percussionLastEffectiveOnset = naturalSample;
        return naturalSample;
    }

    const uint32_t observedInterval = naturalSample - percussionLastNaturalOnset;
    // The ADPCM transaction anchors on its STOP, so the CH7 hat voicing the
    // same beat trails it by a serialization delay. Why not treat it as its own
    // pulse: a sub-period interval rounds to zero and resets the whole loop.
    const bool repeatsCurrentOnset = observedInterval < ADPCM_HIT_SPREAD_SAMPLES;
    if (repeatsCurrentOnset) return std::min(percussionLastEffectiveOnset, latestSample);
    if (!percussionHasPeriod) {
        percussionCalibrationTotal += observedInterval;
        percussionCalibrationIntervals++;
        const bool calibrationComplete = percussionCalibrationIntervals == CALIBRATION_INTERVALS;
        if (calibrationComplete) {
            percussionPeriodQ8 = static_cast<uint32_t>(percussionCalibrationTotal / CALIBRATION_INTERVALS) << 8;
            percussionHasPeriod = true;
        }
        percussionLastNaturalOnset = naturalSample;
        percussionLastEffectiveOnset = naturalSample;
        return naturalSample;
    }

    const uint32_t period = percussionPeriodQ8 >> 8;
    const uint32_t pulseCount = (observedInterval + period / 2) / period;
    const bool intervalIsRhythmic = pulseCount >= 1 && pulseCount <= 8;
    if (!intervalIsRhythmic) {
        percussionHasPeriod = false;
        percussionCalibrationTotal = 0;
        percussionCalibrationIntervals = 0;
        percussionLastNaturalOnset = naturalSample;
        percussionLastEffectiveOnset = naturalSample;
        return naturalSample;
    }

    const int64_t observedPeriodQ8 = (static_cast<int64_t>(observedInterval) << 8) / pulseCount;
    // Why not a slower period filter: the sixteenth grid is stationary, so a
    // long time constant only lets the seeded period stay wrong long enough
    // for the phase loop to run its correction into the clamp.
    percussionPeriodQ8 =
        static_cast<uint32_t>(static_cast<int64_t>(percussionPeriodQ8) + (observedPeriodQ8 - percussionPeriodQ8) / 16);
    const int64_t predictedSample = static_cast<int64_t>(percussionLastEffectiveOnset) +
                                    static_cast<int64_t>(pulseCount) * (percussionPeriodQ8 >> 8);
    const int64_t phaseError = static_cast<int64_t>(naturalSample) - predictedSample;
    const int64_t stabilizedSample = predictedSample + phaseError / 16;
    const int64_t phaseCorrection = stabilizedSample - naturalSample;
    const int64_t boundedCorrection =
        std::clamp<int64_t>(phaseCorrection, -MAXIMUM_CORRECTION_SAMPLES, MAXIMUM_CORRECTION_SAMPLES);
    const int64_t correctedSample = std::max<int64_t>(static_cast<int64_t>(naturalSample) + boundedCorrection, 0);
    const uint32_t effectiveSample = std::min(static_cast<uint32_t>(correctedSample), latestSample);
    const int32_t appliedCorrection = static_cast<int32_t>(effectiveSample) - static_cast<int32_t>(naturalSample);

    percussionLastNaturalOnset = naturalSample;
    percussionLastEffectiveOnset = effectiveSample;
    if (appliedCorrection != 0) {
        stabilizedStartCount++;
        maximumRhythmCorrectionSamples =
            std::max(maximumRhythmCorrectionSamples, static_cast<uint32_t>(std::abs(appliedCorrection)));
    }
    return effectiveSample;
}

bool ChromaticAudioAligner::holdNoiseClusterEvent(const ChromaticEvent& event) {
    const bool clusterHasRoom = noiseClusterCount < NOISE_CLUSTER_CAPACITY;
    if (!clusterHasRoom) return releaseStaleNoiseCluster(event.sample) && scheduleRaw(event);
    noiseCluster[noiseClusterCount++] = event;
    return true;
}

bool ChromaticAudioAligner::flushNoiseCluster(uint32_t effectiveSample) {
    bool success = true;
    for (uint8_t index = 0; index < noiseClusterCount; index++) {
        success = enqueueShifted(noiseCluster[index], effectiveSample) && success;
    }
    noiseClusterCount = 0;
    return success;
}

bool ChromaticAudioAligner::releaseStaleNoiseCluster(uint32_t rawSample) {
    if (noiseClusterCount == 0) return true;
    const bool clusterStillArming = rawSample - noiseCluster[0].sample <= NOISE_CLUSTER_SAMPLES;
    if (clusterStillArming) return true;
    bool success = true;
    for (uint8_t index = 0; index < noiseClusterCount; index++) success = scheduleRaw(noiseCluster[index]) && success;
    noiseClusterCount = 0;
    return success;
}

void ChromaticAudioAligner::shiftRecentChordKeyOns(uint32_t naturalSample, int32_t correction) {
    if (correction == 0) return;
    constexpr uint32_t CHORD_LOOKBACK_SAMPLES = 256;
    const uint32_t firstSample = naturalSample > CHORD_LOOKBACK_SAMPLES ? naturalSample - CHORD_LOOKBACK_SAMPLES : 0;
    for (uint16_t index = normalRead; index != normalWrite; index = (index + 1) & NORMAL_MASK) {
        ChromaticEvent& event = normal[index];
        const bool isRecent = event.sample >= firstSample && event.sample <= naturalSample;
        const bool isChordKeyOn = event.type == ChromaticEventType::YmWrite && event.address == 0x08 &&
                                  (event.value & 0x78) != 0 && (event.value & 0x07) <= 2;
        if (!isRecent || !isChordKeyOn) continue;
        event.sample = static_cast<uint32_t>(static_cast<int64_t>(event.sample) + correction);
    }
}

bool ChromaticAudioAligner::scheduleCompensatedStart(const ChromaticEvent& playEvent) {
    const uint32_t naturalSample = hasPendingStop ? pendingStop.sample : preload[0].sample;
    const uint32_t naturalPreloadSpan = playEvent.sample - naturalSample;
    const uint32_t effectiveSample = stabilizePercussionOnset(naturalSample, playEvent.sample);
    const int32_t rhythmCorrection = static_cast<int32_t>(effectiveSample) - static_cast<int32_t>(naturalSample);
    shiftRecentChordKeyOns(naturalSample, rhythmCorrection);
    const uint32_t preloadSpan = playEvent.sample - effectiveSample;
    const uint16_t scheduledEventCount = preloadCount + 1 + (hasPendingStop ? 1 : 0);
    const bool preloadsFit = hasShiftedCapacity(scheduledEventCount);
    if (!preloadsFit) {
        failureCount++;
        const bool flushed = flushPreload();
        return scheduleRaw(playEvent) && flushed;
    }

    // STOP, accepted bytes, and PLAY form one acoustic transaction. Why not
    // shift STOP with the preceding stream: that creates a roughly one-prime
    // silent hole before every retrigger even though the next hit is ready.
    bool insertedPreload = true;
    if (hasPendingStop) {
        insertedPreload = enqueueShifted(pendingStop, effectiveSample);
        hasPendingStop = false;
    }
    for (uint16_t index = 0; index < preloadCount; index++) {
        insertedPreload = enqueueShifted(preload[index], effectiveSample) && insertedPreload;
    }
    preloadCount = 0;
    const bool insertedPlay = enqueueShifted(playEvent, effectiveSample);
    if (insertedPreload && insertedPlay) {
        activeShiftSamples = preloadSpan;
        // Keep classification on the raw serialization span. Why not use the
        // corrected span: delaying a hit can shrink it below the following
        // bass write and leave that one onset outside the compensated group.
        companionShiftUntilRawSample = playEvent.sample + naturalPreloadSpan;
        compensatedStartCount++;
        maximumPreloadSpanSamples = std::max(maximumPreloadSpanSamples, preloadSpan);
        return true;
    }
    return false;
}

bool ChromaticAudioAligner::ingest(const ChromaticEvent& event) {
    const bool storageAvailable = normal != nullptr && shifted != nullptr && preload != nullptr;
    if (!storageAvailable) return false;
    const bool isControl = event.type == ChromaticEventType::Control;
    const bool isFifoWrite = event.type == ChromaticEventType::FifoWrite;
    if (isFifoWrite && !rawPlaying && preloadArmed) {
        if (preloadCount < PRELOAD_CAPACITY) {
            preload[preloadCount++] = event;
            return true;
        }
        failureCount++;
        const bool flushed = flushPreload();
        preloadArmed = false;
        return scheduleRaw(event) && flushed;
    }
    if (!isControl) {
        const bool shiftsAdpcmFeed = isFifoWrite && rawPlaying && activeShiftSamples > 0;
        const bool insideCompanionWindow =
            rawPlaying && activeShiftSamples > 0 && event.sample <= companionShiftUntilRawSample;
        // CH7 noise is the 16th-note pulse between ADPCM hits. Why not leave
        // it on the raw timeline: moving only the one-shot makes consecutive
        // percussion subdivisions alternately short and long.
        const bool isNoiseWrite = CHROMATIC_SHIFT_RHYTHM_NOISE != 0 && isYmNoiseWrite(event);
        const bool stabilizesStandaloneNoise = isNoiseWrite && rhythmStabilizationEnabled && !insideCompanionWindow;
        if (stabilizesStandaloneNoise) {
            const bool keysOnNoise = event.type == ChromaticEventType::YmWrite && event.address == 0x08 &&
                                     (event.value & 0x07) == 7 && (event.value & 0x78) != 0;
            // The ROM re-arms CH7 with a key-off plus noise-rate writes in the
            // eight samples ahead of the key-on. Why not shift them by the
            // previous hit's offset: they belong to the hit being armed, and
            // trailing the new key-on would silence it.
            if (!keysOnNoise) return holdNoiseClusterEvent(event);
            const uint32_t effectiveSample = stabilizePercussionOnset(event.sample, UINT32_MAX);
            const bool flushedCluster = flushNoiseCluster(effectiveSample);
            return enqueueShifted(event, effectiveSample) && flushedCluster;
        }
        const bool releasedCluster = releaseStaleNoiseCluster(event.sample);
        const bool shiftsRhythmNoise = isNoiseWrite && rawPlaying && activeShiftSamples > 0;
        const bool followsAcousticTimeline = shiftsAdpcmFeed || insideCompanionWindow || shiftsRhythmNoise;
        const bool scheduled = followsAcousticTimeline ? scheduleShifted(event) : scheduleRaw(event);
        return scheduled && releasedCluster;
    }

    const bool releasedClusterBeforeControl = releaseStaleNoiseCluster(event.sample);
    const bool willPlay = (event.value & 0x08) != 0;
    const bool startsPlaying = !rawPlaying && willPlay;
    if (startsPlaying) {
        const bool hasPreload = preloadArmed && preloadCount > 0;
        const uint32_t preloadStartSample =
            hasPreload ? (hasPendingStop ? pendingStop.sample : preload[0].sample) : event.sample;
        const uint32_t preloadSpan = hasPreload ? event.sample - preloadStartSample : 0;
        const bool fitsLookahead = hasPreload && preloadSpan <= LOOKAHEAD_SAMPLES;
        const bool scheduledStart =
            fitsLookahead ? scheduleCompensatedStart(event) : (flushPreload() && scheduleRaw(event));
        rawPlaying = true;
        preloadArmed = false;
        return scheduledStart && releasedClusterBeforeControl;
    }

    const bool stopsPlaying = rawPlaying && !willPlay;
    if (stopsPlaying) {
        preloadCount = 0;
        pendingStop = event;
        hasPendingStop = true;
        rawPlaying = false;
        preloadArmed = true;
        activeShiftSamples = 0;
        companionShiftUntilRawSample = 0;
        return releasedClusterBeforeControl;
    }
    const bool scheduledControl = scheduleRaw(event);
    rawPlaying = willPlay;
    if (!willPlay) {
        preloadArmed = true;
        activeShiftSamples = 0;
        companionShiftUntilRawSample = 0;
    }
    return scheduledControl && releasedClusterBeforeControl;
}

bool ChromaticAudioAligner::advanceRawHorizon(uint32_t sample) {
    const bool releasedCluster = releaseStaleNoiseCluster(sample);
    if (preloadCount == 0 && !hasPendingStop) return releasedCluster;
    const uint32_t oldestPreloadSample = hasPendingStop ? pendingStop.sample : preload[0].sample;
    const uint32_t preloadAge = sample - oldestPreloadSample;
    if (preloadAge <= LOOKAHEAD_SAMPLES) return releasedCluster;
    preloadArmed = false;
    return flushPreload() && releasedCluster;
}

bool ChromaticAudioAligner::popDueEvent(uint32_t acousticSample, ChromaticEvent& event) {
    const bool hasNormal = normalRead != normalWrite;
    const bool hasShifted = shiftedRead != shiftedWrite;
    if (!hasNormal && !hasShifted) return false;
    const bool normalComesFirst =
        hasNormal && (!hasShifted || normal[normalRead].sample <= shifted[shiftedRead].sample);
    const ChromaticEvent& nextEvent = normalComesFirst ? normal[normalRead] : shifted[shiftedRead];
    if (nextEvent.sample > acousticSample) return false;
    event = nextEvent;
    if (normalComesFirst) {
        normalRead = (normalRead + 1) & NORMAL_MASK;
    } else {
        shiftedRead = (shiftedRead + 1) & SHIFTED_MASK;
    }
    return true;
}

}   // namespace gb
