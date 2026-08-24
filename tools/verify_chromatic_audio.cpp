// What this guarantees: CoreS3 deferred YM2151/ADPCM events reproduce the
// synchronous KANTAN demo at the exact sample where each event was emitted.
#include "../core/gb.h"
#include "../m5stack/src/kantan_autoplay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

std::vector<uint8_t> loadRom(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool appendFrame(std::vector<float>& samples, gb::GB& system) {
    const int sampleCount = system.apu.sampleCount;
    samples.insert(samples.end(), system.apu.sampleBuf, system.apu.sampleBuf + sampleCount * 2);
    system.apu.sampleCount = 0;
    return sampleCount > 0;
}

uint32_t hashFloat(uint32_t hash, float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float hash expects IEEE-sized storage");
    std::memcpy(&bits, &value, sizeof(bits));
    for (int byte = 0; byte < 4; byte++) hash = (hash ^ ((bits >> (byte * 8)) & 0xff)) * 16777619U;
    return hash;
}

struct PercussionIntervalRange {
    static constexpr uint32_t CLUSTER_SAMPLES = 64;
    static constexpr uint32_t WARMUP_ONSETS = 24;

    void addOnset(uint32_t sample) {
        const bool joinsCurrentHit = lastOnset != 0 && sample - lastOnset <= CLUSTER_SAMPLES;
        if (joinsCurrentHit) return;
        onsetCount++;
        const bool measuresSteadyState = lastOnset != 0 && onsetCount > WARMUP_ONSETS;
        if (measuresSteadyState) {
            const uint32_t interval = sample - lastOnset;
            minimum = std::min(minimum, interval);
            maximum = std::max(maximum, interval);
        }
        lastOnset = sample;
    }

    uint32_t range() const { return maximum - minimum; }

    uint32_t lastOnset = 0;
    uint32_t onsetCount = 0;
    uint32_t minimum = UINT32_MAX;
    uint32_t maximum = 0;
};

bool verifyAlignmentContract() {
    const auto collect = [](const std::vector<gb::ChromaticEvent>& input, uint32_t horizon) {
        gb::ChromaticAudioAligner aligner;
        for (const gb::ChromaticEvent& event : input) aligner.ingest(event);
        aligner.advanceRawHorizon(horizon);
        std::vector<gb::ChromaticEvent> output;
        gb::ChromaticEvent event;
        while (aligner.popDueEvent(horizon, event)) output.push_back(event);
        if (aligner.failures() != 0) output.clear();
        return output;
    };

    const std::vector<gb::ChromaticEvent> preloaded = {
        {100, gb::ChromaticEventType::Control, 0, 0x07},    {110, gb::ChromaticEventType::FifoWrite, 0, 0x12},
        {115, gb::ChromaticEventType::YmWrite, 0x20, 0x34}, {120, gb::ChromaticEventType::FifoWrite, 0, 0x56},
        {200, gb::ChromaticEventType::Control, 0, 0x0F},    {205, gb::ChromaticEventType::YmWrite, 0x08, 0x47},
    };
    const std::vector<gb::ChromaticEvent> aligned = collect(preloaded, 2000);
    const bool preloadedAligned = aligned.size() == 6 && aligned[0].sample == 100 && aligned[1].sample == 110 &&
                                  aligned[1].type == gb::ChromaticEventType::FifoWrite && aligned[2].sample == 110 &&
                                  aligned[2].type == gb::ChromaticEventType::FifoWrite && aligned[3].sample == 110 &&
                                  aligned[3].type == gb::ChromaticEventType::Control && aligned[4].sample == 115 &&
                                  aligned[5].sample == 115;

    const std::vector<gb::ChromaticEvent> streaming = {
        {10, gb::ChromaticEventType::Control, 0, 0x0F},
        {20, gb::ChromaticEventType::FifoWrite, 0, 0x78},
    };
    const std::vector<gb::ChromaticEvent> streamed = collect(streaming, 2000);
    const bool streamingUnchanged = streamed.size() == 2 && streamed[0].sample == 10 && streamed[1].sample == 20;

    const uint32_t longPlaySample = gb::ChromaticAudioAligner::LOOKAHEAD_SAMPLES + 200;
    const std::vector<gb::ChromaticEvent> longPreload = {
        {0, gb::ChromaticEventType::Control, 0, 0x07},
        {10, gb::ChromaticEventType::FifoWrite, 0, 0x9A},
        {longPlaySample, gb::ChromaticEventType::Control, 0, 0x0F},
    };
    const std::vector<gb::ChromaticEvent> longResult = collect(longPreload, longPlaySample + 1000);
    const bool longPreloadUnchanged = longResult.size() == 3 && longResult[0].sample == 0 &&
                                      longResult[1].sample == 10 && longResult[2].sample == longPlaySample;
    return preloadedAligned && streamingUnchanged && longPreloadUnchanged;
}

}   // namespace

