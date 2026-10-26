// MCLA iOS game host: the recompiled game linked with the runtime, started one logged stage
// at a time.
//
// Like ps5/game/main_ps5.cpp, this drives rex::Runtime directly in the order ReXApp does
// (setup, XEX load, guest heap, main thread) instead of going through ReXApp and its SDL
// window. Every stage is announced before it starts, so the last "NEXT" line in the log
// names what was running if the app dies.
//
// The recompiled game (generated/default) only exists in the owner's private build, which
// compiles this file with MCLA_HAS_GAME=1 and links the generated code and the host sources
// from src/. The public build compiles it with MCLA_HAS_GAME=0: same API, no game.

#include "ios_gamepad.h"
#include "ios_host.h"

#ifndef MCLA_HAS_GAME
#define MCLA_HAS_GAME 0
#endif

#include <string>
#include <utility>
#include <vector>

namespace {
std::vector<std::pair<std::string, std::string>> g_launcher_options;
}

extern "C" void mcla_game_set_option(const char* name, const char* value) {
  if (name && value) g_launcher_options.emplace_back(name, value);
}

#if MCLA_HAS_GAME

#include <rex/cvar.h>
#include <rex/image_info.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/kernel/crt/heap.h>
#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xthread.h>
#include <rex/ui/presenter.h>

// Only for SDL_SetMainReady: without SDL_MAIN_HANDLED this header would define main() on iOS,
// which belongs to the Swift app.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <mach/mach.h>
#include <os/proc.h>

#include <chrono>
#include <filesystem>
#include <string>

// generated/default/mcla_init.cpp
extern const rex::PPCImageInfo PPCImageConfig;
// src/audio_fallback.cpp: SDL output if it opens, otherwise silence.
std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher);
// src/settings_menu.cpp: where the in-game settings menu saves.
extern const char* g_mcla_settings_path;
// The Xenos GPU plugin's factory. On the desktop the plugin is a shared library found by name
// at run time; iOS cannot load code, so here (as on the PS5) it is linked in (SDK patch 07).
extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,
                                                        const rex::system::GpuCreateInfo* info);

REXCVAR_DECLARE(uint32_t, rexcrt_heap_size_mb);

namespace {

using mcla_ios::IOSAppContext;
using mcla_ios::IOSWindow;
using mcla_ios::P;

#define NEXT(...) P("NEXT " __VA_ARGS__)

struct GameState {
  int stage = 0;
  std::string settings_path;
  std::unique_ptr<IOSAppContext> context;
  std::unique_ptr<IOSWindow> window;
  std::unique_ptr<rex::Runtime> runtime;
  rex::system::object_ref<rex::system::XThread> main_thread;
  std::chrono::steady_clock::time_point resumed_at;
  int last_alive_second = 0;
  std::filesystem::path cache_root;
  uint64_t paints_at_last_report = 0;
};

GameState* g_game = nullptr;

int Stop(const char* what, int code) {
  P("mcla-ios stops: %s (code %d)", what, code);
  rex::FlushLogging();
  return code;
}

void ListGuestThreads() {
  auto threads =
      g_game->runtime->kernel_state()->object_table()->GetObjectsByType<rex::system::XThread>();
  P("threads: %d runtime threads", static_cast<int>(threads.size()));
  for (auto& thread : threads) {
    if (!thread) continue;
    P("  thread '%s' id %u%s", thread->name().c_str(), static_cast<unsigned>(thread->thread_id()),
      thread->is_guest_thread() ? " (guest)" : "");
  }
}

// This process's memory as iOS counts it (phys_footprint, what jetsam kills on) and what iOS
// still grants; when the app vanishes without a crash line, these show if memory was the cause.
void LogMemory() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  const bool ok = task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info),
                            &count) == KERN_SUCCESS;
  P("memory: %llu MB used, %llu MB still available to the app",
    ok ? static_cast<unsigned long long>(info.phys_footprint >> 20) : 0ull,
    static_cast<unsigned long long>(os_proc_available_memory() >> 20));
}

// What the shader storage holds: if a second run lists the same files and still creates as many
// pipelines, the storage is written but not used; if the folder stays empty, it is not written.
void ListCache(const char* when) {
  std::error_code ec;
  uint64_t total = 0;
  int files = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(g_game->cache_root, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const uint64_t size = it->file_size(ec);
    total += size;
    if (++files <= 20) {
      P("  cache file %s: %llu KB",
        std::filesystem::relative(it->path(), g_game->cache_root, ec).string().c_str(),
        static_cast<unsigned long long>(size / 1024));
    }
  }
  P("shader cache %s: %d files, %llu KB", when, files,
    static_cast<unsigned long long>(total / 1024));
}

}  // namespace

