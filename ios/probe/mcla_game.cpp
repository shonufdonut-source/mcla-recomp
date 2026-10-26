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
#include <rex/graphics/gpu_stats.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/info.h>
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
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/provider.h>

// Only for SDL_SetMainReady: without SDL_MAIN_HANDLED this header would define main() on iOS,
// which belongs to the Swift app.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <dlfcn.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/proc.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

// Metal's C entry point; the device it returns is the one MoltenVK renders with.
extern "C" void* MTLCreateSystemDefaultDevice(void);

// The Vulkan backend's running totals (graphics/vulkan/command_processor.cpp): draws, draws
// that reached the host, and {submits, submit us, blocking fence waits, wait us}.
namespace rex::graphics::vulkan {
uint64_t DrawCounter();
uint64_t HostDrawCounter();
void SubmissionCounters(uint64_t out[4]);
}  // namespace rex::graphics::vulkan

// MoltenVK's performance tracker (MVK_CONFIG_PERFORMANCE_TRACKING), laid out as
// MoltenVK/API/mvk_private_api.h at the SDK's MoltenVK commit (MVK_PRIVATE_API_VERSION 43).
// MoltenVK reports the size it expects, and the statistics are only read when it matches.
struct MvkTracker {
  uint32_t count;
  double latest, previous, average, minimum, maximum;
};
struct MvkPerformanceStatistics {
  MvkTracker hash_shader_code, spirv_to_msl, msl_compile, msl_load, msl_compress, msl_decompress,
      shader_library_from_cache, function_retrieval, function_specialization, pipeline_compile,
      glsl_to_spirv;
  MvkTracker size_pipeline_cache, write_pipeline_cache, read_pipeline_cache;
  MvkTracker retrieve_command_buffer, command_buffer_encoding, wait_submit_command_buffers,
      submit_command_buffers, command_buffer_execution, retrieve_drawable,
      wait_present_swapchains, present_swapchains, frame_interval;
  MvkTracker gpu_memory_allocated;
};
// Not an exported symbol of the static MoltenVK: an instance entry point, from
// vkGetInstanceProcAddr.
using PfnGetPerformanceStatisticsMVK = VkResult (*)(VkDevice device,
                                                    MvkPerformanceStatistics* perf,
                                                    size_t* perf_size);

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
  std::chrono::steady_clock::time_point last_report_at;
  // The main thread, for the freeze watchdog: its last display tick and where its stack lies.
  mach_port_t main_mach_thread = MACH_PORT_NULL;
  uintptr_t main_stack_low = 0, main_stack_high = 0;
  std::atomic<int64_t> last_tick_ms{0};
  // Off screen (app in the background): graphics stopped, display ticks not expected.
  std::atomic<bool> paused{false};
  std::mutex pause_mutex;
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
// GPU allocations is Metal's own count for this process (textures, buffers, render targets);
// the rest of "used" is CPU-side (guest memory, decoded data, code).
uint64_t GpuAllocatedBytes() {
  static void* device = MTLCreateSystemDefaultDevice();
  if (!device) return 0;
  return reinterpret_cast<uint64_t (*)(void*, SEL)>(objc_msgSend)(
      device, sel_registerName("currentAllocatedSize"));
}

