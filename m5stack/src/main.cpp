// M5Stack CoreS3 frontend for the shared Game Boy / Game Boy Color core.
// Core 1 owns emulation, LCD DMA, audio submission, and SD. Core 0 only polls
// Grove input, keeping I2C latency out of the 59.7275 Hz frame loop.
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>

#include <algorithm>
#include <cstring>

#include "../../core/gb.h"
#include "config.h"
#include "grove_input.h"

extern const uint8_t builtinRomStart[] asm("_binary_data_kantan_gb_play_gbc_start");
extern const uint8_t builtinRomEnd[] asm("_binary_data_kantan_gb_play_gbc_end");

namespace {

struct DisplayDiagnostic {
    uint32_t magic;
    uint32_t m5Board;
    uint32_t displayBoard;
    uint32_t displayCount;
    uint32_t width;
    uint32_t height;
    uint32_t brightness;
    uint32_t pmic;
};

volatile DisplayDiagnostic displayDiagnostic = {0x47424449, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0, 0, 0, 0xFFFFFFFF};
static_assert(sizeof(DisplayDiagnostic) == 32, "JTAG display diagnostic layout must stay stable");

enum class RomSource : uint8_t { BuiltIn, Sd };

struct RomEntry {
    char name[ROM_NAME_MAX];
    uint32_t size;
    RomSource source;
};

enum class AppMode { Menu, Game };

gb::GB systemGb;
RomEntry romEntries[ROM_MAX_FILES];
int romCount = 0;
int menuCursor = 0;
int menuTop = 0;
uint8_t previousMenuButtons = 0;
AppMode appMode = AppMode::Menu;
char currentRomName[ROM_NAME_MAX] = {};
bool sdMounted = false;

bool displayDmaOutstanding = false;
alignas(4) uint16_t displayBandBuffers[2][DISPLAY_WIDTH * DISPLAY_BAND_ROWS];

int16_t audioRing[AUDIO_RING_SAMPLES];
int audioRead = 0;
int audioWrite = 0;
uint32_t audioDropped = 0;
uint32_t audioUnderruns = 0;
int16_t audioChunks[AUDIO_CHUNK_SLOTS][AUDIO_CHUNK_SAMPLES];
int audioChunkIndex = 0;
float dcPreviousInput = 0;
float dcPreviousOutput = 0;
float playbackRateEwma = AUDIO_SAMPLE_RATE;

uint32_t observedRamGeneration = 0;
uint32_t savedRamGeneration = 0;
uint32_t lastRamWriteMs = 0;
GroveJoystickKind loggedJoystickKind = GroveJoystickKind::None;
bool joystickStateLogged = false;

void logGroveInputState() {
    const GroveJoystickKind kind = groveInputJoystickKind();
    const bool unchanged = joystickStateLogged && kind == loggedJoystickKind;
    if (unchanged) return;
    const bool wasConnected = joystickStateLogged && loggedJoystickKind != GroveJoystickKind::None;
    const char* event = kind == GroveJoystickKind::None && wasConnected ? "joystick_disconnected"
                        : kind == GroveJoystickKind::None               ? "joystick_unavailable"
                                                                        : "joystick_connected";
    const char* model = kind == GroveJoystickKind::Joy2 ? "joy2" : kind == GroveJoystickKind::Joy1 ? "joy1" : "none";
    Serial.printf("{\"component\":\"input\",\"event\":\"%s\",\"time_ms\":%u,\"model\":\"%s\"}\n", event, millis(),
                  model);
    loggedJoystickKind = kind;
    joystickStateLogged = true;
}

bool endsWithIgnoreCase(const char* name, const char* suffix) {
    const size_t nameLength = strlen(name);
    const size_t suffixLength = strlen(suffix);
    if (nameLength < suffixLength) return false;
    const char* tail = name + nameLength - suffixLength;
    for (size_t i = 0; i < suffixLength; i++) {
        const char left = tail[i] >= 'A' && tail[i] <= 'Z' ? tail[i] + 32 : tail[i];
        const char right = suffix[i] >= 'A' && suffix[i] <= 'Z' ? suffix[i] + 32 : suffix[i];
        if (left != right) return false;
    }
    return true;
}

const char* baseName(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void joinDisplayDma() {
    if (!displayDmaOutstanding) return;
    M5.Display.waitDMA();
    displayDmaOutstanding = false;
}

void showFatal(const char* message) {
    joinDisplayDma();
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(8, 100);
    M5.Display.print(message);
    Serial.printf("{\"component\":\"boot\",\"event\":\"fatal\",\"message\":\"%s\"}\n", message);
    for (;;) delay(1000);
}

bool mountSd() {
    const int sck = M5.getPin(m5::pin_name_t::sd_spi_sclk);
    const int miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
    const int mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi);
    const int cs = M5.getPin(m5::pin_name_t::sd_spi_ss);
    const bool pinsKnown = sck >= 0 && miso >= 0 && mosi >= 0 && cs >= 0;
    if (!pinsKnown) return false;
    SPI.begin(sck, miso, mosi, cs);
    const bool mounted = SD.begin((uint8_t)cs, SPI, SD_SPI_HZ);
    if (!mounted) return false;
    if (!SD.exists(SD_ROMS_DIR)) SD.mkdir(SD_ROMS_DIR);
    return true;
}

int compareEntries(const void* left, const void* right) {
    const auto* a = static_cast<const RomEntry*>(left);
    const auto* b = static_cast<const RomEntry*>(right);
    return strcasecmp(a->name, b->name);
}

void scanRoms() {
    romCount = 1;
    strncpy(romEntries[0].name, "KANTAN GB [built-in]", ROM_NAME_MAX - 1);
    romEntries[0].name[ROM_NAME_MAX - 1] = '\0';
    romEntries[0].size = (uint32_t)(builtinRomEnd - builtinRomStart);
    romEntries[0].source = RomSource::BuiltIn;
    if (!sdMounted) return;

    File directory = SD.open(SD_ROMS_DIR);
    if (!directory || !directory.isDirectory()) return;
    for (File file = directory.openNextFile(); file && romCount < ROM_MAX_FILES; file = directory.openNextFile()) {
        const char* name = baseName(file.name());
        const bool isRom = !file.isDirectory() && (endsWithIgnoreCase(name, ".gb") || endsWithIgnoreCase(name, ".gbc"));
        const bool nameFits = strlen(name) < ROM_NAME_MAX;
        const bool sizeFits = file.size() >= 0x8000 && file.size() <= ROM_MAX_SIZE;
        if (isRom && nameFits && sizeFits) {
            strncpy(romEntries[romCount].name, name, ROM_NAME_MAX - 1);
            romEntries[romCount].name[ROM_NAME_MAX - 1] = '\0';
            romEntries[romCount].size = (uint32_t)file.size();
            romEntries[romCount].source = RomSource::Sd;
            romCount++;
        }
        file.close();
    }
    directory.close();
    if (romCount > 2) qsort(romEntries + 1, romCount - 1, sizeof(RomEntry), compareEntries);
    if (menuCursor >= romCount) menuCursor = std::max(0, romCount - 1);
}

void makeRomPath(const char* name, char* path, size_t capacity) {
    snprintf(path, capacity, "%s/%s", SD_ROMS_DIR, name);
}

void makeSavePath(const char* name, char* path, size_t capacity) {
    char stem[ROM_NAME_MAX];
    strncpy(stem, name, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    char* extension = strrchr(stem, '.');
    if (extension) *extension = '\0';
    snprintf(path, capacity, "%s/%s.sav", SD_ROMS_DIR, stem);
}

void recoverSaveFiles(const char* path) {
    char partPath[sizeof(SD_ROMS_DIR) + ROM_NAME_MAX + 14];
    char backupPath[sizeof(partPath)];
    snprintf(partPath, sizeof(partPath), "%s.part", path);
    snprintf(backupPath, sizeof(backupPath), "%s.bak", path);
    SD.remove(partPath);

    const bool saveExists = SD.exists(path);
    const bool backupExists = SD.exists(backupPath);
    if (saveExists && backupExists) {
        SD.remove(backupPath);
        return;
    }
    if (!saveExists && backupExists) {
        const bool recovered = SD.rename(backupPath, path);
        Serial.printf("{\"component\":\"save\",\"event\":\"recover\",\"ok\":%s}\n", recovered ? "true" : "false");
    }
}

void loadSram(const char* romName) {
    if (systemGb.cart.ram.empty()) return;
    char path[sizeof(SD_ROMS_DIR) + ROM_NAME_MAX + 8];
    makeSavePath(romName, path, sizeof(path));
    recoverSaveFiles(path);
    File save = SD.open(path, FILE_READ);
    if (!save) return;
    const bool sizeMatches = save.size() == systemGb.cart.ram.size();
    const size_t readBytes = sizeMatches ? save.read(systemGb.cart.ram.data(), systemGb.cart.ram.size()) : 0;
    save.close();
    if (readBytes == systemGb.cart.ram.size()) {
        systemGb.cart.ramGeneration = 0;
        Serial.printf("{\"component\":\"save\",\"event\":\"loaded\",\"bytes\":%u}\n", (unsigned)readBytes);
    }
}

bool saveSram() {
    const bool hasSave = systemGb.loaded && systemGb.cart.battery && !systemGb.cart.ram.empty() && currentRomName[0];
    if (!hasSave || systemGb.cart.ramGeneration == savedRamGeneration) return true;
    joinDisplayDma();

    char path[sizeof(SD_ROMS_DIR) + ROM_NAME_MAX + 8];
    char partPath[sizeof(path) + 6];
    char backupPath[sizeof(path) + 5];
    makeSavePath(currentRomName, path, sizeof(path));
    snprintf(partPath, sizeof(partPath), "%s.part", path);
    snprintf(backupPath, sizeof(backupPath), "%s.bak", path);
    recoverSaveFiles(path);
    SD.remove(partPath);
    File save = SD.open(partPath, FILE_WRITE);
    if (!save) return false;
    const size_t written = save.write(systemGb.cart.ram.data(), systemGb.cart.ram.size());
    save.flush();
    save.close();
    const bool complete = written == systemGb.cart.ram.size();
    if (!complete) {
        SD.remove(partPath);
        return false;
    }
    const bool hadPreviousSave = SD.exists(path);
    SD.remove(backupPath);
    const bool previousBackedUp = !hadPreviousSave || SD.rename(path, backupPath);
    if (!previousBackedUp) {
        SD.remove(partPath);
        return false;
    }
    const bool renamed = SD.rename(partPath, path);
    if (!renamed && hadPreviousSave) SD.rename(backupPath, path);
    if (renamed) SD.remove(backupPath);
    if (renamed) savedRamGeneration = systemGb.cart.ramGeneration;
    Serial.printf("{\"component\":\"save\",\"event\":\"write\",\"ok\":%s,\"bytes\":%u}\n", renamed ? "true" : "false",
                  (unsigned)written);
    return renamed;
}

bool loadRom(const RomEntry& entry) {
    joinDisplayDma();
    bool loaded = false;
    if (entry.source == RomSource::BuiltIn) {
        loaded = systemGb.loadRom(builtinRomStart, entry.size);
    } else {
        char path[sizeof(SD_ROMS_DIR) + ROM_NAME_MAX + 2];
        makeRomPath(entry.name, path, sizeof(path));
        File rom = SD.open(path, FILE_READ);
        if (!rom) return false;

        gb::ByteStorage romData;
        try {
            romData.resize(entry.size);
        } catch (const std::bad_alloc&) {
            rom.close();
            return false;
        }
        const size_t bytesRead = rom.read(romData.data(), entry.size);
        rom.close();
        loaded = bytesRead == entry.size && systemGb.loadRom(std::move(romData));
    }
    if (!loaded) return false;

    systemGb.apu.setSampleRate(AUDIO_SAMPLE_RATE);
    strncpy(currentRomName, entry.name, sizeof(currentRomName) - 1);
    currentRomName[sizeof(currentRomName) - 1] = '\0';
    if (entry.source == RomSource::Sd) loadSram(currentRomName);
    observedRamGeneration = systemGb.cart.ramGeneration;
    savedRamGeneration = systemGb.cart.ramGeneration;
    lastRamWriteMs = millis();
    Serial.printf("{\"component\":\"rom\",\"event\":\"loaded\",\"name\":\"%s\",\"bytes\":%u,\"cgb\":%s}\n",
                  currentRomName, (unsigned)entry.size, systemGb.cgb ? "true" : "false");
    return true;
}

void drawMenu() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(8, 7);
    M5.Display.print("GAME BOY / COLOR");

    if (romCount == 0) {
        M5.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
        M5.Display.setCursor(8, 68);
        M5.Display.print("No ROM found");
        M5.Display.setTextSize(1);
        M5.Display.setCursor(8, 96);
        M5.Display.print("Put .gb/.gbc files in /roms");
        M5.Display.setCursor(8, 112);
        M5.Display.print("on the CoreS3 microSD card.");
        M5.Display.setCursor(8, 208);
        M5.Display.print("BtnC: rescan");
        return;
    }

    const bool cursorAbove = menuCursor < menuTop;
    const bool cursorBelow = menuCursor >= menuTop + MENU_VISIBLE_ROWS;
    if (cursorAbove) menuTop = menuCursor;
    if (cursorBelow) menuTop = menuCursor - MENU_VISIBLE_ROWS + 1;
    for (int slot = 0; slot < MENU_VISIBLE_ROWS; slot++) {
        const int index = menuTop + slot;
        if (index >= romCount) break;
        const int y = MENU_TOP + slot * MENU_ROW_HEIGHT;
        const bool selected = index == menuCursor;
        M5.Display.fillRect(0, y, 320, MENU_ROW_HEIGHT, selected ? TFT_DARKGREEN : TFT_BLACK);
        M5.Display.setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY, selected ? TFT_DARKGREEN : TFT_BLACK);
        M5.Display.setTextSize(2);
        M5.Display.setCursor(8, y + 4);
        char label[25];
        snprintf(label, sizeof(label), "%.24s", romEntries[index].name);
        M5.Display.print(label);
    }
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(8, 222);
    M5.Display.print("UP/DOWN  A/START: run  BtnC: rescan");
}

