#pragma once

#include <cstdint>

enum class GroveJoystickKind : uint8_t { None, Joy1, Joy2 };

void groveInputInit();
uint8_t groveInputBits();
GroveJoystickKind groveInputJoystickKind();
