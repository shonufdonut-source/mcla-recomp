// Controllers for the game through Apple's GameController framework (Xbox, PlayStation and MFi
// pads paired in Settings > Bluetooth). Implemented in ios_gamepad.mm.
#pragma once

#include <memory>

namespace rex::input {
class InputDriver;
}

namespace mcla_ios {

std::unique_ptr<rex::input::InputDriver> CreateGameControllerDriver();

}  // namespace mcla_ios
