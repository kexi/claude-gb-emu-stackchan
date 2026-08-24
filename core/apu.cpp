// APU — 2 pulse + wave + noise, sampled to a float ring buffer
#include "gb.h"

namespace gb {

static const uint8_t DUTY[4][8] = {
    {0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,1},
    {1,0,0,0,0,1,1,1},
    {0,1,1,1,1,1,1,0},
};

void APU::reset() {
    memset(regs, 0, sizeof(regs));
    enabled = true;
    fsClock = 0; fsStep = 0;
    ch1On = ch2On = ch3On = ch4On = false;
    ch4Lfsr = 0x7FFF;
    sampleCount = 0; sampleAccum = 0;
    // post-boot register values
    static const uint8_t init[] = {
        0x80,0xBF,0xF3,0xFF,0xBF, 0xFF,0x3F,0x00,0xFF,0xBF,
        0x7F,0xFF,0x9F,0xFF,0xBF, 0xFF,0xFF,0x00,0x00,0xBF, 0x77,0xF3,0xF1,
    };
    memcpy(regs, init, sizeof(init));
}

uint8_t APU::read(uint8_t reg) const {
    // OR masks for unreadable bits (FF10-FF26)
    static const uint8_t mask[] = {
        0x80,0x3F,0x00,0xFF,0xBF, 0xFF,0x3F,0x00,0xFF,0xBF,
        0x7F,0xFF,0x9F,0xFF,0xBF, 0xFF,0xFF,0x00,0x00,0xBF, 0x00,0x00,0x70,
    };
    if (reg >= 0x20 && reg <= 0x2F) return regs[reg]; // wave RAM
    if (reg > 0x16) return 0xFF;
    uint8_t v = regs[reg] | mask[reg];
    if (reg == 0x16) { // NR52
        v = (enabled ? 0x80 : 0) | 0x70 |
            (ch1On ? 1 : 0) | (ch2On ? 2 : 0) | (ch3On ? 4 : 0) | (ch4On ? 8 : 0);
    }
    return v;
}

void APU::trigger(int ch) {
    switch (ch) {
    case 1: {
        ch1On = (regs[0x02] & 0xF8) != 0;  // DAC on?
        if (ch1Len == 0) ch1Len = 64 - (regs[0x01] & 0x3F);
        ch1Freq = regs[0x03] | ((regs[0x04] & 7) << 8);
        ch1Timer = (2048 - ch1Freq) * 4;
        ch1Vol = regs[0x02] >> 4;
        ch1EnvTimer = regs[0x02] & 7;
        // sweep
        int period = (regs[0x00] >> 4) & 7;
        int shift = regs[0x00] & 7;
        ch1ShadowFreq = ch1Freq;
        ch1SweepTimer = period ? period : 8;
        ch1SweepOn = period != 0 || shift != 0;
        ch1SweepNegUsed = false;
        if (shift) { // overflow check on trigger
            int nf = ch1ShadowFreq >> shift;
            nf = (regs[0x00] & 8) ? ch1ShadowFreq - nf : ch1ShadowFreq + nf;
            if (nf > 2047) ch1On = false;
        }
        break; }
    case 2:
        ch2On = (regs[0x07] & 0xF8) != 0;
        if (ch2Len == 0) ch2Len = 64 - (regs[0x06] & 0x3F);
        ch2Timer = (2048 - (regs[0x08] | ((regs[0x09] & 7) << 8))) * 4;
        ch2Vol = regs[0x07] >> 4;
        ch2EnvTimer = regs[0x07] & 7;
        break;
    case 3:
        ch3On = (regs[0x0A] & 0x80) != 0;
        if (ch3Len == 0) ch3Len = 256 - regs[0x0B];
        ch3Timer = (2048 - (regs[0x0D] | ((regs[0x0E] & 7) << 8))) * 2;
        ch3Pos = 0;
        break;
    case 4:
        ch4On = (regs[0x11] & 0xF8) != 0;
        if (ch4Len == 0) ch4Len = 64 - (regs[0x10] & 0x3F);
        ch4Vol = regs[0x11] >> 4;
        ch4EnvTimer = regs[0x11] & 7;
        ch4Lfsr = 0x7FFF;
        break;
    }
}

void APU::write(uint8_t reg, uint8_t v) {
    if (reg >= 0x20 && reg <= 0x2F) { regs[reg] = v; return; } // wave RAM
    if (reg > 0x16) return;
    if (!enabled && reg != 0x16) {
        // while off, only NR52 and length counters (DMG quirk skipped) are writable
        return;
    }
    switch (reg) {
    case 0x01: regs[reg] = v; ch1Len = 64 - (v & 0x3F); return;
    case 0x02: regs[reg] = v; if (!(v & 0xF8)) ch1On = false; return;
    case 0x04: regs[reg] = v; if (v & 0x80) trigger(1); return;
    case 0x06: regs[reg] = v; ch2Len = 64 - (v & 0x3F); return;
    case 0x07: regs[reg] = v; if (!(v & 0xF8)) ch2On = false; return;
    case 0x09: regs[reg] = v; if (v & 0x80) trigger(2); return;
    case 0x0A: regs[reg] = v; if (!(v & 0x80)) ch3On = false; return;
    case 0x0B: regs[reg] = v; ch3Len = 256 - v; return;
    case 0x0E: regs[reg] = v; if (v & 0x80) trigger(3); return;
    case 0x10: regs[reg] = v; ch4Len = 64 - (v & 0x3F); return;
    case 0x11: regs[reg] = v; if (!(v & 0xF8)) ch4On = false; return;
    case 0x13: regs[reg] = v; if (v & 0x80) trigger(4); return;
    case 0x16: { // NR52: master enable
        bool on = (v & 0x80) != 0;
        if (!on && enabled) {
            // power off clears all registers
            for (int i = 0; i <= 0x15; i++) regs[i] = 0;
            ch1On = ch2On = ch3On = ch4On = false;
        }
        if (on && !enabled) { fsStep = 0; }
        enabled = on;
        return; }
    default:
        regs[reg] = v;
    }
}

void APU::stepFrameSequencer() {
    // steps 0,2,4,6: length; 2,6: sweep; 7: envelope
    if ((fsStep & 1) == 0) {
        const auto tickLength = [](bool enabled, int& length, bool& channelOn) {
            const bool shouldTick = enabled && length > 0;
            if (!shouldTick) return;
            length--;
            if (length == 0) channelOn = false;
        };
        tickLength(regs[0x04] & 0x40, ch1Len, ch1On);
        tickLength(regs[0x09] & 0x40, ch2Len, ch2On);
        tickLength(regs[0x0E] & 0x40, ch3Len, ch3On);
        tickLength(regs[0x13] & 0x40, ch4Len, ch4On);
    }
    if (fsStep == 2 || fsStep == 6) { // sweep
        if (ch1SweepOn && ch1On) {
            if (--ch1SweepTimer <= 0) {
                int period = (regs[0x00] >> 4) & 7;
                ch1SweepTimer = period ? period : 8;
                if (period) {
                    int shift = regs[0x00] & 7;
                    int nf = ch1ShadowFreq >> shift;
                    nf = (regs[0x00] & 8) ? ch1ShadowFreq - nf : ch1ShadowFreq + nf;
                    if (nf > 2047) ch1On = false;
                    else if (shift) {
                        ch1ShadowFreq = nf;
                        ch1Freq = nf;
                        regs[0x03] = nf & 0xFF;
                        regs[0x04] = (regs[0x04] & 0xF8) | ((nf >> 8) & 7);
                        // second overflow check
                        int nf2 = ch1ShadowFreq >> shift;
                        nf2 = (regs[0x00] & 8) ? ch1ShadowFreq - nf2 : ch1ShadowFreq + nf2;
                        if (nf2 > 2047) ch1On = false;
                    }
                }
            }
        }
    }
    if (fsStep == 7) { // envelope
        auto env = [](uint8_t nrx2, int& vol, int& timer) {
            int period = nrx2 & 7;
            if (!period) return;
            if (--timer <= 0) {
                timer = period;
                if (nrx2 & 8) { if (vol < 15) vol++; }
                else { if (vol > 0) vol--; }
            }
        };
        if (ch1On) env(regs[0x02], ch1Vol, ch1EnvTimer);
        if (ch2On) env(regs[0x07], ch2Vol, ch2EnvTimer);
        if (ch4On) env(regs[0x11], ch4Vol, ch4EnvTimer);
    }
    fsStep = (fsStep + 1) & 7;
}

void APU::mixSample() {
    if (sampleCount >= MAX_SAMPLES) return;
    float out[4] = {0, 0, 0, 0};
    if (enabled) {
        if (ch1On) out[0] = (DUTY[regs[0x01] >> 6][ch1Pos] ? 1.f : -1.f) * ch1Vol / 15.f;
        if (ch2On) out[1] = (DUTY[regs[0x06] >> 6][ch2Pos] ? 1.f : -1.f) * ch2Vol / 15.f;
        if (ch3On) {
            uint8_t s = regs[0x20 + (ch3Pos >> 1)];
            s = (ch3Pos & 1) ? (s & 0xF) : (s >> 4);
            int shift = (regs[0x0C] >> 5) & 3;   // 0=mute 1=100% 2=50% 3=25%
            if (shift == 0) s = 0; else s >>= (shift - 1);
            out[2] = (s / 7.5f) - 1.f;
        }
        if (ch4On) out[3] = ((ch4Lfsr & 1) ? -1.f : 1.f) * ch4Vol / 15.f;
    }
    uint8_t nr51 = regs[0x15];
    uint8_t nr50 = regs[0x14];
    float L = 0, R = 0;
    for (int i = 0; i < 4; i++) {
        if (nr51 & (0x10 << i)) L += out[i];
        if (nr51 & (0x01 << i)) R += out[i];
    }
    L *= (((nr50 >> 4) & 7) + 1) / 8.f;
    R *= ((nr50 & 7) + 1) / 8.f;
    L *= 0.22f;
    R *= 0.22f;
    // Chromatic FM expansion: YM2151 + ADPCM mixed in, GB APU gated by FF2B
    ChromaticFM& fm = gb->fm;
    if (fm.enabled) {
        fm.generateSample(sampleRate);
        if (!(fm.audioControl & 0x02)) { L = 0; R = 0; }   // GB APU disabled
        if (fm.audioControl & 0x01) { L += fm.ymL; R += fm.ymR; }
        if (fm.audioControl & 0x04) { L += fm.adpcmOut * 0.5f; R += fm.adpcmOut * 0.5f; }
    }
    sampleBuf[sampleCount * 2] = L;
    sampleBuf[sampleCount * 2 + 1] = R;
    sampleCount++;
}

void APU::tick(int cycles) {
    while (cycles-- > 0) {
        // channel timers
        if (ch1On && --ch1Timer <= 0) {
            ch1Timer = (2048 - (regs[0x03] | ((regs[0x04] & 7) << 8))) * 4;
            ch1Pos = (ch1Pos + 1) & 7;
        }
        if (ch2On && --ch2Timer <= 0) {
            ch2Timer = (2048 - (regs[0x08] | ((regs[0x09] & 7) << 8))) * 4;
            ch2Pos = (ch2Pos + 1) & 7;
        }
        if (ch3On && --ch3Timer <= 0) {
            ch3Timer = (2048 - (regs[0x0D] | ((regs[0x0E] & 7) << 8))) * 2;
            ch3Pos = (ch3Pos + 1) & 31;
        }
        if (ch4On && --ch4Timer <= 0) {
            static const int DIV[8] = {8, 16, 32, 48, 64, 80, 96, 112};
            ch4Timer = DIV[regs[0x12] & 7] << (regs[0x12] >> 4);
            int bit = (ch4Lfsr ^ (ch4Lfsr >> 1)) & 1;
            ch4Lfsr = (ch4Lfsr >> 1) | (bit << 14);
            if (regs[0x12] & 8) ch4Lfsr = (ch4Lfsr & ~0x40) | (bit << 6);
        }
        // frame sequencer @ 512 Hz
        if (++fsClock >= 8192) { fsClock = 0; stepFrameSequencer(); }
        // output sampling
        sampleAccum += sampleRate;
        if (sampleAccum >= 4194304.0) {
            sampleAccum -= 4194304.0;
            mixSample();
        }
    }
}

} // namespace gb