int touchedMenuRow() {
    if (M5.Touch.getCount() == 0) return -1;
    const auto& touch = M5.Touch.getDetail();
    if (!touch.wasClicked()) return -1;
    const bool inside = touch.y >= MENU_TOP && touch.y < MENU_TOP + MENU_VISIBLE_ROWS * MENU_ROW_HEIGHT;
    if (!inside) return -1;
    const int index = menuTop + (touch.y - MENU_TOP) / MENU_ROW_HEIGHT;
    return index < romCount ? index : -1;
}

void resetAudio() {
    M5.Speaker.stop(SPEAKER_CHANNEL);
    audioRead = audioWrite = 0;
    dcPreviousInput = dcPreviousOutput = 0;
    for (int i = 0; i < 2; i++) {
        memset(audioChunks[audioChunkIndex], 0, sizeof(audioChunks[0]));
        M5.Speaker.playRaw(audioChunks[audioChunkIndex], AUDIO_CHUNK_SAMPLES, AUDIO_SAMPLE_RATE, false, 1,
                           SPEAKER_CHANNEL);
        audioChunkIndex = (audioChunkIndex + 1) % AUDIO_CHUNK_SLOTS;
    }
}

void startGame() {
    appMode = AppMode::Game;
    M5.Display.fillScreen(TFT_DARKGREY);
    M5.Display.drawRect(DISPLAY_X - 1, DISPLAY_Y - 1, DISPLAY_WIDTH + 2, DISPLAY_HEIGHT + 2, TFT_CYAN);
    resetAudio();
}

