// MCLA iOS graphics test: the SDK's own Vulkan provider, presenter and immediate drawer,
// presenting into a CAMetalLayer owned by the Swift app. No game code, no game data.
//
// iOS needs two small platform classes the SDK only has for SDL: a windowed-app context
// (runs queued work on the main thread) and a Window (hands the presenter our layer).

#include "mcla_probe.h"

#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface_mac.h>
#include <rex/ui/ui_drawer.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <spdlog/sinks/callback_sink.h>

#include <dispatch/dispatch.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>

namespace {

mcla_log_fn g_log = nullptr;

void P(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_log(buf);
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
    });
  }

 private:
  const void* layer_;
  uint32_t width_, height_;
  std::atomic<bool> paint_pending_{false};
};

// Draws a bar that sweeps across a coloured background, so a frozen or black screen is
// obvious, and counts frames for the frame-rate log.
class TestDrawer final : public rex::ui::UIDrawer {
 public:
  explicit TestDrawer(rex::ui::ImmediateDrawer& drawer) : drawer_(drawer) {}

  void Draw(rex::ui::UIDrawContext& context) override {
    const float w = float(context.render_target_width());
    const float h = float(context.render_target_height());
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    const float x = float((std::sin(t * 1.5) * 0.5 + 0.5) * (w - w * 0.2f));

    // Colours are 0xAABBGGRR.
    const uint32_t bg = 0xFF803010u;   // dark blue
    const uint32_t bar = 0xFF20C0FFu;  // orange
    rex::ui::ImmediateVertex v[8] = {
        {0, 0, 0, 0, bg},      {w, 0, 1, 0, bg},      {w, h, 1, 1, bg},
        {0, h, 0, 1, bg},      {x, h * 0.3f, 0, 0, bar}, {x + w * 0.2f, h * 0.3f, 1, 0, bar},
        {x + w * 0.2f, h * 0.7f, 1, 1, bar}, {x, h * 0.7f, 0, 1, bar},
    };
    const uint16_t idx[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};

    drawer_.Begin(context, w, h);
    rex::ui::ImmediateDrawBatch batch;
    batch.vertices = v;
    batch.vertex_count = 8;
    batch.indices = idx;
    batch.index_count = 12;
    drawer_.BeginDrawBatch(batch);
    rex::ui::ImmediateDraw draw;
    draw.primitive_type = rex::ui::ImmediatePrimitiveType::kTriangles;
    draw.count = 12;
    drawer_.Draw(draw);
    drawer_.EndDrawBatch();
    drawer_.End();
    ++frames_;
  }

  uint64_t frames() const { return frames_; }

 private:
  rex::ui::ImmediateDrawer& drawer_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  uint64_t frames_ = 0;
};

struct GfxState {
  std::unique_ptr<IOSAppContext> context;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider;
  std::unique_ptr<IOSWindow> window;
  std::unique_ptr<rex::ui::Presenter> presenter;
  std::unique_ptr<rex::ui::ImmediateDrawer> drawer;
  std::unique_ptr<TestDrawer> test_drawer;
  uint64_t last_frames = 0;
  std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
  bool reported_first_frame = false;
};

GfxState* g_gfx = nullptr;

}  // namespace

extern "C" int mcla_gfx_start(const void* metal_layer, unsigned width_px, unsigned height_px,
                              mcla_log_fn log) {
  if (g_gfx) return 0;
  g_log = log;
  P("graphics test: start (%ux%u px), SDK Vulkan provider + presenter, MoltenVK linked statically",
    width_px, height_px);

  // SDK log lines go to the app log too, so its own errors show up there.
  rex::InitLogging(nullptr);
  rex::AddSink(std::make_shared<spdlog::sinks::callback_sink_mt>(
      [](const spdlog::details::log_msg& msg) {
        std::string text(msg.payload.data(), msg.payload.size());
        P("  [sdk %s] %s", spdlog::level::to_short_c_str(msg.level), text.c_str());
      }));

  auto* s = new GfxState();
  s->context = std::make_unique<IOSAppContext>();

  // With GPU emulation requirements first (what the game needs), then presentation only.
  s->provider = rex::ui::vulkan::VulkanProvider::Create(true, true);
  if (s->provider) {
    P("  VulkanProvider with GPU emulation requirements: OK");
  } else {
    P("  VulkanProvider with GPU emulation requirements: FAILED (see [sdk] lines); trying "
      "presentation only");
    s->provider = rex::ui::vulkan::VulkanProvider::Create(false, true);
    if (!s->provider) {
      P("  VulkanProvider presentation only: FAILED");
      delete s;
      return 1;
    }
    P("  VulkanProvider presentation only: OK");
  }

  s->window = std::make_unique<IOSWindow>(*s->context, metal_layer, width_px, height_px);
  if (!s->window->Open()) {
    P("  window Open FAILED");
    delete s;
    return 2;
  }
  s->presenter = s->provider->CreatePresenter();
  if (!s->presenter) {
    P("  CreatePresenter FAILED");
    delete s;
    return 3;
  }
  s->drawer = s->provider->CreateImmediateDrawer();
  if (!s->drawer) {
    P("  CreateImmediateDrawer FAILED");
    delete s;
    return 4;
  }
  s->drawer->SetPresenter(s->presenter.get());
  s->test_drawer = std::make_unique<TestDrawer>(*s->drawer);
  s->window->SetPresenter(s->presenter.get());
  s->presenter->AddUIDrawerFromUIThread(s->test_drawer.get(), 0);
  s->window->RequestPaint();
  P("  presenter attached to the CAMetalLayer; painting");
  g_gfx = s;
  return 0;
}

extern "C" void mcla_gfx_tick(void) {
  if (!g_gfx) return;
  g_gfx->presenter->RequestUIPaintFromUIThread();
  const uint64_t frames = g_gfx->test_drawer->frames();
  if (!g_gfx->reported_first_frame && frames > 0) {
    g_gfx->reported_first_frame = true;
    P("  first frame drawn through the SDK presenter");
  }
  auto now = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(now - g_gfx->last_report).count();
  if (secs >= 5.0) {
    P("  %.1f frames/s over the last %.0f s (%llu total)",
      double(frames - g_gfx->last_frames) / secs, secs, (unsigned long long)frames);
    g_gfx->last_frames = frames;
    g_gfx->last_report = now;
  }
}

extern "C" void mcla_gfx_resize(unsigned width_px, unsigned height_px) {
  if (g_gfx) g_gfx->window->Resize(width_px, height_px);
}
