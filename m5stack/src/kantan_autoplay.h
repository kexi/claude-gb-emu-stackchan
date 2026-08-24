#pragma once

#include "../../core/gb.h"

namespace stackchan {

constexpr uint32_t KANTAN_DEMO_PRESS_FIRST_FRAME = 206;
constexpr uint32_t KANTAN_DEMO_PRESS_FRAME_COUNT = 4;

constexpr uint8_t kantanDemoInput(uint32_t frame) {
    const bool isDemoGestureFrame =
        frame >= KANTAN_DEMO_PRESS_FIRST_FRAME && frame < KANTAN_DEMO_PRESS_FIRST_FRAME + KANTAN_DEMO_PRESS_FRAME_COUNT;
    return isDemoGestureFrame ? gb::BTN_SELECT | gb::BTN_A : 0;
}

}   // namespace stackchan