void enterMenu() {
    saveSram();
    joinDisplayDma();
    M5.Speaker.stop(SPEAKER_CHANNEL);
    appMode = AppMode::Menu;
    previousMenuButtons = 0;
    if (!sdMounted) sdMounted = mountSd();
    scanRoms();
    drawMenu();
}

void menuLoop() {
    M5.update();
    logGroveInputState();
    uint8_t buttons = groveInputBits();
    if (M5.BtnB.isPressed()) buttons |= gb::BTN_START;
    const uint8_t pressed = buttons & ~previousMenuButtons;
    previousMenuButtons = buttons;
    bool redraw = false;
    if ((pressed & gb::BTN_UP) && romCount > 0) {
        menuCursor = (menuCursor + romCount - 1) % romCount;
        redraw = true;
    }
    if ((pressed & gb::BTN_DOWN) && romCount > 0) {
        menuCursor = (menuCursor + 1) % romCount;
        redraw = true;
    }
    const int touched = touchedMenuRow();
    if (touched >= 0) {
        menuCursor = touched;
        redraw = true;
    }
    const bool launchRequested = touched >= 0 || (pressed & (gb::BTN_A | gb::BTN_START));
    if (launchRequested && romCount > 0) {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(8, 208);
        M5.Display.print("Loading...");
        if (loadRom(romEntries[menuCursor])) {
            startGame();
            return;
        }
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.setCursor(8, 208);
        M5.Display.print("ROM load failed");
    }
    if (M5.BtnC.wasClicked()) {
        if (!sdMounted) sdMounted = mountSd();
        scanRoms();
        redraw = true;
    }
    if (redraw) drawMenu();
    delay(16);
}

