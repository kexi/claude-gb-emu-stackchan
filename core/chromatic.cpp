// Chromatic FM expansion: YM2151 (ymfm) + MSM6258 ADPCM decoder.
#include "chromatic.h"
#ifdef GB_EMBEDDED

#include <cstring>

namespace gb {

ChromaticFM::~ChromaticFM() = default;

void ChromaticFM::reset() {
    ymAddrLatch = 0;
    audioControl = 0x02;
    volumeLeft = volumeRight = 0x80;
    adpcmVolume = 8;
    memset(fifo, 0, sizeof(fifo));
    fifoRd = fifoWr = fifoCount = 0;
    adpcmActive = nibbleHi = false;
    adpcmSignal = adpcmStep = 0;
    adpcmAcc = ymAcc = 0;
    adpcmOut = ymL = ymR = 0;
}

uint8_t ChromaticFM::read(uint8_t) { return 0xFF; }
void ChromaticFM::write(uint8_t, uint8_t) {}
void ChromaticFM::generateSample(double) { ymL = ymR = adpcmOut = 0; }
void ChromaticFM::adpcmDecodeNibble() {}
void ChromaticFM::adpcmStart() {}
void ChromaticFM::adpcmStop() {}

} // namespace gb

#else

#include "ymfm/ymfm_opm.h"
#include <cstring>

namespace gb {

namespace {

// ymfm interface with a minimal timer implementation so games that poll the
// YM2151 timer flags (tempo timing) keep working
struct OpmWrap : public ymfm::ymfm_interface {
    ymfm::ym2151 chip{*this};
    int64_t timer[2] = {-1, -1};   // remaining clocks, <0 = disabled

    void ymfm_set_timer(uint32_t tnum, int32_t duration) override {
        if (tnum < 2) timer[tnum] = duration;   // negative disables
    }
    void stepClocks(int clocks) {
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
    16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552,
};

// per-volume gain, output = (signal * gain) >> 10
const int VOLUME_GAIN[16] = {
    267, 410, 492, 656, 799, 1045, 1270, 1536,
    2048, 2479, 2991, 3994, 5325, 6431, 8561, 10363,
};

const double YM_CLOCK = 3579545.0;
const double YM_SAMPLE_HZ = YM_CLOCK / 64.0;    // 55930.4 Hz
const double ADPCM_SAMPLE_HZ = 15625.0;

} // namespace

ChromaticFM::~ChromaticFM() { delete static_cast<OpmWrap*>(opm); }

void ChromaticFM::reset() {
    if (!opm) opm = new OpmWrap();
    OpmWrap* w = static_cast<OpmWrap*>(opm);
    w->chip.reset();
    w->timer[0] = w->timer[1] = -1;
    ymAddrLatch = 0;
    audioControl = 0x03;
    volumeLeft = volumeRight = 0x80;
    adpcmVolume = 8;
    memset(fifo, 0, sizeof(fifo));
    fifoRd = fifoWr = fifoCount = 0;
    adpcmActive = false; nibbleHi = false;
    adpcmSignal = 0; adpcmStep = 0;
    adpcmAcc = 0; adpcmOut = 0;
    ymAcc = 0; ymL = ymR = 0;
}

void ChromaticFM::adpcmStart() {
    adpcmActive = true;
    adpcmSignal = 0; adpcmStep = 0;
    adpcmAcc = 0; adpcmOut = 0;
}

void ChromaticFM::adpcmStop() {
    adpcmActive = false; adpcmOut = 0;
    fifoRd = fifoWr = fifoCount = 0; nibbleHi = false;
}

uint8_t ChromaticFM::read(uint8_t reg) {
    switch (reg) {
    case 0: return ymAddrLatch;
    case 2: {
        OpmWrap* w = static_cast<OpmWrap*>(opm);
        uint8_t busy = (w && (w->chip.read_status() & 0x80)) ? 0x80 : 0x00;
        uint8_t ready = (fifoCount < 256) ? 0x40 : 0x00;
        uint8_t playing = adpcmActive ? 0x20 : 0x00;
        return busy | ready | playing;
    }
    case 3: return audioControl;
    case 4: return volumeLeft;
    case 5: return volumeRight;
    case 6: return 0x51;               // expansion ID
    case 7: return 0x03;               // status-map version
    default: return 0xFF;              // FF29 is write-only
    }
}

void ChromaticFM::write(uint8_t reg, uint8_t v) {
    switch (reg) {
    case 0:
        ymAddrLatch = v;
        if (v == 0xFE) adpcmStart();
        else if (v == 0xFD) adpcmStop();
        break;
    case 1:
        if (ymAddrLatch == 0xFF) {
            if (fifoCount < 256) {
                fifo[fifoWr] = v;
                fifoWr = (fifoWr + 1) & 0xFF;
                fifoCount++;
            }
        } else if (ymAddrLatch == 0xFE) adpcmStart();
        else if (ymAddrLatch == 0xFD) adpcmStop();
        else {
            OpmWrap* w = static_cast<OpmWrap*>(opm);
            if (w) {
                w->chip.write_address(ymAddrLatch);
                w->chip.write_data(v);
            }
        }
        break;
    case 3:
        audioControl = v;
        adpcmVolume = v >> 4;
        break;
    case 4: volumeLeft = v; break;
    case 5: volumeRight = v; break;
    default: break;
    }
}

void ChromaticFM::adpcmDecodeNibble() {
    if (fifoCount == 0) { adpcmOut = 0; return; }
    uint8_t byte = fifo[fifoRd];
    int code = nibbleHi ? (byte >> 4) : (byte & 0x0F);
    if (nibbleHi) {
        fifoRd = (fifoRd + 1) & 0xFF;
        fifoCount--;
    }
    nibbleHi = !nibbleHi;

    int step = STEP_SIZE[adpcmStep];
    int delta = (step >> 3) + ((code & 1) ? (step >> 2) : 0) +
                ((code & 2) ? (step >> 1) : 0) + ((code & 4) ? step : 0);
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
    adpcmOut = scaled / 32768.0f;
}

void ChromaticFM::generateSample(double sampleRate) {
    OpmWrap* w = static_cast<OpmWrap*>(opm);
    if (!w) { ymL = ymR = adpcmOut = 0; return; }
    // YM2151: catch up to the host sample position
    ymAcc += YM_SAMPLE_HZ / sampleRate;
    while (ymAcc >= 1.0) {
        ymAcc -= 1.0;
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
        adpcmAcc += ADPCM_SAMPLE_HZ / sampleRate;
        while (adpcmAcc >= 1.0) {
            adpcmAcc -= 1.0;
            adpcmDecodeNibble();
        }
    }
}

} // namespace gb

#endif