extern "C" int mcla_game_available(void) { return 1; }

extern "C" int mcla_game_start(const void* metal_layer, unsigned width_px, unsigned height_px,
                               const char* game_dir, const char* data_dir, int stage,
                               mcla_log_fn log) {
  mcla_ios::g_log = log;
  if (g_game) {
    P("game host: already started this session (stage %d); restart the app to run again",
      g_game->stage);
    return 1;
  }
  g_game = new GameState();
  g_game->stage = stage;
  P("mcla-ios starts, stage %d", stage);

  const std::filesystem::path game_root = game_dir;
  const std::filesystem::path data_root = data_dir;
  const std::filesystem::path user_root = data_root / "user";
  const std::filesystem::path cache_root = data_root / "cache";
  g_game->cache_root = cache_root;

  NEXT("check the game folder %s", game_root.c_str());
  std::error_code ec;
  if (!std::filesystem::is_regular_file(game_root / "default.xex", ec)) {
    return Stop("default.xex is not in the game folder", 2);
  }
  std::filesystem::create_directories(user_root, ec);
  std::filesystem::create_directories(cache_root, ec);

  NEXT("initialise cvars and logging (app log)");
  char program[] = "mcla";
  char* arguments[] = {program, nullptr};
  rex::cvar::Init(1, arguments);
  rex::InitLoggingEarly();
  rex::LogConfig log_config;
  log_config.log_to_console = false;
  log_config.extra_sinks.push_back(mcla_ios::MakeSdkLogSink());
  log_config.flush_level = spdlog::level::trace;
  rex::InitLogging(log_config);

  // What the in-game settings menu saved; the app bundle is read-only, so it lives in data_dir.
  g_game->settings_path = (data_root / "mcla.toml").string();
  g_mcla_settings_path = g_game->settings_path.c_str();
  if (std::filesystem::exists(g_game->settings_path, ec)) {
    try {
      rex::cvar::LoadConfig(g_game->settings_path);
      P("settings: %s read", g_mcla_settings_path);
    } catch (const std::exception& e) {
      P("settings: %s not read: %s", g_mcla_settings_path, e.what());
    }
  }
  // No controller mapping file is shipped; do not look for one by a relative path.
  rex::cvar::SetFlagByName("hid_mappings_file", "");
  for (const auto& [name, value] : g_launcher_options) {
    const bool ok = rex::cvar::SetFlagByName(name, value);
    P("launcher setting: %s = %s%s", name.c_str(), value.c_str(), ok ? "" : " (not accepted)");
  }
  // The app owns UIApplication; SDL (audio only, if it opens) must not wait for SDL_main.
  SDL_SetMainReady();

  rex::RuntimeConfig config;
  config.audio_factory = &CreateMclaAudioSystem;
  config.kernel_init = rex::kernel::InitializeKernel;

  if (stage >= 5) {
    // Presentation first, as the desktop host does: the graphics system has to know it will
    // present before the runtime wires it to the guest.
    g_game->context = std::make_unique<IOSAppContext>();
    NEXT("create the Xenos graphics system (Vulkan backend, MoltenVK)");
    rex::system::GpuCreateInfo gpu_create_info;
    gpu_create_info.struct_size = sizeof gpu_create_info;
    gpu_create_info.backend = "vulkan";
    std::unique_ptr<rex::system::IGraphicsSystem> graphics(
        rex_gpu_create(rex::system::kGpuPluginAbiVersion, &gpu_create_info));
    if (!graphics) {
      return Stop("the GPU plugin returned no graphics system", 7);
    }
    NEXT("SetupPresentation with the iOS app context");
    if (XFAILED(graphics->SetupPresentation(g_game->context.get())) || !graphics->presenter()) {
      return Stop("graphics setup failed", 8);
    }
    NEXT("open the iOS window (%ux%u px) and attach the presenter", width_px, height_px);
    g_game->window =
        std::make_unique<IOSWindow>(*g_game->context, metal_layer, width_px, height_px);
    if (!g_game->window->Open()) {
      return Stop("the window did not open", 10);
    }
    g_game->window->SetPresenter(graphics->presenter());
    config.graphics = std::move(graphics);
    // Controllers through GameController. The idle stand-in stays behind them, as on the
    // desktop, so the game still sees a pad in slot 0 while none is connected.
    config.input_factory = [](bool) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = std::make_unique<rex::input::InputSystem>(nullptr);
      // InputSystem::Setup does not set its drivers up; the host does, as ReXApp does.
      auto pads = mcla_ios::CreateGameControllerDriver();
      pads->Setup();
      input->AddDriver(std::move(pads));
      input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 0));
      input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
      return input;
    };
  }

  NEXT("construct rex::Runtime");
  g_game->runtime = std::make_unique<rex::Runtime>(game_root, user_root, std::filesystem::path(),
                                                   cache_root, std::filesystem::path());
  auto* runtime = g_game->runtime.get();
  if (stage >= 5) {
    runtime->set_app_context(g_game->context.get());
    runtime->set_display_window(g_game->window.get());
  }

  rex::PPCImageInfo image = PPCImageConfig;
  NEXT("Runtime::Setup (guest memory, function table, kernel state, file systems)");
  auto status = runtime->Setup(image, std::move(config));
  if (XFAILED(status)) {
    P("Runtime::Setup failed: %08X", static_cast<unsigned>(status));
    return Stop("setup failed", 3);
  }
  if (image.register_modules) {
    NEXT("register guest modules");
    image.register_modules(runtime->kernel_state());
  }
  if (stage >= 5 && runtime->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime->input_system())
        ->AttachWindow(g_game->window.get());
  }
  P("PASS Runtime::Setup");
  if (stage <= 1) return Stop("stage 1 complete", 0);

  NEXT("Runtime::LoadXexImage game:\\default.xex");
  status = runtime->LoadXexImage("game:\\default.xex");
  if (XFAILED(status)) {
    P("LoadXexImage failed: %08X", static_cast<unsigned>(status));
    return Stop("XEX load failed", 4);
  }
  P("PASS XEX image loaded, title id %08X",
    static_cast<unsigned>(runtime->kernel_state()->title_id()));
  if (stage <= 2) return Stop("stage 2 complete", 0);

  if (image.rexcrt_heap) {
    NEXT("create the guest heap (%u MiB)", static_cast<unsigned>(REXCVAR_GET(rexcrt_heap_size_mb)));
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime->memory())) {
      return Stop("guest heap creation failed", 5);
    }
  }
  // The game probes for loose city data on a "t:" drive retail consoles do not have.
  runtime->file_system()->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");

  NEXT("Runtime::PrepareModuleLaunch (suspended main guest thread)");
  g_game->main_thread = runtime->PrepareModuleLaunch();
  if (!g_game->main_thread) {
    return Stop("could not create the main guest thread", 6);
  }
  P("PASS main guest thread created: the entry point is ready to run");
  if (stage <= 3) return Stop("stage 3 complete", 0);

  if (stage >= 5 && runtime->graphics_system()) {
    NEXT("initialise shader storage under %s", cache_root.c_str());
    ListCache("before loading");
    runtime->graphics_system()->InitializeShaderStorage(
        cache_root, runtime->kernel_state()->title_id(), true);
  }

  NEXT("resume the main guest thread; guest code runs from here");
  g_game->main_thread->Resume();
  g_game->resumed_at = std::chrono::steady_clock::now();
  P("PASS main guest thread resumed%s", stage >= 5 ? " (with graphics)" : " (no graphics: the "
    "game is expected to stop at its video setup)");
  return 0;
}