void applyInput() {
    uint8_t buttons = groveInputBits();
    if (M5.BtnA.isPressed()) buttons |= gb::BTN_SELECT;
    if (M5.BtnB.isPressed()) buttons |= gb::BTN_START;
    const uint8_t newlyPressed = buttons & ~systemGb.buttons;
    if (newlyPressed) systemGb.requestInterrupt(gb::INT_JOYPAD);
    systemGb.buttons = buttons;
}

void scaleDisplayBand(uint16_t* destination, int band) {
    const int firstOutputY = band * DISPLAY_BAND_ROWS;
    for (int bandY = 0; bandY < DISPLAY_BAND_ROWS; bandY++) {
        const int outputY = firstOutputY + bandY;
        const int sourceY = outputY * 2 / 3;
        const uint16_t* source = systemGb.ppu.framebuffer + sourceY * GB_WIDTH;
        uint16_t* output = destination + bandY * DISPLAY_WIDTH;
        for (int pair = 0; pair < GB_WIDTH / 2; pair++) {
            const uint16_t left = source[pair * 2];
            const uint16_t right = source[pair * 2 + 1];
            output[pair * 3] = left;
            output[pair * 3 + 1] = left;
            output[pair * 3 + 2] = right;
        }
    }
}

void pushDisplayFrame() {
    const bool displayFrame = systemGb.ppu.frameCount % DISPLAY_FRAME_DIVIDER == 0;
    if (!displayFrame) return;

    for (int band = 0; band < DISPLAY_BANDS; band++) {
        uint16_t* buffer = displayBandBuffers[band & 1];
        // The next band is scaled while SPI DMA still owns the other buffer.
        // Waiting here keeps each buffer alive until its transfer completes.
        scaleDisplayBand(buffer, band);
        joinDisplayDma();
        M5.Display.pushImageDMA(DISPLAY_X, DISPLAY_Y + band * DISPLAY_BAND_ROWS, DISPLAY_WIDTH, DISPLAY_BAND_ROWS,
                                buffer);
        displayDmaOutstanding = true;
    }
}

