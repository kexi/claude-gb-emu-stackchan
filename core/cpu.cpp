// SM83 (LR35902) CPU core
#include "gb.h"

namespace gb {

enum { FZ = 0x80, FN = 0x40, FH = 0x20, FC = 0x10 };

void CPU::reset(bool cgb) {
    a = cgb ? 0x11 : 0x01;
    f = 0xB0; b = 0x00; c = 0x13; d = 0x00; e = 0xD8; h = 0x01; l = 0x4D;
    pc = 0x0100; sp = 0xFFFE;
    ime = false; imeDelay = 0; halted = false; haltBug = false;
}

// base T-cycle counts for unprefixed opcodes (conditional extras added at runtime)
static const uint8_t CYC[256] = {
     4,12, 8, 8, 4, 4, 8, 4,20, 8, 8, 8, 4, 4, 8, 4,
     4,12, 8, 8, 4, 4, 8, 4,12, 8, 8, 8, 4, 4, 8, 4,
     8,12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
     8,12, 8, 8,12,12,12, 4, 8, 8, 8, 8, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
     8,12,12,16,12,16, 8,16, 8,16,12, 4,12,24, 8,16,
     8,12,12, 4,12,16, 8,16, 8,16,12, 4,12, 4, 8,16,
    12,12, 8, 4, 4,16, 8,16,16, 4,16, 4, 4, 4, 8,16,
    12,12, 8, 4, 4,16, 8,16,12, 8,16, 4, 4, 4, 8,16,
};

int CPU::handleInterrupts() {
    uint8_t pending = gb->ifReg & gb->ieReg & 0x1F;
    if (!pending) return 0;
    halted = false;
    if (!ime) return 0;
    ime = false;
    for (int i = 0; i < 5; i++) {
        if (pending & (1 << i)) {
            gb->ifReg &= ~(1 << i);
            sp -= 2;
            gb->write(sp, pc & 0xFF);
            gb->write(sp + 1, pc >> 8);
            pc = 0x40 + i * 8;
            return 20;
        }
    }
    return 0;
}

int CPU::step() {
    const bool hasPendingImeEnable = imeDelay > 0;
    if (hasPendingImeEnable) {
        imeDelay--;
        if (imeDelay == 0) ime = true;
    }

    int ic = handleInterrupts();
    if (ic) return ic;
    if (halted) return 4;

    GB& g = *gb;
    uint8_t op = g.read(pc);
    if (haltBug) haltBug = false; else pc++;

    int cycles = CYC[op];

    // helpers -----------------------------------------------------------
    auto rd = [&](uint16_t addr) { return g.read(addr); };
    auto wr = [&](uint16_t addr, uint8_t v) { g.write(addr, v); };
    auto imm8 = [&]() { return g.read(pc++); };
    auto imm16 = [&]() { uint16_t v = g.read(pc) | (g.read(pc + 1) << 8); pc += 2; return v; };
    auto hl = [&]() { return (uint16_t)((h << 8) | l); };
    auto getR = [&](int i) -> uint8_t {
        switch (i) { case 0: return b; case 1: return c; case 2: return d; case 3: return e;
                     case 4: return h; case 5: return l; case 6: return rd(hl()); default: return a; }
    };
    auto setR = [&](int i, uint8_t v) {
        switch (i) { case 0: b = v; break; case 1: c = v; break; case 2: d = v; break; case 3: e = v; break;
                     case 4: h = v; break; case 5: l = v; break; case 6: wr(hl(), v); break; default: a = v; }
    };
    auto push16 = [&](uint16_t v) { sp -= 2; wr(sp, v & 0xFF); wr(sp + 1, v >> 8); };
    auto pop16 = [&]() { uint16_t v = rd(sp) | (rd(sp + 1) << 8); sp += 2; return v; };
    auto setBC = [&](uint16_t v) { b = v >> 8; c = v & 0xFF; };
    auto setDE = [&](uint16_t v) { d = v >> 8; e = v & 0xFF; };
    auto setHL = [&](uint16_t v) { h = v >> 8; l = v & 0xFF; };
    auto bc = [&]() { return (uint16_t)((b << 8) | c); };
    auto de = [&]() { return (uint16_t)((d << 8) | e); };
    auto cond = [&](int cc) -> bool {
        switch (cc) { case 0: return !(f & FZ); case 1: return (f & FZ) != 0;
                      case 2: return !(f & FC); default: return (f & FC) != 0; }
    };
    auto alu = [&](int aluOp, uint8_t v) {
        switch (aluOp) {
        case 0: { // ADD
            int r = a + v;
            f = ((r & 0xFF) == 0 ? FZ : 0) | (((a & 0xF) + (v & 0xF)) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
            a = r; break; }
        case 1: { // ADC
            int cy = (f & FC) ? 1 : 0;
            int r = a + v + cy;
            f = ((r & 0xFF) == 0 ? FZ : 0) | (((a & 0xF) + (v & 0xF) + cy) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
            a = r; break; }
        case 2: { // SUB
            int r = a - v;
            f = FN | ((r & 0xFF) == 0 ? FZ : 0) | (((a & 0xF) - (v & 0xF)) < 0 ? FH : 0) | (r < 0 ? FC : 0);
            a = r; break; }
        case 3: { // SBC
            int cy = (f & FC) ? 1 : 0;
            int r = a - v - cy;
            f = FN | ((r & 0xFF) == 0 ? FZ : 0) | (((a & 0xF) - (v & 0xF) - cy) < 0 ? FH : 0) | (r < 0 ? FC : 0);
            a = r; break; }
        case 4: // AND
            a &= v; f = (a == 0 ? FZ : 0) | FH; break;
        case 5: // XOR
            a ^= v; f = (a == 0 ? FZ : 0); break;
        case 6: // OR
            a |= v; f = (a == 0 ? FZ : 0); break;
        default: { // CP
            int r = a - v;
            f = FN | ((r & 0xFF) == 0 ? FZ : 0) | (((a & 0xF) - (v & 0xF)) < 0 ? FH : 0) | (r < 0 ? FC : 0);
            break; }
        }
    };
    auto inc8 = [&](uint8_t v) -> uint8_t {
        uint8_t r = v + 1;
        f = (f & FC) | (r == 0 ? FZ : 0) | ((v & 0xF) == 0xF ? FH : 0);
        return r;
    };
    auto dec8 = [&](uint8_t v) -> uint8_t {
        uint8_t r = v - 1;
        f = (f & FC) | FN | (r == 0 ? FZ : 0) | ((v & 0xF) == 0 ? FH : 0);
        return r;
    };
    auto addHL = [&](uint16_t v) {
        uint32_t r = hl() + v;
        f = (f & FZ) | (((hl() & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0);
        setHL(r);
    };

    // LD r,r' block (0x40-0x7F) ------------------------------------------
    if (op >= 0x40 && op <= 0x7F) {
        if (op == 0x76) { // HALT
            uint8_t pending = g.ifReg & g.ieReg & 0x1F;
            if (!ime && pending) haltBug = true;
            else halted = true;
            return cycles;
        }
        setR((op >> 3) & 7, getR(op & 7));
        return cycles;
    }
    // ALU A,r block (0x80-0xBF) ------------------------------------------
    if (op >= 0x80 && op <= 0xBF) {
        alu((op >> 3) & 7, getR(op & 7));
        return cycles;
    }

    switch (op) {
    case 0x00: break; // NOP
    case 0x01: setBC(imm16()); break;
    case 0x02: wr(bc(), a); break;
    case 0x03: setBC(bc() + 1); break;
    case 0x04: b = inc8(b); break;
    case 0x05: b = dec8(b); break;
    case 0x06: b = imm8(); break;
    case 0x07: { // RLCA
        f = (a & 0x80) ? FC : 0; a = (a << 1) | (a >> 7); break; }
    case 0x08: { uint16_t addr = imm16(); wr(addr, sp & 0xFF); wr(addr + 1, sp >> 8); break; }
    case 0x09: addHL(bc()); break;
    case 0x0A: a = rd(bc()); break;
    case 0x0B: setBC(bc() - 1); break;
    case 0x0C: c = inc8(c); break;
    case 0x0D: c = dec8(c); break;
    case 0x0E: c = imm8(); break;
    case 0x0F: { // RRCA
        f = (a & 1) ? FC : 0; a = (a >> 1) | (a << 7); break; }
    case 0x10: { // STOP — used for CGB speed switch
        imm8();
        if (g.cgb && (g.key1 & 0x01)) {
            g.doubleSpeed = !g.doubleSpeed;
            g.key1 = (g.doubleSpeed ? 0x80 : 0x00);
        }
        break; }
    case 0x11: setDE(imm16()); break;
    case 0x12: wr(de(), a); break;
    case 0x13: setDE(de() + 1); break;
    case 0x14: d = inc8(d); break;
    case 0x15: d = dec8(d); break;
    case 0x16: d = imm8(); break;
    case 0x17: { // RLA
        uint8_t cy = (f & FC) ? 1 : 0;
        f = (a & 0x80) ? FC : 0; a = (a << 1) | cy; break; }
    case 0x18: { int8_t off = (int8_t)imm8(); pc += off; break; }
    case 0x19: addHL(de()); break;
    case 0x1A: a = rd(de()); break;
    case 0x1B: setDE(de() - 1); break;
    case 0x1C: e = inc8(e); break;
    case 0x1D: e = dec8(e); break;
    case 0x1E: e = imm8(); break;
    case 0x1F: { // RRA
        uint8_t cy = (f & FC) ? 0x80 : 0;
        f = (a & 1) ? FC : 0; a = (a >> 1) | cy; break; }
    case 0x20: case 0x28: case 0x30: case 0x38: { // JR cc
        int8_t off = (int8_t)imm8();
        if (cond((op >> 3) & 3)) { pc += off; cycles += 4; }
        break; }
    case 0x21: setHL(imm16()); break;
    case 0x22: wr(hl(), a); setHL(hl() + 1); break;
    case 0x23: setHL(hl() + 1); break;
    case 0x24: h = inc8(h); break;
    case 0x25: h = dec8(h); break;
    case 0x26: h = imm8(); break;
    case 0x27: { // DAA
        int adj = 0;
        if (f & FN) {
            if (f & FH) adj |= 0x06;
            if (f & FC) adj |= 0x60;
            a -= adj;
        } else {
            if ((f & FH) || (a & 0xF) > 9) adj |= 0x06;
            if ((f & FC) || a > 0x99) adj |= 0x60;
            a += adj;
        }
        f = (f & (FN | FC)) | (a == 0 ? FZ : 0) | ((adj & 0x60) ? FC : 0);
        break; }
    case 0x29: addHL(hl()); break;
    case 0x2A: a = rd(hl()); setHL(hl() + 1); break;
    case 0x2B: setHL(hl() - 1); break;
    case 0x2C: l = inc8(l); break;
    case 0x2D: l = dec8(l); break;
    case 0x2E: l = imm8(); break;
    case 0x2F: a = ~a; f |= FN | FH; break; // CPL
    case 0x31: sp = imm16(); break;
    case 0x32: wr(hl(), a); setHL(hl() - 1); break;
    case 0x33: sp++; break;
    case 0x34: wr(hl(), inc8(rd(hl()))); break;
    case 0x35: wr(hl(), dec8(rd(hl()))); break;
    case 0x36: wr(hl(), imm8()); break;
    case 0x37: f = (f & FZ) | FC; break; // SCF
    case 0x39: addHL(sp); break;
    case 0x3A: a = rd(hl()); setHL(hl() - 1); break;
    case 0x3B: sp--; break;
    case 0x3C: a = inc8(a); break;
    case 0x3D: a = dec8(a); break;
    case 0x3E: a = imm8(); break;
    case 0x3F: f = (f & FZ) | ((f & FC) ? 0 : FC); break; // CCF

    case 0xC0: case 0xC8: case 0xD0: case 0xD8: // RET cc
        if (cond((op >> 3) & 3)) { pc = pop16(); cycles += 12; }
        break;
    case 0xC1: setBC(pop16()); break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: { // JP cc
        uint16_t addr = imm16();
        if (cond((op >> 3) & 3)) { pc = addr; cycles += 4; }
        break; }
    case 0xC3: pc = imm16(); break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: { // CALL cc
        uint16_t addr = imm16();
        if (cond((op >> 3) & 3)) { push16(pc); pc = addr; cycles += 12; }
        break; }
    case 0xC5: push16(bc()); break;
    case 0xC6: alu(0, imm8()); break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF: // RST
        push16(pc); pc = op & 0x38; break;
    case 0xC9: pc = pop16(); break;
    case 0xCB: { // CB prefix
        uint8_t cb = imm8();
        int r = cb & 7;
        int bit = (cb >> 3) & 7;
        cycles = (r == 6) ? ((cb & 0xC0) == 0x40 ? 12 : 16) : 8;
        uint8_t v = getR(r);
        switch (cb >> 6) {
        case 0:
            switch (bit) {
            case 0: f = (v & 0x80) ? FC : 0; v = (v << 1) | (v >> 7); break;          // RLC
            case 1: f = (v & 1) ? FC : 0; v = (v >> 1) | (v << 7); break;             // RRC
            case 2: { uint8_t cy = (f & FC) ? 1 : 0; f = (v & 0x80) ? FC : 0; v = (v << 1) | cy; break; } // RL
            case 3: { uint8_t cy = (f & FC) ? 0x80 : 0; f = (v & 1) ? FC : 0; v = (v >> 1) | cy; break; } // RR
            case 4: f = (v & 0x80) ? FC : 0; v <<= 1; break;                          // SLA
            case 5: f = (v & 1) ? FC : 0; v = (v >> 1) | (v & 0x80); break;           // SRA
            case 6: f = 0; v = (v >> 4) | (v << 4); break;                            // SWAP
            case 7: f = (v & 1) ? FC : 0; v >>= 1; break;                             // SRL
            }
            f |= (v == 0 ? FZ : 0);
            setR(r, v);
            break;
        case 1: // BIT
            f = (f & FC) | FH | ((v & (1 << bit)) ? 0 : FZ);
            break;
        case 2: setR(r, v & ~(1 << bit)); break; // RES
        case 3: setR(r, v | (1 << bit)); break;  // SET
        }
        break; }
    case 0xCD: { uint16_t addr = imm16(); push16(pc); pc = addr; break; }
    case 0xCE: alu(1, imm8()); break;
    case 0xD1: setDE(pop16()); break;
    case 0xD5: push16(de()); break;
    case 0xD6: alu(2, imm8()); break;
    case 0xD9: pc = pop16(); ime = true; break; // RETI
    case 0xDE: alu(3, imm8()); break;
    case 0xE0: wr(0xFF00 + imm8(), a); break;
    case 0xE1: setHL(pop16()); break;
    case 0xE2: wr(0xFF00 + c, a); break;
    case 0xE5: push16(hl()); break;
    case 0xE6: alu(4, imm8()); break;
    case 0xE8: { // ADD SP,e8
        int8_t off = (int8_t)imm8();
        int r = sp + off;
        f = (((sp & 0xF) + (off & 0xF)) > 0xF ? FH : 0) | (((sp & 0xFF) + (off & 0xFF)) > 0xFF ? FC : 0);
        sp = r; break; }
    case 0xE9: pc = hl(); break;
    case 0xEA: wr(imm16(), a); break;
    case 0xEE: alu(5, imm8()); break;
    case 0xF0: a = rd(0xFF00 + imm8()); break;
    case 0xF1: { uint16_t v = pop16(); a = v >> 8; f = v & 0xF0; break; }
    case 0xF2: a = rd(0xFF00 + c); break;
    case 0xF3: ime = false; imeDelay = 0; break; // DI
    case 0xF5: push16((a << 8) | f); break;
    case 0xF6: alu(6, imm8()); break;
    case 0xF8: { // LD HL,SP+e8
        int8_t off = (int8_t)imm8();
        int r = sp + off;
        f = (((sp & 0xF) + (off & 0xF)) > 0xF ? FH : 0) | (((sp & 0xFF) + (off & 0xFF)) > 0xFF ? FC : 0);
        setHL(r); break; }
    case 0xF9: sp = hl(); break;
    case 0xFA: a = rd(imm16()); break;
    case 0xFB: if (!ime && imeDelay == 0) imeDelay = 2; break; // EI
    case 0xFE: alu(7, imm8()); break;
    default: break; // illegal opcodes: treat as NOP
    }
    return cycles;
}

} // namespace gb
