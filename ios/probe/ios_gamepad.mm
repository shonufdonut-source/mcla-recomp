// GameController input driver: maps an extended gamepad onto the Xbox 360 pad the game expects.
//
// Controllers arrive through GCControllerDidConnect/DidDisconnect notifications (posted on the
// main thread); the game polls state from its own threads, which read the elements' current
// values directly. Everything the game asks for is logged once, and the first button changes are
// logged, so a log shows whether the game sees the pad and what it reads.

#include "ios_gamepad.h"

#include "ios_host.h"

#include <rex/input/input_driver.h>

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace mcla_ios {
namespace {

// The Xbox types and status codes (X_RESULT, X_INPUT_STATE, ...) live in these namespaces.
using namespace rex;
using namespace rex::input;

// "GC" in the top bytes keeps these apart from the other drivers' device handles.
constexpr uint64_t kDeviceBase = 0x4743000000000000ull;
constexpr int kLoggedButtonChanges = 60;

struct Pad {
  uint64_t serial = 0;
  GCController* controller = nil;
  std::string name;
  X_INPUT_GAMEPAD last{};
  uint32_t packet = 0;
  bool caps_logged = false;
  bool state_logged = false;
};

int16_t Axis(float value) {
  value = std::clamp(value, -1.0f, 1.0f);
  return static_cast<int16_t>(value >= 0 ? std::lround(value * 32767.0f)
                                         : std::lround(value * 32768.0f));
}

uint8_t Trigger(float value) {
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::string ButtonNames(uint16_t buttons) {
  static const struct {
    uint16_t bit;
    const char* name;
  } kNames[] = {{X_INPUT_GAMEPAD_A, "A"},
                {X_INPUT_GAMEPAD_B, "B"},
                {X_INPUT_GAMEPAD_X, "X"},
                {X_INPUT_GAMEPAD_Y, "Y"},
                {X_INPUT_GAMEPAD_START, "Start"},
                {X_INPUT_GAMEPAD_BACK, "Back"},
                {X_INPUT_GAMEPAD_GUIDE, "Guide"},
                {X_INPUT_GAMEPAD_LEFT_SHOULDER, "LB"},
                {X_INPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
                {X_INPUT_GAMEPAD_LEFT_THUMB, "LS"},
                {X_INPUT_GAMEPAD_RIGHT_THUMB, "RS"},
                {X_INPUT_GAMEPAD_DPAD_UP, "Up"},
                {X_INPUT_GAMEPAD_DPAD_DOWN, "Down"},
                {X_INPUT_GAMEPAD_DPAD_LEFT, "Left"},
                {X_INPUT_GAMEPAD_DPAD_RIGHT, "Right"}};
  std::string out;
  for (const auto& entry : kNames) {
    if (!(buttons & entry.bit)) continue;
    if (!out.empty()) out += "+";
    out += entry.name;
  }
  return out.empty() ? "none" : out;
}

class GameControllerDriver final : public rex::input::InputDriver {
 public:
  GameControllerDriver() : InputDriver(nullptr, 0) {}

  ~GameControllerDriver() override {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    if (connect_observer_) [center removeObserver:connect_observer_];
    if (disconnect_observer_) [center removeObserver:disconnect_observer_];
  }

  X_STATUS Setup() override {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    connect_observer_ = [center addObserverForName:GCControllerDidConnectNotification
                                            object:nil
                                             queue:nil
                                        usingBlock:^(NSNotification* note) {
                                          Add((GCController*)note.object,
                                              "connected");
                                        }];
    disconnect_observer_ = [center addObserverForName:GCControllerDidDisconnectNotification
                                               object:nil
                                                queue:nil
                                           usingBlock:^(NSNotification* note) {
                                             Remove((GCController*)note.object);
                                           }];
    for (GCController* controller in GCController.controllers) {
      Add(controller, "already connected");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    P("input: GameController driver ready, %d controller(s); pads paired in Settings > "
      "Bluetooth connect on their own",
      static_cast<int>(pads_.size()));
    return X_STATUS_SUCCESS;
  }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& pad : pads_) {
      DeviceInfo info;
      info.id = static_cast<DeviceId>(kDeviceBase | pad.serial);
      info.name = pad.name;
      out.push_back(info);
    }
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override {
    std::lock_guard<std::mutex> lock(mutex_);
    Pad* pad = Find(id);
    if (!pad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!pad->caps_logged) {
      pad->caps_logged = true;
      P("input: game asked for the capabilities of '%s' (flags %u)", pad->name.c_str(), flags);
    }
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xF7FF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    std::lock_guard<std::mutex> lock(mutex_);
    Pad* pad = Find(id);
    if (!pad) return X_ERROR_DEVICE_NOT_CONNECTED;
    GCExtendedGamepad* gamepad = pad->controller.extendedGamepad;
    if (!gamepad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!pad->state_logged) {
      pad->state_logged = true;
      P("input: game is reading '%s'", pad->name.c_str());
    }

    uint16_t buttons = 0;
    auto press = [&buttons](GCControllerButtonInput* button, uint16_t bit) {
      if (button && button.isPressed) buttons |= bit;
    };
    press(gamepad.dpad.up, X_INPUT_GAMEPAD_DPAD_UP);
    press(gamepad.dpad.down, X_INPUT_GAMEPAD_DPAD_DOWN);
    press(gamepad.dpad.left, X_INPUT_GAMEPAD_DPAD_LEFT);
    press(gamepad.dpad.right, X_INPUT_GAMEPAD_DPAD_RIGHT);
    press(gamepad.buttonMenu, X_INPUT_GAMEPAD_START);
    press(gamepad.buttonOptions, X_INPUT_GAMEPAD_BACK);
    press(gamepad.leftThumbstickButton, X_INPUT_GAMEPAD_LEFT_THUMB);
    press(gamepad.rightThumbstickButton, X_INPUT_GAMEPAD_RIGHT_THUMB);
    press(gamepad.leftShoulder, X_INPUT_GAMEPAD_LEFT_SHOULDER);
    press(gamepad.rightShoulder, X_INPUT_GAMEPAD_RIGHT_SHOULDER);
    press(gamepad.buttonHome, X_INPUT_GAMEPAD_GUIDE);
    // GameController names the face buttons by position in the Xbox layout (A at the bottom).
    press(gamepad.buttonA, X_INPUT_GAMEPAD_A);
    press(gamepad.buttonB, X_INPUT_GAMEPAD_B);
    press(gamepad.buttonX, X_INPUT_GAMEPAD_X);
    press(gamepad.buttonY, X_INPUT_GAMEPAD_Y);

    X_INPUT_GAMEPAD now{};
    now.buttons = buttons;
    now.left_trigger = Trigger(gamepad.leftTrigger.value);
    now.right_trigger = Trigger(gamepad.rightTrigger.value);
    // Both use up = positive Y.
    now.thumb_lx = Axis(gamepad.leftThumbstick.xAxis.value);
    now.thumb_ly = Axis(gamepad.leftThumbstick.yAxis.value);
    now.thumb_rx = Axis(gamepad.rightThumbstick.xAxis.value);
    now.thumb_ry = Axis(gamepad.rightThumbstick.yAxis.value);

    if (std::memcmp(&now, &pad->last, sizeof(now)) != 0) {
      const uint16_t before = pad->last.buttons;
      if (before != buttons && logged_changes_ < kLoggedButtonChanges) {
        ++logged_changes_;
        P("input: buttons %s%s", ButtonNames(buttons).c_str(),
          logged_changes_ == kLoggedButtonChanges ? " (no more button logging this run)" : "");
      }
      pad->last = now;
      ++pad->packet;
    }
    if (out_state) {
      out_state->packet_number = pad->packet;
      out_state->gamepad = now;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    std::lock_guard<std::mutex> lock(mutex_);
    // Rumble through GCDeviceHaptics comes later; accepting it keeps the game content.
    return Find(id) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Find(id)) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!keystroke_logged_) {
      keystroke_logged_ = true;
      P("input: game asked for keystrokes (not translated yet)");
    }
    return X_ERROR_EMPTY;
  }

 private:
  void Add(GCController* controller, const char* how) {
    if (!controller) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& pad : pads_) {
      if (pad.controller == controller) return;
    }
    NSString* vendor = controller.vendorName ?: @"controller";
    NSString* category = controller.productCategory ?: @"";
    if (!controller.extendedGamepad) {
      P("input: '%s' (%s) %s but has no extended gamepad profile; ignored", vendor.UTF8String,
        category.UTF8String, how);
      return;
    }
    Pad pad;
    pad.serial = ++next_serial_;
    pad.controller = controller;
    pad.name = vendor.UTF8String;
    pads_.push_back(pad);
    P("input: '%s' (%s) %s", vendor.UTF8String, category.UTF8String, how);
  }

  void Remove(GCController* controller) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(pads_.begin(), pads_.end(),
                           [controller](const Pad& pad) { return pad.controller == controller; });
    if (it == pads_.end()) return;
    P("input: '%s' disconnected", it->name.c_str());
    pads_.erase(it);
  }

  Pad* Find(DeviceId id) {
    for (auto& pad : pads_) {
      if (static_cast<DeviceId>(kDeviceBase | pad.serial) == id) return &pad;
    }
    return nullptr;
  }

  std::mutex mutex_;
  std::vector<Pad> pads_;
  uint64_t next_serial_ = 0;
  int logged_changes_ = 0;
  bool keystroke_logged_ = false;
  id connect_observer_ = nil;
  id disconnect_observer_ = nil;
};

}  // namespace

std::unique_ptr<rex::input::InputDriver> CreateGameControllerDriver() {
  return std::make_unique<GameControllerDriver>();
}

}  // namespace mcla_ios