extern "C" void mcla_game_tick(void) {
  if (!g_game || !g_game->main_thread || g_game->resumed_at.time_since_epoch().count() == 0) {
    return;
  }
  const int second = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::steady_clock::now() - g_game->resumed_at)
                                          .count());
  if (second <= g_game->last_alive_second) return;
  g_game->last_alive_second = second;
  if (second <= 30 || second % 30 == 0) P("alive: %d s", second);
  if (g_game->window && second % 5 == 0) {
    const uint64_t paints = g_game->window->paints();
    P("display: %.1f fps over the last 5 s (presenter paints)",
      static_cast<double>(paints - g_game->paints_at_last_report) / 5.0);
    g_game->paints_at_last_report = paints;
    LogMemory();
  }
  if (g_game->window && (second == 60 || second == 300)) ListCache("now");
  // Twice, a few seconds apart: which threads exist, and whether the set changes.
  if (second == 4 || second == 8) ListGuestThreads();
}

extern "C" void mcla_game_resize(unsigned width_px, unsigned height_px) {
  if (g_game && g_game->window) g_game->window->Resize(width_px, height_px);
}

#else  // !MCLA_HAS_GAME

extern "C" int mcla_game_available(void) { return 0; }

extern "C" int mcla_game_start(const void*, unsigned, unsigned, const char*, const char*, int,
                               mcla_log_fn log) {
  mcla_ios::g_log = log;
  mcla_ios::P("game host: this build has no game code (public build); the owner's private "
              "build links it in");
  return 1;
}

extern "C" void mcla_game_tick(void) {}

extern "C" void mcla_game_resize(unsigned, unsigned) {}

#endif  // MCLA_HAS_GAME