void LogMemory() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  const bool ok = task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info),
                            &count) == KERN_SUCCESS;
  P("memory: %llu MB used (GPU allocations %llu MB), %llu MB still available to the app",
    ok ? static_cast<unsigned long long>(info.phys_footprint >> 20) : 0ull,
    static_cast<unsigned long long>(GpuAllocatedBytes() >> 20),
    static_cast<unsigned long long>(os_proc_available_memory() >> 20));
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Where the main thread is while it does not respond: it is stopped for a moment, its return
// addresses are read from its frame chain, and it continues. Nothing is allocated or locked
// while it is stopped (it may hold the malloc or log lock); the names are looked up after.
void LogMainThreadStack() {
  const mach_port_t thread = g_game->main_mach_thread;
  if (thread == MACH_PORT_NULL) return;
  constexpr int kMaxFrames = 24;
  uintptr_t pcs[kMaxFrames];
  int frames = 0;
  // Return addresses of system code can carry a pointer signature in the top bits.
  auto strip = [](uint64_t address) { return static_cast<uintptr_t>(address & 0x0000000FFFFFFFFFull); };
  if (thread_suspend(thread) != KERN_SUCCESS) return;
  arm_thread_state64_t state{};
  mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
  if (thread_get_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                       &count) == KERN_SUCCESS) {
    pcs[frames++] = strip(arm_thread_state64_get_pc(state));
    pcs[frames++] = strip(arm_thread_state64_get_lr(state));
    uintptr_t fp = strip(arm_thread_state64_get_fp(state));
    while (frames < kMaxFrames && fp >= g_game->main_stack_low &&
           fp + 2 * sizeof(uintptr_t) <= g_game->main_stack_high && (fp & 7) == 0) {
      const auto* frame = reinterpret_cast<const uintptr_t*>(fp);
      const uintptr_t next = frame[0];
      const uintptr_t ret = strip(frame[1]);
      if (ret == 0) break;
      pcs[frames++] = ret;
      if (next <= fp) break;
      fp = next;
    }
  }
  thread_resume(thread);
  for (int i = 0; i < frames; ++i) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(pcs[i]), &info) && info.dli_fname) {
      const char* image = std::strrchr(info.dli_fname, '/');
      image = image ? image + 1 : info.dli_fname;
      const auto offset = static_cast<unsigned long long>(
          pcs[i] - reinterpret_cast<uintptr_t>(info.dli_fbase));
      if (info.dli_sname) {
        P("  main %2d: %s+0x%llx %s+%llu", i, image, offset, info.dli_sname,
          static_cast<unsigned long long>(pcs[i] - reinterpret_cast<uintptr_t>(info.dli_saddr)));
      } else {
        P("  main %2d: %s+0x%llx", i, image, offset);
      }
    } else {
      P("  main %2d: 0x%llx", i, static_cast<unsigned long long>(pcs[i]));
    }
  }
}

// The main thread presents frames and runs the display tick. When its ticks stop for 2 s while
// the app is on screen, this says so (again at 10 s, then every 30 s) with its stack and memory,
// and how long the freeze lasted once it responds again.
void WatchMainThread() {
  bool frozen = false;
  int64_t frozen_from_ms = 0;
  int64_t next_report_s = 2;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int64_t last = g_game->last_tick_ms.load();
    if (last == 0 || g_game->paused.load()) {
      frozen = false;
      continue;
    }
    const int64_t stale_ms = NowMs() - last;
    if (stale_ms >= 2000) {
      if (!frozen) {
        frozen = true;
        frozen_from_ms = last;
        next_report_s = 2;
      }
      if (stale_ms / 1000 >= next_report_s) {
        P("main thread not responding for %lld s; its stack:", static_cast<long long>(stale_ms / 1000));
        LogMainThreadStack();
        LogMemory();
        next_report_s = next_report_s < 10 ? 10 : next_report_s + 30;
      }
    } else if (frozen) {
      frozen = false;
      P("main thread responding again after a %.1f s freeze",
        static_cast<double>(last - frozen_from_ms) / 1000.0);
    }
  }
}

// Where each frame's time goes, between two 5-second reports. Three views:
//   frame work: the Vulkan backend's own counters, per frame shown on screen;
//   Metal: MoltenVK's tracker - GPU run time of each command buffer, encoding, shader compiles;
//   cpu: the busiest threads, so a CPU-bound thread (the GPU command thread translating the
//        game's commands, a guest thread) shows next to a GPU-bound frame.
struct FrameWorkSample {
  uint64_t draws = 0, host_draws = 0;
  // SDK patch 11 (gpu_stats.h): texture loads, resolves, render target transfers so far.
  int64_t texture_loads = 0, resolves = 0, transfers = 0;
  uint64_t submissions[4] = {};
  MvkPerformanceStatistics mvk{};
  bool mvk_valid = false;
  // Thread id -> CPU time so far (us), and the thread's name.
  std::unordered_map<uint64_t, std::pair<uint64_t, std::string>> threads;
};
FrameWorkSample g_last_work;

rex::ui::vulkan::VulkanProvider* GameVkProvider() {
  auto* graphics = g_game && g_game->runtime ? g_game->runtime->graphics_system() : nullptr;
  return graphics ? static_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider()) : nullptr;
}