int audioAvailable() { return (audioWrite - audioRead) & AUDIO_RING_MASK; }

int enqueueAudio() {
    const int produced = systemGb.apu.sampleCount;
    float previousInput = dcPreviousInput;
    float previousOutput = dcPreviousOutput;
    int write = audioWrite;
    int read = audioRead;
    for (int i = 0; i < produced; i++) {
        const float input = (systemGb.apu.sampleBuf[i * 2] + systemGb.apu.sampleBuf[i * 2 + 1]) * 0.5f;
        const float filtered = input - previousInput + 0.9985f * previousOutput;
        previousInput = input;
        previousOutput = filtered;
        float sample = filtered * 0.9f;
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        audioRing[write] = (int16_t)(sample * 32767.0f);
        write = (write + 1) & AUDIO_RING_MASK;
        const bool ringFull = write == read;
        if (ringFull) {
            read = (read + 1) & AUDIO_RING_MASK;
            audioDropped++;
        }
    }
    dcPreviousInput = previousInput;
    dcPreviousOutput = previousOutput;
    audioWrite = write;
    audioRead = read;
    systemGb.apu.sampleCount = 0;
    return produced;
}

void drainAudio(uint32_t playbackRate) {
    while (audioAvailable() >= AUDIO_CHUNK_SAMPLES) {
        const bool speakerQueueFull = M5.Speaker.isPlaying(SPEAKER_CHANNEL) >= 2;
        if (speakerQueueFull) return;
        int16_t* chunk = audioChunks[audioChunkIndex];
        const int firstLength = std::min(AUDIO_CHUNK_SAMPLES, AUDIO_RING_SAMPLES - audioRead);
        memcpy(chunk, audioRing + audioRead, firstLength * sizeof(int16_t));
        const int secondLength = AUDIO_CHUNK_SAMPLES - firstLength;
        if (secondLength > 0) memcpy(chunk + firstLength, audioRing, secondLength * sizeof(int16_t));
        audioRead = (audioRead + AUDIO_CHUNK_SAMPLES) & AUDIO_RING_MASK;
        const bool queued = M5.Speaker.playRaw(chunk, AUDIO_CHUNK_SAMPLES, playbackRate, false, 1, SPEAKER_CHANNEL);
        if (!queued) {
            audioRead = (audioRead - AUDIO_CHUNK_SAMPLES) & AUDIO_RING_MASK;
            return;
        }
        audioChunkIndex = (audioChunkIndex + 1) % AUDIO_CHUNK_SLOTS;
    }
}

