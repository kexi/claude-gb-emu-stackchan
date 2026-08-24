#include <M5Unified.h>

#include <atomic>

#include "../../core/gb.h"
#include "config.h"
#include "grove_input.h"

namespace {

constexpr uint8_t JOY2_REG_OFFSET_8BIT = 0x60;
constexpr uint8_t JOY2_REG_BUTTON = 0x20;

std::atomic<uint8_t> groveBits{0};
std::atomic<GroveJoystickKind> groveJoystickKind{GroveJoystickKind::None};
GroveJoystickKind joyKind = GroveJoystickKind::None;

uint8_t directionBits(int x, int y) {
    if (JOY_INVERT_X) x = -x;
    if (JOY_INVERT_Y) y = -y;
    uint8_t bits = 0;
    if (x > JOY_DEADZONE) bits |= gb::BTN_RIGHT;
    else if (x < -JOY_DEADZONE) bits |= gb::BTN_LEFT;
    if (y > JOY_DEADZONE) bits |= gb::BTN_UP;
    else if (y < -JOY_DEADZONE) bits |= gb::BTN_DOWN;
    return bits;
}

bool readJoy1(uint8_t* bits) {
    uint8_t raw[3];
    const bool readOk = M5.Ex_I2C.start(JOY1_I2C_ADDR, true, GROVE_I2C_FREQ) && M5.Ex_I2C.read(raw, sizeof(raw));
    M5.Ex_I2C.stop();
    if (!readOk) return false;
    *bits = directionBits((int)raw[0] - 128, (int)raw[1] - 128);
    const bool pressed = JOY1_BTN_ACTIVE_HIGH ? raw[2] != 0 : raw[2] == 0;
    if (pressed) *bits |= gb::BTN_START;
    return true;
}

bool readJoy2(uint8_t* bits) {
    uint8_t offset[2];
    uint8_t button = 1;
    const bool readOk =
        M5.Ex_I2C.readRegister(JOY2_I2C_ADDR, JOY2_REG_OFFSET_8BIT, offset, sizeof(offset), GROVE_I2C_FREQ) &&
        M5.Ex_I2C.readRegister(JOY2_I2C_ADDR, JOY2_REG_BUTTON, &button, 1, GROVE_I2C_FREQ);
    if (!readOk) return false;
    *bits = directionBits((int8_t)offset[0], (int8_t)offset[1]);
    if (button == 0) *bits |= gb::BTN_START;
    return true;
}

GroveJoystickKind probeJoystick() {
    // A bare ACK was not sufficient on the assembled CoreS3: an address could
    // ACK while its expected register transaction failed, causing reconnect
    // churn. Announce a device only after one complete model-specific read.
    uint8_t bits = 0;
    if (readJoy2(&bits)) return GroveJoystickKind::Joy2;
    if (readJoy1(&bits)) return GroveJoystickKind::Joy1;
    return GroveJoystickKind::None;
}

void groveTask(void*) {
    uint32_t lastProbeMs = 0;
    uint8_t joystickBits = 0;
    int failedReads = 0;
    for (;;) {
        uint8_t bits = 0;
        if (digitalRead(DUAL_BTN_PIN_RED) == LOW) bits |= gb::BTN_A;
        if (digitalRead(DUAL_BTN_PIN_BLUE) == LOW) bits |= gb::BTN_B;

        if (joyKind == GroveJoystickKind::None) {
            const uint32_t now = millis();
            const bool shouldProbe = now - lastProbeMs >= JOY_REPROBE_MS;
            if (shouldProbe) {
                lastProbeMs = now;
                joyKind = probeJoystick();
                groveJoystickKind.store(joyKind, std::memory_order_relaxed);
            }
        } else {
            uint8_t freshBits = 0;
            const bool readOk = joyKind == GroveJoystickKind::Joy2 ? readJoy2(&freshBits) : readJoy1(&freshBits);
            if (readOk) {
                failedReads = 0;
                joystickBits = freshBits;
            } else {
                failedReads++;
                const bool joystickLost = failedReads >= JOY_READ_FAIL_LIMIT;
                if (joystickLost) {
                    joyKind = GroveJoystickKind::None;
                    groveJoystickKind.store(joyKind, std::memory_order_relaxed);
                    joystickBits = 0;
                }
            }
            bits |= joystickBits;
        }

        groveBits.store(bits, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(GROVE_POLL_MS));
    }
}

}   // namespace

uint8_t groveInputBits() { return groveBits.load(std::memory_order_relaxed); }

GroveJoystickKind groveInputJoystickKind() { return groveJoystickKind.load(std::memory_order_relaxed); }

void groveInputInit() {
    pinMode(DUAL_BTN_PIN_BLUE, INPUT_PULLUP);
    pinMode(DUAL_BTN_PIN_RED, INPUT_PULLUP);
    M5.Ex_I2C.begin(M5.Ex_I2C.getPort(), JOY_I2C_SDA, JOY_I2C_SCL);
    joyKind = probeJoystick();
    groveJoystickKind.store(joyKind, std::memory_order_relaxed);
    xTaskCreatePinnedToCore(groveTask, "grove", 3072, nullptr, 3, nullptr, 0);
}
