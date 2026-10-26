# DEVLOG: MCLA iPad port

Legend: **[device]** verified on the iPad, **[pc]** verified on the Windows PC, **[read]** read from source or docs, **[assumed]** not verified.

## 2026-10-25: Study (M0)

- [pc] Windows build of upstream `holdmysocks/mcla-recomp` installed to `C:\MCLA`; plays at 720p/30 FPS
  after moving it from a hard drive to the SSD. Heat is a likely cause of late-session stutter on that laptop;
  not proven.
- [read] ReXGlue SDK v0.10.0 already has a macOS MoltenVK path and `linux-arm64` builds. No iOS target.
- [read] Vulkan renderer feature list recorded in `NOTES.md`; the A12X feature check is still open.
- [read] Blocking issue: a CI-built `.ipa` needs generated code derived from the game (NOTES.md section 4).
- Nothing built for iOS. Nothing run on the iPad.

## Not working / unknown

- Everything iOS. No iOS target exists yet.

## 2026-10-25: M1a skeleton app and CI

- [read] Decision: private-repo build for the playable app (game-derived code stays in the private repo's runner
  and artifact). Public fork holds code only.
- [CI] `ios/` skeleton builds on a macos-15 runner and produces `MCLA-unsigned.ipa` with an arm64 Mach-O
  executable (157 KB). First run reported success but had a compile error hidden by a pipe; fixed with
  `pipefail` and an executable check, second run is genuine.
- [assumed] App launches, log viewer, folder picker and device report work. **Not run on the iPad yet.**
- Next: iPad test of M1a (see `ios/README.md`), then M1b: private repo, link the SDK and recompiled code,
  reach the entry point.

## 2026-10-25: M1a on the iPad