uint32_t updatePlaybackRate(int produced, int64_t wallFrameUs) {
    if (wallFrameUs > 0) {
        const float observed = produced * 1000000.0f / (float)wallFrameUs;
        playbackRateEwma += 0.03f * (observed - playbackRateEwma);
    }
    float rate = playbackRateEwma;
    const int ringError = audioAvailable() - 2048;
    rate += ringError * 0.02f;
    if (rate > AUDIO_SAMPLE_RATE) rate = AUDIO_SAMPLE_RATE;
    if (rate < 8000) rate = 8000;
    return (uint32_t)(rate + 0.5f);
}

void maybeSaveSram() {
    const uint32_t generation = systemGb.cart.ramGeneration;
    const bool changed = generation != observedRamGeneration;
    if (changed) {
        observedRamGeneration = generation;
        lastRamWriteMs = millis();
    }
    const bool pending = generation != savedRamGeneration;
    const bool quiet = millis() - lastRamWriteMs >= SRAM_QUIET_SAVE_MS;
    if (pending && quiet) saveSram();
}

void gameLoop() {
    static int64_t nextFrameUs = esp_timer_get_time();
    static int64_t previousStartUs = esp_timer_get_time();
    static uint32_t perfStartedMs = millis();
    static uint32_t perfFrames = 0;
    static uint64_t perfFrameUs = 0;
#ifdef GB_PROFILE
    static uint64_t perfCpuUs = 0;
    static uint64_t perfPpuUs = 0;
    static uint64_t perfApuUs = 0;
#endif

    const int64_t frameStartUs = esp_timer_get_time();
    const int64_t wallFrameUs = frameStartUs - previousStartUs;
    previousStartUs = frameStartUs;

    joinDisplayDma();
    M5.update();
    logGroveInputState();
    if (M5.BtnC.wasHold()) {
        enterMenu();
        return;
    }
    applyInput();

    const int64_t emulationStartUs = esp_timer_get_time();
    systemGb.runFrame();
    const int64_t emulationUs = esp_timer_get_time() - emulationStartUs;
    const int produced = enqueueAudio();
    const uint32_t playbackRate = updatePlaybackRate(produced, wallFrameUs);
    drainAudio(playbackRate);

    pushDisplayFrame();
    maybeSaveSram();

    perfFrames++;
    perfFrameUs += emulationUs;
#ifdef GB_PROFILE
    perfCpuUs += systemGb.profileCpuUs;
    perfPpuUs += systemGb.profilePpuUs;
    perfApuUs += systemGb.profileApuUs;
#endif
    const uint32_t nowMs = millis();
    if (nowMs - perfStartedMs >= PERF_LOG_INTERVAL_MS) {
        const float seconds = (nowMs - perfStartedMs) / 1000.0f;
        const float fps = perfFrames / seconds;
        const uint32_t averageUs = perfFrames ? (uint32_t)(perfFrameUs / perfFrames) : 0;
        const int ringSamples = audioAvailable();
        if (ringSamples == 0) audioUnderruns++;
        Serial.printf("{\"component\":\"runtime\",\"event\":\"perf\",\"frame\":%u,\"fps\":%.2f,"
                      "\"frame_us\":%u,\"audio_ring_samples\":%d,\"audio_underruns\":%u,"
                      "\"audio_dropped\":%u,\"playback_rate_hz\":%u,\"heap_free_bytes\":%u,"
                      "\"psram_free_bytes\":%u,\"display_mode\":\"scaled-3:2-dma-30fps\"}\n",
                      systemGb.ppu.frameCount, fps, averageUs, ringSamples, audioUnderruns, audioDropped, playbackRate,
                      ESP.getFreeHeap(), ESP.getFreePsram());
#ifdef GB_PROFILE
        Serial.printf("{\"component\":\"runtime\",\"event\":\"profile\",\"frame\":%u,"
                      "\"cpu_us\":%u,\"ppu_us\":%u,\"apu_us\":%u}\n",
                      systemGb.ppu.frameCount, (unsigned)(perfCpuUs / perfFrames), (unsigned)(perfPpuUs / perfFrames),
                      (unsigned)(perfApuUs / perfFrames));
        perfCpuUs = perfPpuUs = perfApuUs = 0;
#endif
        perfStartedMs = nowMs;
        perfFrames = 0;
        perfFrameUs = 0;
    }

    nextFrameUs += GB_FRAME_US;
    const int64_t remainingUs = nextFrameUs - esp_timer_get_time();
    if (remainingUs > 0) delayMicroseconds((uint32_t)remainingUs);
    const bool badlyBehind = esp_timer_get_time() - nextFrameUs > GB_FRAME_US * 3;
    if (badlyBehind) nextFrameUs = esp_timer_get_time();
}

}   // namespace

