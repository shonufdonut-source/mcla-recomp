// iOS platform pieces the SDK only has for SDL: a windowed-app context (runs queued work on
// the main thread) and a Window that hands the presenter the Swift app's CAMetalLayer.
// Shared by the graphics test (mcla_gfx.cpp) and the game host (mcla_game.cpp).
#pragma once

#include "mcla_probe.h"

#include <rex/logging.h>
#include <rex/ui/surface_mac.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <spdlog/sinks/callback_sink.h>

#include <dispatch/dispatch.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>

namespace mcla_ios {

inline mcla_log_fn g_log = nullptr;

inline void P(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_log) g_log(buf);
}

// SDK log lines go to the app log too, so its own errors show up there. A line that repeats
// word for word (MoltenVK warns on every bind of some pipelines) is shown three times, then
// only as a count every 1000 repeats. The sink's mutex guards the table.
inline spdlog::sink_ptr MakeSdkLogSink() {
  return std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg& msg) {
    static std::unordered_map<std::string, uint32_t> repeats;
    std::string text(msg.payload.data(), msg.payload.size());
    const char* level = spdlog::level::to_short_c_str(msg.level);
    if (msg.level >= spdlog::level::warn) {
      if (repeats.size() > 4096) repeats.clear();
      const uint32_t count = ++repeats[text];
      if (count == 3) {
        P("  [sdk %s] %s (repeats are counted from here)", level, text.c_str());
        return;
      }
      if (count > 3) {
        if (count % 1000 == 0) P("  [sdk %s] seen %u times: %s", level, count, text.c_str());
        return;
      }
    }
    P("  [sdk %s] %s", level, text.c_str());
  });
}

class IOSAppContext final : public rex::ui::WindowedAppContext {
 public:
  void NotifyUILoopOfPendingFunctions() override {
    dispatch_async_f(dispatch_get_main_queue(), this, [](void* context) {
      static_cast<IOSAppContext*>(context)->ExecutePendingFunctionsFromUIThread();
    });
  }
  void PlatformQuitFromUIThread() override {}
};

class IOSWindow final : public rex::ui::Window {
 public:
  IOSWindow(rex::ui::WindowedAppContext& context, const void* layer, uint32_t width,
            uint32_t height)
      : Window(context, "MCLA", width, height), layer_(layer), width_(width), height_(height) {}

  ~IOSWindow() override { EnterDestructor(); }

  void Resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(width, height, destruction_receiver);
  }

 protected:
  bool OpenImpl() override {
    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(width_, height_, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) return true;
    OnFocusUpdate(true, destruction_receiver);
    return true;
  }

  void RequestCloseImpl() override {
    WindowDestructionReceiver destruction_receiver(this);
    OnBeforeClose(destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) return;
    OnAfterClose();
  }

  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(
      rex::ui::Surface::TypeFlags allowed_types) override {
    if (!(allowed_types & rex::ui::Surface::kTypeFlag_CAMetalLayer) || !layer_) return nullptr;
    // No SDL window or view: the layer belongs to the Swift app (SDK patch 05 reads its size).
    return std::make_unique<rex::ui::CAMetalLayerSurface>(nullptr, nullptr, layer_);
  }

  void RequestPaintImpl() override {
    if (paint_pending_.exchange(true)) return;
    app_context().CallInUIThreadDeferred([this] {
      paint_pending_ = false;
      OnPaint();
      ++paints_;
    });
  }

 public:
  // Presenter paints so far: each one shows the newest guest frame, so paints per second is
  // the frame rate on screen.
  uint64_t paints() const { return paints_.load(); }

 private:
  std::atomic<uint64_t> paints_{0};
  const void* layer_;
  uint32_t width_, height_;
  std::atomic<bool> paint_pending_{false};
};

}  // namespace mcla_ios
