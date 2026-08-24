// PPU — scanline renderer, DMG + CGB modes
#include "gb.h"

namespace gb {

// DMG shades (slightly warm off-white to dark, easy on the eyes)
static const uint32_t DMG_SHADES_RGBA[4] = {
    0xFFD0F8E0, 0xFF70C088, 0xFF566834, 0xFF201808, // ABGR (little-endian RGBA)
};

static Pixel toPixel(uint32_t rgba) {
#if defined(GB_EMBEDDED) && defined(ESP_PLATFORM)
    const uint8_t r = rgba & 0xFF;
    const uint8_t g = (rgba >> 8) & 0xFF;
    const uint8_t b = (rgba >> 16) & 0xFF;
    const uint16_t rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return (Pixel)__builtin_bswap16(rgb565);
#else
    return rgba;
#endif
}

static Pixel dmgShades[4];

void PPU::reset(bool cgbMode) {
    (void)cgbMode;
    lcdc = 0x91; stat = 0x85; scy = scx = 0; ly = 0; lyc = 0;
    bgp = 0xFC; obp0 = obp1 = 0xFF; wy = wx = 0;
    bcps = ocps = 0;
    dot = 0; windowLine = 0; frameDone = false; statLine = false;
    // CGB boot leaves palette RAM white-ish; fill with white so DMG-only ROMs
    // that never touch BCPD still show something
    memset(bgPal, 0xFF, sizeof(bgPal));
    memset(objPal, 0xFF, sizeof(objPal));
    memset(framebuffer, 0xFF, sizeof(framebuffer));
    for (int i = 0; i < 4; i++) dmgShades[i] = toPixel(DMG_SHADES_RGBA[i]);
}

Pixel PPU::cgbColor(const uint8_t* pal, int palIdx, int colorIdx) const {
    int off = palIdx * 8 + colorIdx * 2;
    uint16_t c = pal[off] | (pal[off + 1] << 8);
    int r = c & 0x1F, gr = (c >> 5) & 0x1F, bl = (c >> 10) & 0x1F;
    // mild color correction: blend channels slightly like the CGB LCD
    int R = (r * 26 + gr * 4 + bl * 2) / 32;
    int G = (gr * 24 + bl * 8) / 32;
    int B = (r * 6 + gr * 4 + bl * 22) / 32;
    R = R * 255 / 31; G = G * 255 / 31; B = B * 255 / 31;
    return toPixel(0xFF000000 | (B << 16) | (G << 8) | R);
}

void PPU::checkStatIrq() {
    bool line = false;
    int mode = stat & 3;
    if ((stat & 0x40) && ly == lyc) line = true;
    if ((stat & 0x20) && mode == 2) line = true;
    if ((stat & 0x10) && mode == 1) line = true;
    if ((stat & 0x08) && mode == 0) line = true;
    if (line && !statLine) gb->requestInterrupt(INT_STAT);
    statLine = line;
}

void PPU::tick(int dots) {
    if (!(lcdc & 0x80)) { // LCD off
        ly = 0; dot = 0; stat = (stat & ~3);
        return;
    }
    while (dots > 0) {
#ifdef GB_EMBEDDED
        // CPU-visible writes can only happen between GB::tick calls. Jumping to
        // the next mode/line boundary therefore preserves every observable PPU
        // transition while avoiding one loop iteration per 4.19 MHz dot.
        int nextEvent;
        if (dot == 0) nextEvent = 1;
        else if (dot < 81) nextEvent = 81;
        else if (dot < 253) nextEvent = 253;
        else nextEvent = 456;
        int advance = nextEvent - dot;
        if (advance > dots) advance = dots;
        dot += advance;
        dots -= advance;
#else
        dot++;
        dots--;
#endif
        int oldMode = stat & 3;
        int mode;
        if (ly >= 144) mode = 1;
        else if (dot <= 80) mode = 2;
        else if (dot <= 80 + 172) mode = 3;
        else mode = 0;

        if (mode != oldMode) {
            stat = (stat & ~3) | mode;
            if (mode == 0) { // entering hblank
                renderScanline();
                if (gb->hdmaActive) gb->doHdmaBlock();
            }
            if (mode == 1) {
                gb->requestInterrupt(INT_VBLANK);
                frameDone = true;
                frameCount++;
            }
            checkStatIrq();
        }

        if (dot >= 456) {
            dot = 0;
            ly++;
            if (ly > 153) { ly = 0; windowLine = 0; }
            stat = (stat & ~4) | (ly == lyc ? 4 : 0);
            checkStatIrq();
        }
    }
}

void PPU::renderScanline() {
    if (ly >= 144) return;
    GB& g = *gb;
    Pixel* row = framebuffer + ly * 160;
    bool cgbMode = g.cgb;

    uint8_t bgColorIdx[160];   // 0-3 color index of BG/window pixel
    uint8_t bgPriority[160];   // CGB: BG attribute bit7 per pixel
    memset(bgColorIdx, 0, sizeof(bgColorIdx));
    memset(bgPriority, 0, sizeof(bgPriority));

    bool bgEnable = (lcdc & 0x01) != 0;    // DMG: BG off; CGB: sprites always on top when 0
    bool winEnable = (lcdc & 0x20) && wx <= 166 && wy <= ly;
    bool windowDrawn = false;

    // ---- background + window ----
    if (cgbMode || bgEnable) {
        for (int x = 0; x < 160; x++) {
            int tileX, tileY, px, py;
            uint16_t mapBase;
            bool inWindow = winEnable && x >= (wx - 7);
            if (inWindow) {
                mapBase = (lcdc & 0x40) ? 0x1C00 : 0x1800;
                int wxo = x - (wx - 7);
                tileX = wxo >> 3; px = wxo & 7;
                tileY = windowLine >> 3; py = windowLine & 7;
                windowDrawn = true;
            } else {
                mapBase = (lcdc & 0x08) ? 0x1C00 : 0x1800;
                int bx = (x + scx) & 0xFF, by = (ly + scy) & 0xFF;
                tileX = bx >> 3; px = bx & 7;
                tileY = by >> 3; py = by & 7;
            }
            uint16_t mapAddr = mapBase + tileY * 32 + tileX;
            uint8_t tileNum = g.vram[0][mapAddr];
            uint8_t attr = cgbMode ? g.vram[1][mapAddr] : 0;
            int bank = (attr >> 3) & 1;
            if (attr & 0x20) px = 7 - px;      // X flip
            if (attr & 0x40) py = 7 - py;      // Y flip
            uint16_t tileAddr;
            if (lcdc & 0x10) tileAddr = tileNum * 16;
            else tileAddr = 0x1000 + ((int8_t)tileNum) * 16;
            uint8_t lo = g.vram[bank][tileAddr + py * 2];
            uint8_t hi = g.vram[bank][tileAddr + py * 2 + 1];
            int ci = ((lo >> (7 - px)) & 1) | (((hi >> (7 - px)) & 1) << 1);
            bgColorIdx[x] = ci;
            bgPriority[x] = (attr >> 7) & 1;
            if (cgbMode) {
                row[x] = cgbColor(bgPal, attr & 7, ci);
            } else {
                row[x] = dmgShades[(bgp >> (ci * 2)) & 3];
            }
        }
    } else {
        for (int x = 0; x < 160; x++) row[x] = dmgShades[0];
    }
    if (windowDrawn) windowLine++;

    // ---- sprites ----
    if (!(lcdc & 0x02)) return;
    int spriteH = (lcdc & 0x04) ? 16 : 8;

    // collect up to 10 sprites on this line, in OAM order
    int idx[10], count = 0;
    for (int i = 0; i < 40 && count < 10; i++) {
        int sy = g.oam[i * 4] - 16;
        if (ly >= sy && ly < sy + spriteH) idx[count++] = i;
    }
    // DMG: lower X = higher priority; ties broken by OAM order.
    // CGB: OAM order only. Draw lowest-priority first so high priority overdraws.
    if (!cgbMode) {
        // stable sort by X (insertion sort, max 10 entries)
        for (int i = 1; i < count; i++) {
            int v = idx[i], vx = g.oam[v * 4 + 1];
            int j = i - 1;
            while (j >= 0 && g.oam[idx[j] * 4 + 1] > vx) { idx[j + 1] = idx[j]; j--; }
            idx[j + 1] = v;
        }
    }
    for (int k = count - 1; k >= 0; k--) {
        int i = idx[k];
        int sy = g.oam[i * 4] - 16;
        int sx = g.oam[i * 4 + 1] - 8;
        uint8_t tile = g.oam[i * 4 + 2];
        uint8_t attr = g.oam[i * 4 + 3];
        if (spriteH == 16) tile &= 0xFE;
        int py = ly - sy;
        if (attr & 0x40) py = spriteH - 1 - py;   // Y flip
        int bank = cgbMode ? ((attr >> 3) & 1) : 0;
        uint16_t tileAddr = tile * 16 + py * 2;
        uint8_t lo = g.vram[bank][tileAddr];
        uint8_t hi = g.vram[bank][tileAddr + 1];
        for (int x = 0; x < 8; x++) {
            int sxp = sx + x;
            if (sxp < 0 || sxp >= 160) continue;
            int px = (attr & 0x20) ? x : 7 - x;    // X flip
            int ci = ((lo >> px) & 1) | (((hi >> px) & 1) << 1);
            if (ci == 0) continue;                 // transparent
            // priority: LCDC bit0=0 in CGB → sprites always on top
            bool masterPriority = !cgbMode || bgEnable;
            if (masterPriority && bgColorIdx[sxp] != 0) {
                if ((attr & 0x80) || (cgbMode && bgPriority[sxp])) continue;
            }
            if (cgbMode) {
                row[sxp] = cgbColor(objPal, attr & 7, ci);
            } else {
                uint8_t pal = (attr & 0x10) ? obp1 : obp0;
                row[sxp] = dmgShades[(pal >> (ci * 2)) & 3];
            }
        }
    }
}

} // namespace gb