void setup() {
    auto config = M5.config();
    // This firmware is intentionally board-specific. The StackChan fallback
    // keeps its PMIC, amp, and IO-expander setup even if probing is inconclusive.
    config.fallback_board = m5::board_t::board_M5StackChan;
    config.internal_spk = true;
    config.internal_mic = false;
    M5.begin(config);
    Serial.begin(115200);

    displayDiagnostic.m5Board = (uint32_t)M5.getBoard();
    displayDiagnostic.displayBoard = (uint32_t)M5.Display.getBoard();
    displayDiagnostic.displayCount = (uint32_t)M5.getDisplayCount();
    displayDiagnostic.width = (uint32_t)M5.Display.width();
    displayDiagnostic.height = (uint32_t)M5.Display.height();
    displayDiagnostic.brightness = (uint32_t)M5.Display.getBrightness();
    displayDiagnostic.pmic = (uint32_t)M5.Power.getType();

    M5.Display.setRotation(1);
    M5.Display.setSwapBytes(false);
    if (auto* bus = M5.Display.getPanel()->getBus()) bus->setClock(DISPLAY_SPI_HZ);
    M5.Display.powerSaveOff();
    M5.Display.wakeup();
    M5.Display.setBrightness(255);
    displayDiagnostic.width = (uint32_t)M5.Display.width();
    displayDiagnostic.height = (uint32_t)M5.Display.height();
    displayDiagnostic.brightness = (uint32_t)M5.Display.getBrightness();
    M5.Display.fillScreen(TFT_NAVY);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(82, 86);
    M5.Display.print("GB BOOT");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(74, 126);
    M5.Display.print("Loading built-in ROM...");

    auto speakerConfig = M5.Speaker.config();
    speakerConfig.sample_rate = AUDIO_SAMPLE_RATE;
    speakerConfig.task_pinned_core = 0;
    speakerConfig.task_priority = 4;
    M5.Speaker.config(speakerConfig);
    const bool speakerStarted = M5.Speaker.begin();
    M5.Speaker.setVolume(SPEAKER_VOLUME);
    Serial.printf("{\"component\":\"audio\",\"event\":\"speaker_begin\",\"ok\":%s}\n",
                  speakerStarted ? "true" : "false");
    if (speakerStarted) {
        M5.Speaker.tone(880, 120, 0, true);
        delay(140);
        M5.Speaker.stop(0);
    }

    scanRoms();
    const bool builtinLoaded = loadRom(romEntries[0]);
    if (!builtinLoaded) showFatal("Built-in ROM failed");

    groveInputInit();
    logGroveInputState();
    Serial.printf("{\"component\":\"boot\",\"event\":\"ready\",\"target\":\"m5stack-cores3\","
                  "\"boot_rom\":\"built-in\",\"sd_deferred\":true,\"heap_free_bytes\":%u,"
                  "\"psram_free_bytes\":%u}\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
    startGame();
}

void loop() {
    if (appMode == AppMode::Menu) {
        menuLoop();
        return;
    }
    gameLoop();
}