VkDevice GameVkDevice() {
  auto* provider = GameVkProvider();
  return provider && provider->vulkan_device() ? provider->vulkan_device()->device()
                                               : VK_NULL_HANDLE;
}

PfnGetPerformanceStatisticsMVK GetPerformanceStatisticsMVK() {
  static PfnGetPerformanceStatisticsMVK function = nullptr;
  static bool looked_up = false;
  if (looked_up) return function;
  auto* provider = GameVkProvider();
  if (!provider || !provider->vulkan_instance()) return nullptr;
  looked_up = true;
  const auto* instance = provider->vulkan_instance();
  function = reinterpret_cast<PfnGetPerformanceStatisticsMVK>(
      instance->functions().vkGetInstanceProcAddr(instance->instance(),
                                                  "vkGetPerformanceStatisticsMVK"));
  if (!function) P("Metal: MoltenVK has no vkGetPerformanceStatisticsMVK; no Metal lines");
  return function;
}

void SampleThreads(FrameWorkSample& sample) {
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t thread_count = 0;
  if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS) return;
  for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
    // Both from the kernel through the thread's port, so a thread that ends meanwhile only fails
    // the call (its pthread name through pthread_getname_np could be freed memory by then).
    thread_extended_info_data_t extended{};
    mach_msg_type_number_t extended_count = THREAD_EXTENDED_INFO_COUNT;
    thread_identifier_info_data_t ident{};
    mach_msg_type_number_t ident_count = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&extended),
                    &extended_count) == KERN_SUCCESS &&
        thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&ident),
                    &ident_count) == KERN_SUCCESS) {
      const uint64_t cpu_us = (extended.pth_user_time + extended.pth_system_time) / 1000u;
      extended.pth_name[sizeof(extended.pth_name) - 1] = '\0';
      sample.threads[ident.thread_id] = {cpu_us,
                                         extended.pth_name[0] ? extended.pth_name : "unnamed"};
    }
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                thread_count * sizeof(thread_act_t));
}

FrameWorkSample TakeFrameWorkSample() {
  FrameWorkSample sample;
  sample.draws = rex::graphics::vulkan::DrawCounter();
  sample.host_draws = rex::graphics::vulkan::HostDrawCounter();
  rex::graphics::vulkan::SubmissionCounters(sample.submissions);
  namespace stats = rex::graphics::stats;
  sample.texture_loads = stats::Get(stats::kTextureLoads);
  sample.resolves = stats::Get(stats::kResolves);
  sample.transfers = stats::Get(stats::kRenderTargetTransfers);
  VkDevice device = GameVkDevice();
  PfnGetPerformanceStatisticsMVK get_statistics = device ? GetPerformanceStatisticsMVK() : nullptr;
  if (get_statistics) {
    size_t expected = 0;
    get_statistics(device, nullptr, &expected);
    if (expected == sizeof(sample.mvk)) {
      size_t size = sizeof(sample.mvk);
      sample.mvk_valid = get_statistics(device, &sample.mvk, &size) == VK_SUCCESS;
    } else {
      static bool reported = false;
      if (!reported) {
        reported = true;
        P("Metal: MoltenVK statistics are %zu bytes, this build expects %zu; not read", expected,
          sizeof(sample.mvk));
      }
    }
  }
  SampleThreads(sample);
  return sample;
}

// Activities and their average duration between two samples (MoltenVK keeps lifetime averages).
struct TrackerDelta {
  uint32_t count;
  double average_ms;
};
TrackerDelta Delta(const MvkTracker& before, const MvkTracker& after) {
  const uint32_t count = after.count - before.count;
  if (count == 0) return {0, 0.0};
  return {count, (after.average * after.count - before.average * before.count) / count};
}