int runVerification(int argc, char** argv) {
    const char* romPath = argc > 1 ? argv[1] : "m5stack/data/kantan-gb-play.gbc";
    const int frames = argc > 2 ? std::atoi(argv[2]) : 3500;
    const int maximumAllowedChordPlayOffset = argc > 3 ? std::atoi(argv[3]) : -1;
    const bool validFrames = frames > 0 && frames <= 10000;
    const bool validOffset = maximumAllowedChordPlayOffset >= -1;
    if (!validFrames || !validOffset) {
        std::fprintf(stderr, "usage: verify_chromatic_audio [rom] [frames 1..10000] [maximum chord-to-play samples]\n");
        return 2;
    }
    if (!verifyAlignmentContract()) {
        std::fprintf(stderr, "Chromatic alignment contract failed\n");
        return 1;
    }

    const std::vector<uint8_t> rom = loadRom(romPath);
    if (rom.empty()) {
        std::fprintf(stderr, "ROM file could not be read: %s\n", romPath);
        return 1;
    }

    auto synchronous = std::make_unique<gb::GB>();
    auto producer = std::make_unique<gb::GB>();
    for (gb::GB* system : {synchronous.get(), producer.get()}) {
        system->apu.setSampleRate(44100.0);
        system->fm.enabled = true;
    }
    producer->fm.deferred = true;
    const bool loadedBoth = synchronous->loadRom(rom.data(), rom.size()) && producer->loadRom(rom.data(), rom.size());
    if (!loadedBoth) {
        std::fprintf(stderr, "KANTAN ROM was rejected\n");
        return 1;
    }

    std::vector<float> synchronousPcm;
    std::vector<gb::ChromaticEvent> events;
    synchronousPcm.reserve(static_cast<size_t>(frames) * 800 * 2);
    for (int frame = 0; frame < frames; frame++) {
        const uint8_t input = stackchan::kantanDemoInput(static_cast<uint32_t>(frame));
        synchronous->buttons = input;
        producer->buttons = input;
        synchronous->runFrame();
        producer->runFrame();
        const bool sampleCountsMatch = synchronous->apu.sampleCount == producer->apu.sampleCount;
        if (!sampleCountsMatch || !appendFrame(synchronousPcm, *synchronous)) {
            std::fprintf(stderr, "sample production diverged at frame %d\n", frame);
            return 1;
        }
        producer->apu.sampleCount = 0;
        gb::ChromaticEvent event;
        while (producer->fm.popDeferredEvent(event)) events.push_back(event);
    }

    gb::ChromaticFM renderer;
    renderer.enabled = true;
    renderer.reset();
    size_t eventIndex = 0;
    float maximumDifference = 0;
    uint32_t rawPcmHash = 2166136261U;
    uint32_t rawAdpcmDecoderHash = 2166136261U;
    uint32_t lastChordChannel2Sample = 0;
    uint32_t minimumChordPlayOffset = UINT32_MAX;
    uint32_t maximumChordPlayOffset = 0;
    uint32_t measuredChordPlayOffsets = 0;
    uint32_t adpcmStarvations = 0;
    uint32_t firstAdpcmStarvationSample = 0;
    uint32_t firstAdpcmStarvationRefillGap = 0;
    uint32_t fifoWritesSincePlay = 0;
    uint32_t firstAdpcmStarvationWritesSincePlay = 0;
    uint32_t lastFifoWriteSample = 0;
    uint32_t lastRawStartSample = 0;
    uint32_t rawNormalizedIntervalMinimum = UINT32_MAX;
    uint32_t rawNormalizedIntervalMaximum = 0;
    uint32_t rawStarts = 0;
    PercussionIntervalRange rawPercussionIntervals;
    bool adpcmStreamIsBeingFed = false;
    const size_t sampleCount = synchronousPcm.size() / 2;
    for (size_t sample = 0; sample < sampleCount; sample++) {
        while (eventIndex < events.size() && events[eventIndex].sample <= sample) {
            const gb::ChromaticEvent& event = events[eventIndex];
            const bool lateEvent = event.sample < sample;
            if (lateEvent) {
                std::fprintf(stderr, "late event at sample %zu: timestamp=%u\n", sample, event.sample);
                return 1;
            }
            const bool keysOnChordChannel2 = event.type == gb::ChromaticEventType::YmWrite && event.address == 0x08 &&
                                             (event.value & 0x78) != 0 && (event.value & 0x07) == 2;
            const bool keysOnNoiseChannel = event.type == gb::ChromaticEventType::YmWrite && event.address == 0x08 &&
                                            (event.value & 0x78) != 0 && (event.value & 0x07) == 7;
            if (keysOnChordChannel2) lastChordChannel2Sample = event.sample;

            const bool writesFifo = event.type == gb::ChromaticEventType::FifoWrite;
            const bool starvesBeforeRefill = writesFifo && adpcmStreamIsBeingFed && !renderer.adpcmActive;
            if (starvesBeforeRefill) {
                if (adpcmStarvations == 0) {
                    firstAdpcmStarvationSample = event.sample;
                    firstAdpcmStarvationRefillGap = event.sample - lastFifoWriteSample;
                    firstAdpcmStarvationWritesSincePlay = fifoWritesSincePlay;
                }
                adpcmStarvations++;
            }
            if (writesFifo) {
                lastFifoWriteSample = event.sample;
                fifoWritesSincePlay++;
            }

            const bool changesControl = event.type == gb::ChromaticEventType::Control;
            const bool wasPlaying = (renderer.audioControl & 0x08) != 0;
            const bool willPlay = changesControl && (event.value & 0x08) != 0;
            const bool startsPlaying = changesControl && !wasPlaying && willPlay;
            const bool stopsPlaying = changesControl && wasPlaying && !willPlay;
            if (stopsPlaying) adpcmStreamIsBeingFed = false;
            if (startsPlaying) {
                rawStarts++;
                constexpr uint32_t RHYTHM_CALIBRATION_STARTS = 6;
                const bool hasCalibratedRawInterval = lastRawStartSample != 0 && rawStarts > RHYTHM_CALIBRATION_STARTS;
                if (hasCalibratedRawInterval) {
                    constexpr uint32_t HALF_PERIOD_THRESHOLD = 15000;
                    const uint32_t interval = event.sample - lastRawStartSample;
                    const uint32_t normalizedInterval = interval < HALF_PERIOD_THRESHOLD ? interval * 2 : interval;
                    rawNormalizedIntervalMinimum = std::min(rawNormalizedIntervalMinimum, normalizedInterval);
                    rawNormalizedIntervalMaximum = std::max(rawNormalizedIntervalMaximum, normalizedInterval);
                }
                lastRawStartSample = event.sample;
                adpcmStreamIsBeingFed = true;
                fifoWritesSincePlay = 0;
                const uint32_t chordPlayOffset = event.sample - lastChordChannel2Sample;
                constexpr uint32_t MAXIMUM_ASSOCIATED_CHORD_OFFSET = 2048;
                const bool followsChord =
                    lastChordChannel2Sample != 0 && chordPlayOffset <= MAXIMUM_ASSOCIATED_CHORD_OFFSET;
                if (followsChord) {
                    minimumChordPlayOffset = std::min(minimumChordPlayOffset, chordPlayOffset);
                    maximumChordPlayOffset = std::max(maximumChordPlayOffset, chordPlayOffset);
                    measuredChordPlayOffsets++;
                }
            }
            const bool startsPercussion = startsPlaying || keysOnNoiseChannel;
            if (startsPercussion) rawPercussionIntervals.addOnset(event.sample);
            renderer.applyDeferredEvent(event);
            eventIndex++;
        }
        renderer.generateSample(44100.0);
        rawAdpcmDecoderHash = hashFloat(rawAdpcmDecoderHash, renderer.adpcmRawOut);
        float left = 0;
        float right = 0;
        if (renderer.audioControl & 0x01) {
            left += renderer.ymL;
            right += renderer.ymR;
        }
        if (renderer.audioControl & 0x04) {
            left += renderer.adpcmOut * 0.5f;
            right += renderer.adpcmOut * 0.5f;
        }
        const float leftDifference = std::abs(left - synchronousPcm[sample * 2]);
        const float rightDifference = std::abs(right - synchronousPcm[sample * 2 + 1]);
        rawPcmHash = hashFloat(hashFloat(rawPcmHash, left), right);
        maximumDifference = std::max(maximumDifference, std::max(leftDifference, rightDifference));
        const bool sampleMatches = leftDifference == 0 && rightDifference == 0;
        if (!sampleMatches) {
            std::fprintf(stderr, "PCM mismatch at sample %zu: sync=(%.9g,%.9g) deferred=(%.9g,%.9g) diff=%.9g\n",
                         sample, synchronousPcm[sample * 2], synchronousPcm[sample * 2 + 1], left, right,
                         std::max(leftDifference, rightDifference));
            return 1;
        }
    }

    if (adpcmStarvations != 0) {
        std::fprintf(stderr,
                     "Raw ADPCM continuity failed: starvation=%u first_starvation_sample=%u first_refill_gap=%u "
                     "first_writes_since_play=%u\n",
                     adpcmStarvations, firstAdpcmStarvationSample, firstAdpcmStarvationRefillGap,
                     firstAdpcmStarvationWritesSincePlay);
        return 1;
    }

    gb::ChromaticFM alignedRenderer;
    gb::ChromaticAudioAligner aligner;
    aligner.setRhythmStabilization(true);
    alignedRenderer.enabled = true;
    alignedRenderer.reset();
    size_t alignedEventIndex = 0;
    uint32_t alignedChordChannel0Sample = 0;
    uint32_t alignedChordChannel2Sample = 0;
    uint32_t alignedMaximumChannel0Offset = 0;
    uint32_t alignedMaximumChannel2Offset = 0;
    uint32_t alignedMaximumHatOffset = 0;
    uint32_t alignedMaximumBassOffset = 0;
    uint32_t alignedLastPlaySample = 0;
    uint32_t alignedLastStopSample = 0;
    uint32_t alignedMaximumStopPlayGap = 0;
    uint32_t alignedStopPlayPairs = 0;
    uint32_t alignedMaximumRetriggerStep = 0;
    uint32_t alignedNormalizedIntervalMinimum = UINT32_MAX;
    uint32_t alignedNormalizedIntervalMaximum = 0;
    float alignedAdpcmBeforeRetrigger = 0;
    uint32_t alignedStarts = 0;
    uint32_t alignedChordStarts = 0;
    uint32_t alignedStarvations = 0;
    PercussionIntervalRange alignedPercussionIntervals;
    bool alignedStreamIsBeingFed = false;
    const size_t alignedSampleCount = sampleCount > gb::ChromaticAudioAligner::LOOKAHEAD_SAMPLES
                                          ? sampleCount - gb::ChromaticAudioAligner::LOOKAHEAD_SAMPLES
                                          : 0;
    for (size_t sample = 0; sample < alignedSampleCount; sample++) {
        bool measuresRetriggerAfterGenerate = false;
        const uint32_t rawHorizon = static_cast<uint32_t>(sample) + gb::ChromaticAudioAligner::LOOKAHEAD_SAMPLES;
        while (alignedEventIndex < events.size() && events[alignedEventIndex].sample <= rawHorizon) {
            if (!aligner.ingest(events[alignedEventIndex])) {
                std::fprintf(stderr, "alignment ingest failed at raw sample %u\n", events[alignedEventIndex].sample);
                return 1;
            }
            alignedEventIndex++;
        }
        if (!aligner.advanceRawHorizon(rawHorizon)) {
            std::fprintf(stderr, "alignment horizon failed at raw sample %u\n", rawHorizon);
            return 1;
        }

        gb::ChromaticEvent event;
        while (aligner.popDueEvent(static_cast<uint32_t>(sample), event)) {
            const bool keyOn =
                event.type == gb::ChromaticEventType::YmWrite && event.address == 0x08 && (event.value & 0x78) != 0;
            if (keyOn && (event.value & 0x07) == 0) alignedChordChannel0Sample = event.sample;
            if (keyOn && (event.value & 0x07) == 2) alignedChordChannel2Sample = event.sample;
            if (keyOn && alignedLastPlaySample != 0 && event.sample >= alignedLastPlaySample) {
                const uint32_t onsetOffset = event.sample - alignedLastPlaySample;
                constexpr uint32_t MAXIMUM_ASSOCIATED_ONSET_OFFSET = 2048;
                if (onsetOffset <= MAXIMUM_ASSOCIATED_ONSET_OFFSET && (event.value & 0x07) == 7) {
                    alignedMaximumHatOffset = std::max(alignedMaximumHatOffset, onsetOffset);
                }
                if (onsetOffset <= MAXIMUM_ASSOCIATED_ONSET_OFFSET && (event.value & 0x07) == 4) {
                    alignedMaximumBassOffset = std::max(alignedMaximumBassOffset, onsetOffset);
                }
            }

            const bool changesControl = event.type == gb::ChromaticEventType::Control;
            const bool wasPlaying = (alignedRenderer.audioControl & 0x08) != 0;
            const bool willPlay = changesControl && (event.value & 0x08) != 0;
            const bool startsPlaying = changesControl && !wasPlaying && willPlay;
            const bool stopsPlaying = changesControl && wasPlaying && !willPlay;
            if (stopsPlaying) {
                alignedStreamIsBeingFed = false;
                alignedLastStopSample = event.sample;
                alignedAdpcmBeforeRetrigger = alignedRenderer.adpcmOut;
            }
            if (startsPlaying) {
                alignedStreamIsBeingFed = true;
                alignedStarts++;
                constexpr uint32_t RHYTHM_CALIBRATION_STARTS = 6;
                const bool hasCalibratedAlignedInterval =
                    alignedLastPlaySample != 0 && alignedStarts > RHYTHM_CALIBRATION_STARTS;
                if (hasCalibratedAlignedInterval) {
                    constexpr uint32_t HALF_PERIOD_THRESHOLD = 15000;
                    const uint32_t interval = event.sample - alignedLastPlaySample;
                    const uint32_t normalizedInterval = interval < HALF_PERIOD_THRESHOLD ? interval * 2 : interval;
                    alignedNormalizedIntervalMinimum = std::min(alignedNormalizedIntervalMinimum, normalizedInterval);
                    alignedNormalizedIntervalMaximum = std::max(alignedNormalizedIntervalMaximum, normalizedInterval);
                }
                alignedLastPlaySample = event.sample;
                if (alignedLastStopSample != 0 && event.sample >= alignedLastStopSample) {
                    alignedMaximumStopPlayGap =
                        std::max(alignedMaximumStopPlayGap, event.sample - alignedLastStopSample);
                    alignedStopPlayPairs++;
                    measuresRetriggerAfterGenerate = event.sample == alignedLastStopSample;
                }
                constexpr uint32_t MAXIMUM_ASSOCIATED_CHORD_OFFSET = 2048;
                const uint32_t channel0Offset = event.sample - alignedChordChannel0Sample;
                const uint32_t channel2Offset = event.sample - alignedChordChannel2Sample;
                const bool followsChord = alignedChordChannel0Sample != 0 && alignedChordChannel2Sample != 0 &&
                                          event.sample >= alignedChordChannel0Sample &&
                                          event.sample >= alignedChordChannel2Sample &&
                                          channel2Offset <= MAXIMUM_ASSOCIATED_CHORD_OFFSET;
                if (followsChord) {
                    alignedMaximumChannel0Offset = std::max(alignedMaximumChannel0Offset, channel0Offset);
                    alignedMaximumChannel2Offset = std::max(alignedMaximumChannel2Offset, channel2Offset);
                    alignedChordStarts++;
                }
            }
            const bool keysOnNoiseChannel = keyOn && (event.value & 0x07) == 7;
            const bool startsPercussion = startsPlaying || keysOnNoiseChannel;
            if (startsPercussion) alignedPercussionIntervals.addOnset(event.sample);
            const bool refillsStarvedStream = event.type == gb::ChromaticEventType::FifoWrite &&
                                              alignedStreamIsBeingFed && !alignedRenderer.adpcmActive;
            if (refillsStarvedStream) alignedStarvations++;
            alignedRenderer.applyDeferredEvent(event);
        }
        alignedRenderer.generateSample(44100.0);
        if (measuresRetriggerAfterGenerate) {
            const uint32_t retriggerStep =
                static_cast<uint32_t>(std::abs(alignedRenderer.adpcmOut - alignedAdpcmBeforeRetrigger) * 32768.0f);
            alignedMaximumRetriggerStep = std::max(alignedMaximumRetriggerStep, retriggerStep);
        }
    }

    const bool hasRequiredOffsetMeasurements = maximumAllowedChordPlayOffset < 0 || alignedChordStarts > 0;
    const bool chordPlayOffsetPasses =
        maximumAllowedChordPlayOffset < 0 ||
        alignedMaximumChannel2Offset <= static_cast<uint32_t>(maximumAllowedChordPlayOffset);
    const uint32_t rawNormalizedIntervalRange = rawNormalizedIntervalMaximum - rawNormalizedIntervalMinimum;
    const uint32_t alignedNormalizedIntervalRange = alignedNormalizedIntervalMaximum - alignedNormalizedIntervalMinimum;
    // 3500-frame measurement of the stabilized grid is 273 samples; the ceiling
    // keeps roughly 40% headroom for shorter runs whose warmup weighs more.
    constexpr uint32_t MAXIMUM_PERCUSSION_GRID_RANGE = 384;
    const bool percussionGridPasses = alignedPercussionIntervals.onsetCount > 0 &&
                                      alignedPercussionIntervals.range() < rawPercussionIntervals.range() &&
                                      alignedPercussionIntervals.range() <= MAXIMUM_PERCUSSION_GRID_RANGE;
    const bool rhythmStabilizationPasses =
        aligner.stabilizedStarts() > 0 && aligner.maximumRhythmCorrection() <= 1536 &&
        alignedNormalizedIntervalRange < rawNormalizedIntervalRange && percussionGridPasses;
    const bool alignmentPasses = aligner.compensatedStarts() > 0 && aligner.failures() == 0 &&
                                 alignedStarvations == 0 && hasRequiredOffsetMeasurements && chordPlayOffsetPasses &&
                                 alignedMaximumHatOffset <= 64 && alignedMaximumBassOffset <= 128 &&
                                 alignedStopPlayPairs > 0 && alignedMaximumStopPlayGap == 0 &&
                                 alignedMaximumRetriggerStep <= 1 && rhythmStabilizationPasses;
    if (!alignmentPasses) {
        std::fprintf(stderr,
                     "Aligned ADPCM timing failed: compensated=%u failures=%u starvation=%u starts=%u chord_starts=%u "
                     "ch0_max=%u ch2_max=%u hat_max=%u bass_max=%u stop_play_pairs=%u stop_play_max=%u "
                     "retrigger_step_max=%u stabilized=%u correction_max=%u raw_interval_range=%u "
                     "aligned_interval_range=%u raw_percussion_range=%u aligned_percussion_range=%u "
                     "allowed=%d span_max=%u\n",
                     aligner.compensatedStarts(), aligner.failures(), alignedStarvations, alignedStarts,
                     alignedChordStarts, alignedMaximumChannel0Offset, alignedMaximumChannel2Offset,
                     alignedMaximumHatOffset, alignedMaximumBassOffset, alignedStopPlayPairs, alignedMaximumStopPlayGap,
                     alignedMaximumRetriggerStep, aligner.stabilizedStarts(), aligner.maximumRhythmCorrection(),
                     rawNormalizedIntervalRange, alignedNormalizedIntervalRange, rawPercussionIntervals.range(),
                     alignedPercussionIntervals.range(), maximumAllowedChordPlayOffset, aligner.maximumPreloadSpan());
        return 1;
    }

    std::printf("chromatic_audio_parity frames=%d samples=%zu events=%zu max_diff=%.9g late_events=0 "
                "raw_adpcm_starvation=%u raw_chord_play_count=%u raw_chord_play_min=%u raw_chord_play_max=%u "
                "aligned_samples=%zu aligned_compensated=%u aligned_failures=%u aligned_starvation=%u "
                "aligned_ch0_max=%u aligned_ch2_max=%u aligned_hat_max=%u aligned_bass_max=%u "
                "aligned_stop_play_pairs=%u aligned_stop_play_max=%u aligned_retrigger_step_max=%u "
                "aligned_stabilized=%u aligned_correction_max=%u raw_interval_min=%u raw_interval_max=%u "
                "raw_interval_range=%u aligned_interval_min=%u aligned_interval_max=%u aligned_interval_range=%u "
                "raw_percussion_min=%u raw_percussion_max=%u raw_percussion_range=%u "
                "aligned_percussion_min=%u aligned_percussion_max=%u aligned_percussion_range=%u "
                "alignment_span_max=%u raw_pcm_hash=%08x raw_adpcm_hash=%08x\n",
                frames, sampleCount, events.size(), maximumDifference, adpcmStarvations, measuredChordPlayOffsets,
                measuredChordPlayOffsets > 0 ? minimumChordPlayOffset : 0, maximumChordPlayOffset, alignedSampleCount,
                aligner.compensatedStarts(), aligner.failures(), alignedStarvations, alignedMaximumChannel0Offset,
                alignedMaximumChannel2Offset, alignedMaximumHatOffset, alignedMaximumBassOffset, alignedStopPlayPairs,
                alignedMaximumStopPlayGap, alignedMaximumRetriggerStep, aligner.stabilizedStarts(),
                aligner.maximumRhythmCorrection(), rawNormalizedIntervalMinimum, rawNormalizedIntervalMaximum,
                rawNormalizedIntervalRange, alignedNormalizedIntervalMinimum, alignedNormalizedIntervalMaximum,
                alignedNormalizedIntervalRange, rawPercussionIntervals.minimum, rawPercussionIntervals.maximum,
                rawPercussionIntervals.range(), alignedPercussionIntervals.minimum, alignedPercussionIntervals.maximum,
                alignedPercussionIntervals.range(), aligner.maximumPreloadSpan(), rawPcmHash, rawAdpcmDecoderHash);
    return 0;
}

int main(int argc, char** argv) {
    try {
        return runVerification(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Chromatic audio verification failed: %s\n", error.what());
        return 1;
    }
}