- [device] Skeleton installs with iloader and runs on iPad8,1 (iPad Pro 11" 2018), iPadOS 26.7.1, free Apple ID.
- [device] physical memory 3786 MB. `os_proc_available_memory` 2862 MB at launch. Better than the 1.5 to 2 GB guess.
- [device] Metal: Apple A12X GPU, family apple4/apple5 true, apple6/7 false. Unified memory. Max buffer 2048 MB.
  Recommended working set 2730 MB. Read-write texture tier 2. Raster order groups true.
- [device] **Argument buffers tier 0**, **BC texture compression false**, **32-bit float filtering false**.
- Bug seen in the screenshot: every log line was shown twice in the viewer (the file log was not checked).
  Fixed by rebuilding the viewer text from the log; fix not yet re-tested on device.
- Not yet tested on device: Import game folder, Share log.

## 2026-10-25: M1a import and share verified on device

- [device] Viewer shows each log line once (duplicate fix confirmed).
- [device] **Share log works**: `mcla.log` exported from the iPad and read on the PC.
- [device] **App Documents folder works**: after putting `default.xex` into On My iPad > MCLA (the app's own
  Documents folder, via LocalSend + Files), the log reported `1 files, 8 MB, default.xex present: true`.
- [device] Files-app folder picker: first attempt had every item greyed out (files are greyed because the picker
  only accepts folders; one test stopped at the Downloads folder, which is in iCloud with an upload error).
  The folder bookmark path (`saved game folder: ...`) is **not verified**. The Documents folder route is the
  one to use for now.
- Decision: game files live in the app's Documents folder (On My iPad > MCLA). The PC prep script (M3) will
  arrange files for that layout.
- Privacy note: do not stage game files in iCloud Drive. Use On My iPad.

## 2026-10-25: M1b step 1, cross-compiling the SDK for iOS (CI probe, no game data)

- [CI] Workflow `ios-sdk.yml` clones ReXGlue v0.10.0, applies this repo's SDK patches plus `ios/sdk/patches/*`,
  and configures for `CMAKE_SYSTEM_NAME=iOS`, arm64, iOS 16.
- [CI] Run 1 stopped at configure: the Khronos Vulkan loader's CMake cannot handle iOS. Fix (patch 01): skip the
  loader on iOS. iOS has no loader or ICD search; MoltenVK will be called directly, the way the SDK's PS5 path
  links its driver (`vk_icdGetInstanceProcAddr`). That source change in `vulkan_instance.cpp` is still to do.
- [read] Fibers (`fiber_posix.cpp`, ucontext) are only used for the guest `CreateFiber` API, not every guest
  thread. Whether iOS allows ucontext is to be shown by the compile.
- [CI] Runs 2-4: MoltenVK's CMake is macOS-only (patch `moltenvk/01`: UIKit + CoreGraphics, static library);
  SDK executables must not be bundles (`CMAKE_MACOSX_BUNDLE=OFF`); Apple install rules referenced the loader
  (patch 02).
- [CI] Run 5: **configure passes for iOS and the whole SDK compiles** apart from 28 failed steps with two causes:
  float `std::format` needs iOS 16.3+ (minimum raised to iOS 17.0 for SDK and app), and `mach_vm.h` is not
  available on iOS (patch 03: `vm_region_64`). `fiber_posix.cpp` (ucontext) compiled; whether it links and runs
  on iOS is **not verified**.
- [CI] Run 6 (40258451989): **SDK builds for iOS arm64 with 0 failures** (860 steps, including linking the
  `rexglue` executable). Only a MoltenVK warning about an iOS 18 API remains (device is on 26.7.1).
  Skeleton app also builds with the iOS 17 minimum.

## 2026-10-25: M1b step 2, device probe app (SDK + MoltenVK linked into the app)

- [CI] `ios-app.yml` builds the SDK core, a static MoltenVK and `ios/probe` for iOS arm64, merges 11 archives
  into `libmcla_probe_all.a`, and links it into the app (run 40260925234): 4.9 MB arm64 executable containing
  `mcla_probe_run`, MoltenVK and the Vulkan entry points. Two failed runs first: dependencies not built
  (fixed with a link-check executable) and archives looked for in the wrong folder (the SDK writes to `out/`).
- [assumed] The probe runs on the iPad. **Not run on the device yet.** It reports MoltenVK features, formats,
  limits and device creation; SDK page allocation; the SDK's 4.5 GB shm-backed guest arena; the SDK fault
  handler; SDK fibers.
- Log keeps the previous run (`mcla-previous.log`) so a crash during the probe is not lost.

## 2026-10-25: M1b probe on the iPad

- [device] Probe ran to completion, no crash (iPad8,1, iPadOS 26.7.1, free Apple ID). Full table in NOTES.md s.10.
- [device] MoltenVK: device creation OK; required store/atomic features and fragment shader interlock present;
  no geometry shader, no BC, no D24S8, no samplerMipLodBias, no custom border colour; 16 samplers per stage.
- [device] SDK page allocation and patch 03 OK. `shm_open` EPERM. A 4.5 GB reservation with all 9 guest views and
  correct aliasing works on a temp-file backing. SDK fault handler works. SDK fibers fail (ucontext).
- Patch 04: SDK `CreateFileMappingHandle` uses an unlinked temp file on iOS; the probe now maps the guest views
  through that SDK call.
- [device] Re-run with patch 04: `rex::memory::CreateFileMappingHandle 4.50 GB OK (patch 04, temp file)`, 4.5 GB
  reservation, all 9 views, aliasing OK, fault handler OK. Steps 1, 2 match the first run. **The SDK's guest
  memory path is verified on the iPad.**
- Fibers deferred: only reachable through the `rexcrt_CreateFiber` hook, which MCLA's generated code does not
  reference (local read-only search of `generated/`, nothing copied).
- Noted (earlier run): the Documents folder no longer had `default.xex` after this install (1 file, 0 MB). Not investigated.
- Not yet: the iOS surface (SDL/CAMetalLayer), calling MoltenVK directly, running on device.

## 2026-10-25: M4 step 1, graphics path (SDK presenter through MoltenVK into a CAMetalLayer)

- SDK patch 05: `CAMetalLayerSurface` reads its size from the layer when there is no SDL window.
- SDK patch 06: `REX_PLATFORM_IOS`; on iOS `VulkanInstance` uses MoltenVK's `vkGetInstanceProcAddr` directly.
- `ios/probe/mcla_gfx.cpp`: iOS windowed-app context + `Window` around the app's CAMetalLayer; SDK
  `VulkanProvider` (GPU-emulation requirements first), presenter and immediate drawer paint a moving bar.
  SDK log lines are forwarded to the app log. Startup report logs MetalFX support.
- [CI] Built first try (run 40263116359): 5.8 MB arm64, 12 archives merged.
- [assumed] Draws on the iPad. **Not run on the device yet.**
- [device] Graphics test: the SDK created the Vulkan instance (`VK_EXT_metal_surface`, MoltenVK direct) and the
  device (interlock, robustness2, stencil export), then **the app closed** right after MoltenVK's "Created
  VkDevice" line, inside `VulkanProvider::Create`. Reproduced twice. Probe still passes. Cause unknown: waiting
  for the iOS crash report (.ips). Device thermal state was 2 (serious) at launch.
- [device] MetalFX spatial upscaling supported, temporal not. Argument buffers are tier 1 (NOTES corrected).
- [device] Crash report decoded against the saved build (offset matches): `NSInvalidArgumentException`,
  unrecognized selector `setBorderColorMVK:` in `MVKSampler::newMTLSamplerDescriptor` <- `vkCreateSampler` <-
  SDK `UISamplers::Create`. That is a MoltenVK Objective-C category, dropped by the linker from the static
  library. Fix: link with `-ObjC`. Not an iOS or A12X limitation.