void LogFrameWork(const FrameWorkSample& before, const FrameWorkSample& after, double seconds,
                  uint64_t frames) {
  const double per_frame = frames ? 1.0 / double(frames) : 0.0;
  const uint64_t submits = after.submissions[0] - before.submissions[0];
  const uint64_t waits = after.submissions[2] - before.submissions[2];
  P("frame work: %llu frames; per frame %.0f draws (%.0f reached the GPU), %.1f submits "
    "(%.1f ms CPU), %.1f blocking GPU waits (%.1f ms)",
    static_cast<unsigned long long>(frames), double(after.draws - before.draws) * per_frame,
    double(after.host_draws - before.host_draws) * per_frame, double(submits) * per_frame,
    double(after.submissions[1] - before.submissions[1]) / 1000.0 * per_frame,
    double(waits) * per_frame,
    double(after.submissions[3] - before.submissions[3]) / 1000.0 * per_frame);
  P("frame work: per frame %.1f texture loads, %.1f resolves (EDRAM copies), %.1f render "
    "target transfers",
    double(after.texture_loads - before.texture_loads) * per_frame,
    double(after.resolves - before.resolves) * per_frame,
    double(after.transfers - before.transfers) * per_frame);
  // What the GPU memory holds (the rest of "GPU allocations" is the 512 MB guest memory buffer,
  // the EDRAM buffer, upload buffers and MoltenVK's own).
  namespace stats = rex::graphics::stats;
  constexpr double kMB = 1024.0 * 1024.0;
  P("gpu memory: textures %.0f MB in %lld (allocator holds %.0f MB in %lld blocks, %.0f MB used), "
    "render targets %.0f MB in %lld",
    double(stats::Get(stats::kTextureBytes)) / kMB,
    static_cast<long long>(stats::Get(stats::kTextureCount)),
    double(stats::Get(stats::kTextureAllocatorBlockBytes)) / kMB,
    static_cast<long long>(stats::Get(stats::kTextureAllocatorBlocks)),
    double(stats::Get(stats::kTextureAllocatorAllocationBytes)) / kMB,
    double(stats::Get(stats::kRenderTargetBytes)) / kMB,
    static_cast<long long>(stats::Get(stats::kRenderTargetCount)));
  // The six guest texture formats taking the most memory.
  std::vector<std::pair<int64_t, uint32_t>> formats;
  for (uint32_t format = 0; format < 64; ++format) {
    const int64_t bytes = stats::texture_format_bytes[format].load(std::memory_order_relaxed);
    if (bytes > 0) formats.emplace_back(bytes, format);
  }
  std::sort(formats.begin(), formats.end(), [](const auto& a, const auto& b) { return a > b; });
  std::string by_format = "gpu memory by texture format:";
  for (size_t i = 0; i < formats.size() && i < 6; ++i) {
    const auto* info = rex::graphics::FormatInfo::Get(formats[i].second);
    char part[96];
    snprintf(part, sizeof(part), "%s %s %.0f MB (%lld)", i ? "," : "",
             info && info->name ? info->name : "?", double(formats[i].first) / kMB,
             static_cast<long long>(
                 stats::texture_format_count[formats[i].second].load(std::memory_order_relaxed)));
    by_format += part;
  }
  P("%s", by_format.c_str());
  if (before.mvk_valid && after.mvk_valid) {
    const TrackerDelta run = Delta(before.mvk.command_buffer_execution,
                                   after.mvk.command_buffer_execution);
    const TrackerDelta encode = Delta(before.mvk.command_buffer_encoding,
                                      after.mvk.command_buffer_encoding);
    const TrackerDelta msl = Delta(before.mvk.msl_compile, after.mvk.msl_compile);
    const TrackerDelta pipelines = Delta(before.mvk.pipeline_compile, after.mvk.pipeline_compile);
    const TrackerDelta drawable = Delta(before.mvk.retrieve_drawable, after.mvk.retrieve_drawable);
    // GPU busy: the summed run time of the command buffers over the wall time (it can pass 100%
    // when the GPU overlaps them).
    P("Metal: %.1f command buffers per frame, GPU run %.1f ms each (GPU busy %.0f%%), "
      "encoding %.1f ms each; drawable wait %.1f ms; compiled %u shaders (%.0f ms each), "
      "%u pipelines (%.0f ms each); GPU memory %.0f MB",
      double(run.count) * per_frame, run.average_ms,
      seconds > 0 ? run.average_ms * run.count / (seconds * 10.0) : 0.0, encode.average_ms,
      drawable.average_ms, msl.count, msl.average_ms, pipelines.count, pipelines.average_ms,
      after.mvk.gpu_memory_allocated.latest / 1024.0);
  }
  // The five busiest threads over the interval.
  std::vector<std::pair<double, std::string>> busy;
  for (const auto& [id, now] : after.threads) {
    auto it = before.threads.find(id);
    const uint64_t used_us = now.first - (it != before.threads.end() ? it->second.first : 0);
    if (seconds > 0) busy.emplace_back(double(used_us) / (seconds * 10000.0), now.second);
  }
  std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string line = "cpu:";
  for (size_t i = 0; i < busy.size() && i < 5; ++i) {
    char part[96];
    snprintf(part, sizeof(part), "%s %s %.0f%%", i ? "," : "", busy[i].second.c_str(),
             busy[i].first);
    line += part;
  }
  P("%s", line.c_str());
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
    // MoltenVK reads its configuration when the Vulkan instance is created: time each Metal
    // command buffer on the GPU and each shader compile, for the "Metal:" log lines.
    setenv("MVK_CONFIG_PERFORMANCE_TRACKING", "1", 1);
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
  g_game->last_report_at = g_game->resumed_at;
  g_last_work = TakeFrameWorkSample();
  P("PASS main guest thread resumed%s", stage >= 5 ? " (with graphics)" : " (no graphics: the "
    "game is expected to stop at its video setup)");
  // mcla_game_start runs on the main thread.
  g_game->main_mach_thread = mach_thread_self();
  const pthread_t main_pthread = pthread_self();
  g_game->main_stack_high = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(main_pthread));
  g_game->main_stack_low = g_game->main_stack_high - pthread_get_stacksize_np(main_pthread);
  std::thread(WatchMainThread).detach();
  return 0;
}

