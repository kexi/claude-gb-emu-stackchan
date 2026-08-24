#pragma once

#include <cstddef>
#include <cstdint>

constexpr int GB_WIDTH = 160;
constexpr int GB_HEIGHT = 144;
constexpr int DISPLAY_WIDTH = 240;
constexpr int DISPLAY_HEIGHT = 216;
constexpr int DISPLAY_X = 40;
constexpr int DISPLAY_Y = 12;
constexpr int DISPLAY_BAND_ROWS = 54;
constexpr int DISPLAY_BANDS = DISPLAY_HEIGHT / DISPLAY_BAND_ROWS;
constexpr int DISPLAY_FRAME_DIVIDER = DISPLAY_BANDS;
constexpr uint32_t DISPLAY_SPI_HZ = 40000000;
static_assert(DISPLAY_WIDTH * 2 == GB_WIDTH * 3, "horizontal scale must be exactly 3:2");
static_assert(DISPLAY_HEIGHT * 2 == GB_HEIGHT * 3, "vertical scale must be exactly 3:2");
static_assert(DISPLAY_HEIGHT % DISPLAY_BAND_ROWS == 0, "display bands must cover the scaled frame exactly");
static_assert(DISPLAY_WIDTH * DISPLAY_BAND_ROWS * 2 < 32768, "one display band must fit one SPI transaction");

constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr uint8_t SPEAKER_VOLUME = 192;
constexpr uint8_t SPEAKER_CHANNEL = 1;
constexpr int AUDIO_RING_SAMPLES = 8192;
constexpr int AUDIO_RING_MASK = AUDIO_RING_SAMPLES - 1;
constexpr int AUDIO_CHUNK_SAMPLES = 512;
constexpr int AUDIO_CHUNK_SLOTS = 4;
constexpr int AUDIO_MIX_RING_SAMPLES = 2048;
constexpr int AUDIO_MIX_RING_MASK = AUDIO_MIX_RING_SAMPLES - 1;
constexpr int AUDIO_RING_TARGET = AUDIO_RING_SAMPLES / 2;
constexpr float AUDIO_RATE_EWMA_ALPHA = 0.03f;
constexpr int AUDIO_RATE_WARMUP_FRAMES = 120;
constexpr float AUDIO_RATE_FEEDBACK_GAIN = 0.1f;
constexpr float AUDIO_RATE_FEEDBACK_MAX = 0.02f;
constexpr float AUDIO_RATE_SLEW_MAX = 0.0025f;
constexpr uint32_t AUDIO_RATE_MIN = 4000;
constexpr uint32_t AUDIO_RATE_MAX = 48000;
constexpr uint32_t AUDIO_RESAMPLE_ONE = 1U << 16;
static_assert((AUDIO_RING_SAMPLES & AUDIO_RING_MASK) == 0, "audio ring size must be a power of two");
static_assert((AUDIO_MIX_RING_SAMPLES & AUDIO_MIX_RING_MASK) == 0, "mix ring size must be a power of two");

constexpr char SD_ROMS_DIR[] = "/roms";
constexpr uint32_t SD_SPI_HZ = 25000000;
constexpr size_t ROM_MAX_SIZE = 4 * 1024 * 1024;
constexpr int ROM_MAX_FILES = 64;
constexpr int ROM_NAME_MAX = 96;
constexpr uint32_t SRAM_QUIET_SAVE_MS = 5000;

constexpr int MENU_TOP = 30;
constexpr int MENU_ROW_HEIGHT = 24;
constexpr int MENU_VISIBLE_ROWS = 7;

constexpr int JOY_I2C_SDA = 9;
constexpr int JOY_I2C_SCL = 8;
constexpr uint8_t JOY2_I2C_ADDR = 0x63;
constexpr uint8_t JOY1_I2C_ADDR = 0x52;
constexpr uint32_t GROVE_I2C_FREQ = 100000;
constexpr uint32_t GROVE_POLL_MS = 8;
constexpr uint32_t JOY_REPROBE_MS = 1000;
constexpr int JOY_READ_FAIL_LIMIT = 5;
constexpr int JOY_DEADZONE = 40;
constexpr bool JOY_INVERT_X = true;
constexpr bool JOY_INVERT_Y = false;
constexpr bool JOY1_BTN_ACTIVE_HIGH = true;
constexpr int DUAL_BTN_PIN_BLUE = 18;
constexpr int DUAL_BTN_PIN_RED = 17;

constexpr int64_t GB_FRAME_US = 16743;   // 70224 / 4194304 seconds
constexpr uint32_t PERF_LOG_INTERVAL_MS = 1000;
constexpr uint32_t M5_UPDATE_FRAME_DIVIDER = 2;