extern "C" void mcla_game_tick(void) {
  if (!g_game || !g_game->main_thread || g_game->resumed_at.time_since_epoch().count() == 0) {
    return;
  }
  g_game->last_tick_ms = NowMs();
  const auto now = std::chrono::steady_clock::now();
  const int second = static_cast<int>(
      std::chrono::duration_cast<std::chrono::seconds>(now - g_game->resumed_at).count());
  if (second <= g_game->last_alive_second) return;
  g_game->last_alive_second = second;
  if (second <= 30 || second % 30 == 0) P("alive: %d s", second);
  if (g_game->window && second % 5 == 0) {
    // Over the real time since the last report: after a freeze that is longer than 5 s.
    const uint64_t paints = g_game->window->paints();
    const double seconds =
        std::chrono::duration<double>(now - g_game->last_report_at).count();
    P("display: %.1f fps over the last %.0f s (presenter paints)",
      seconds > 0 ? static_cast<double>(paints - g_game->paints_at_last_report) / seconds : 0.0,
      seconds);
    const uint64_t frames = paints - g_game->paints_at_last_report;
    g_game->paints_at_last_report = paints;
    g_game->last_report_at = now;
    LogMemory();
    FrameWorkSample work = TakeFrameWorkSample();
    LogFrameWork(g_last_work, work, seconds, frames);
    g_last_work = std::move(work);
  }
  if (g_game->window && (second == 60 || second == 300)) ListCache("now");
  // Twice, a few seconds apart: which threads exist, and whether the set changes.
  if (second == 4 || second == 8) ListGuestThreads();
}

extern "C" void mcla_game_resize(unsigned width_px, unsigned height_px) {
  if (g_game && g_game->window) g_game->window->Resize(width_px, height_px);
}

extern "C" void mcla_game_set_paused(int paused) {
  if (!g_game || !g_game->runtime || g_game->resumed_at.time_since_epoch().count() == 0) return;
  std::lock_guard<std::mutex> lock(g_game->pause_mutex);
  if (g_game->paused.load() == (paused != 0)) return;
  // The Xenos plugin's system (rex_gpu_create) is a GraphicsSystem.
  auto* graphics = static_cast<rex::graphics::GraphicsSystem*>(g_game->runtime->graphics_system());
  const int64_t start = NowMs();
  if (paused) {
    g_game->paused = true;
    if (g_game->window) g_game->window->SetPaused(true);
    // Waits for the command processor to finish the command it is on.
    if (graphics) graphics->Pause();
    P("paused: the app left the screen; graphics stopped after %lld ms",
      static_cast<long long>(NowMs() - start));
  } else {
    if (graphics) graphics->Resume();
    if (g_game->window) g_game->window->SetPaused(false);
    g_game->last_tick_ms = NowMs();
    g_game->paused = false;
    P("resumed: the app is back on screen");
  }
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

extern "C" void mcla_game_set_paused(int) {}

#endif  // MCLA_HAS_GAME
