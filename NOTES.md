# MCLA iOS/iPadOS port: study notes and feasibility

Target: iPad Pro 11" 2018 (A12X, 4 GB RAM), iPadOS 26.x, sideloaded with iloader, no Mac, no JIT.
Status of everything below: **read from source or public READMEs, not run on a device.** Items marked
(verify) are from memory or inference and must be checked before they drive a decision.

## 1. How MCLA is built (this repo)

- The game's PowerPC code is translated to C++ at build time by the ReXGlue SDK (v0.10.0, BSD-3, a
  Xenia derivative). That output lands in `generated/` and is **derived from the user's game binary**.
  `.gitignore` already excludes it (`/generated/*`), and it is ~125 MB on the PC build.
- This repo itself holds only: the manifest and function hints (`config/`, `mcla_manifest.toml`), a small
  host app (`src/`, ~13 files), SDK patches (`patches/`), and build scripts (`scripts/`, `ps5/`).
- The SDK supplies the Xbox 360 kernel, GPU (Xenos) and audio reimplementation, so everything below
  about portability is really about the SDK.
- Because the code is compiled ahead of time to native code, there is no JIT. The same property the GTA V
  port relies on. This is the good news.

## 2. What is portable and what is not (SDK v0.10.0)

| Area | Finding | iOS impact |
|---|---|---|
| Renderers | `src/graphics/d3d12` (Windows only) and `src/graphics/vulkan` | D3D12 is irrelevant. Vulkan is the only candidate. |
| macOS support | SDK has `mac-arm64` presets, `surface_mac.cpp`, `src/ui/vulkan/vulkan_moltenvk.cpp`, and a `thirdparty/moltenvk` tree | A macOS-on-MoltenVK path already exists. iOS is a different target (see below). |
| Windowing | SDL3 (`thirdparty/sdl3`, `window_sdl.cpp`) | SDL3 supports iOS, but the SDK's surface code has Windows, GNU/Linux and macOS files only. An iOS surface (CAMetalLayer) is needed. |
| Memory | `core/memory_posix.cpp` reserves and commits guest address space with mmap/mprotect | iOS allows PROT_NONE reservations but a large reservation plus commits must fit the per-app limit (verify). |
| Fibers | `core/fiber_posix.cpp` uses makecontext/swapcontext | These are marked prohibited in the iOS SDK (verify in CI). Needs a replacement: hand-written ARM64 context switch, or one thread per fiber. |
| Fault handling | `core/exception_handler_posix.cpp` uses signal handlers (SIGSEGV, plus SIGBUS on Mac) | In-process signal handlers work on iOS, but a debugger attach swallows them and Mach exception ports are restricted (verify). Used for guest MMIO faults. |
| `MAP_JIT` | Referenced for Mac in `memory_posix.cpp` | Must stay off; sideloaded apps cannot get the JIT entitlement. Check no recompiled path needs executable memory. |
| SIMD | `thirdparty/simde` | Recompiled code that uses x86 intrinsics maps to NEON through simde. The SDK already builds `linux-arm64`, so ARM64 codegen is at least exercised (verify on this title). |
| Audio | `src/audio` | Backend choice not yet read. Needs an iOS output (SDL3 audio or CoreAudio). |
| Input | `src/input`, SDL | SDL3 handles MFi/Xbox/PS controllers on iOS. Touch controls are new work. |
| FFmpeg | `thirdparty/ffmpeg-overlay` | Used for audio/video decode. Needs an iOS static build. |
| Windows-only host code | `CMakeLists.txt` has `add_executable(mcla WIN32 ...)`; `src/` has Windows-flavoured pieces (crash trace, sampling profiler) | Needs `#if` guards or exclusion for iOS. Not yet audited file by file. |

## 3. Renderer decision: MoltenVK vs. new Metal backend (recommendation pending your OK)

What the Vulkan backend requires (from `src/graphics/vulkan` and `src/ui/vulkan/vulkan_device.cpp`):

- `vertexPipelineStoresAndAtomics` and `fragmentStoresAndAtomics`: always required (memexport and the
  render-target paths).
- `geometryShader`: preferred. The SDK already has `vulkan_require_geometry_shader`, defaulting **off on
  macOS** with a comment that MoltenVK does not expose it, plus "fallback primitive emulation paths".
- `fillModeNonSolid`: same treatment, optional on macOS.
- `fragmentShaderSampleInterlock` (VK_EXT_fragment_shader_interlock): used by the render-target cache for
  Xenos EDRAM emulation. The alternative path uses render passes / dynamic rendering.
- Others seen: `tessellationShader`, `fullDrawIndexUint32`, `independentBlend`, `depthClamp`,
  `sampleRateShading`, `shaderClipDistance`/`shaderCullDistance`, `imageCubeArray`, `textureCompressionBC`,
  `samplerAnisotropy`, VK_KHR_dynamic_rendering, VK_EXT_custom_border_color, VK_EXT_extended_dynamic_state.

Assessment:

- The macOS MoltenVK work in the SDK already handles the biggest known gap (no geometry shaders).
  That makes **MoltenVK the likely cheaper route**: no new GPU backend, only an iOS surface, a static
  MoltenVK build, and fixes for whatever the A12X's Metal feature set lacks.
- Unknown and decisive, to verify against MoltenVK's documented feature tables for iOS / Apple GPU
  family 5: `textureCompressionBC` (Apple GPUs before the M-series/A15 lack BC; the A12X does not support
  it in hardware, so the texture cache would need to **decode BC to RGBA at upload**, costing memory
  and time), `fragmentShaderSampleInterlock` on A12X, `tessellationShader`, 32-bit indices,
  `independentBlend`, and the argument-buffer/descriptor limits.
- A new native Metal backend (what the GTA V port did) would avoid the Vulkan layer cost but means
  rewriting the Xenos command processor, texture cache and render-target cache: months of work. Not
  recommended unless MoltenVK fails the feature check.

**Recommendation:** MoltenVK first, with a feature-check milestone that logs exactly which features are
missing on the iPad before any rendering work. Not built, not tested.

## 4. The blocking build/legal question (needs the owner's decision)

The recompiled code in `generated/` comes from the game binary. An `.ipa` that contains it is therefore
**derived from the game**, even though no raw assets are in it. Your rule: ISO, extracted data and anything
derived stay local and never go into a repo, release or artifact. A plain GitHub Actions macOS build of
this repo cannot produce a playable `.ipa` without that generated code, and I cannot generate it on the
runner without the ISO. Options:

1. **Private-repo build, like the GTA V port.** The template there tells users to use a private repo, give
   the runner a Tailscale key, and let the build machine pull the user's own files from their PC at build
   time. The resulting `.ipa` is a private artifact of your own repo. This breaks the literal "never in
   any artifact" rule, so it needs your explicit OK.
2. **Ship the app without the game code and load it at runtime.** Not possible: iOS forbids loading
   unsigned native code at runtime, and there is no JIT.
3. **Build the `.ipa` on your PC.** Needs the iOS SDK and Apple's linker, which only exist on macOS. Not
   possible on Windows.
4. **Rent a Mac in the cloud for builds.** Same privacy question as option 1, with a third party.

Nothing is built until you choose.

## 5. gta5-ios-port: what it actually is

- Public copies exist (`uxsmr/gta5-ios-port`, 1 star, created 2026-10-11, license NOASSERTION; the README
  cites `kevinerikjs/gta5-ios-port`, which returns 404 now). Several look-alike repos exist too
  (`gta5-ios-port-audited`, `gta5-ios-port-v2`). I did not run anything from them. I only read the README
  and file tree as data, and I cannot vouch for any of them. Use as a structural reference only.
- It runs a **WebAssembly** build of GTA V translated by **wasm2c** to C and compiled to ARM64. That is a
  different pipeline from MCLA's PowerPC-to-C++, but the same no-JIT principle.
- Graphics: its **own Metal renderer** consuming a D3D11-style command stream; shaders translated to Metal
  at build time (Tint-based tool) and precompiled into the app. MCLA's equivalent would be Xenos
  microcode to SPIR-V (already in the SDK) then SPIRV-Cross/MoltenVK to MSL, either ahead of time or at
  runtime (runtime Metal shader compilation from source is allowed on iOS; only code generation is not).
- Packaging: a Swift/UIKit app (XcodeGen project), entitlements files (`Free.entitlements`,
  `Full.entitlements`), a GitHub Actions workflow on a macOS runner, game files streamed or copied in.
- Memory: its own README says free Apple IDs get a smaller memory budget, and the "Increased Memory
  Limit" and "Extended Virtual Addressing" entitlements need a **paid** developer account.

### 5a. From the owner's static analysis of GTA5Port v1.4.2 (`GTA5Port-analysis.md`, not run)

- Custom Metal renderer fed by a graphics-command ring; links Metal, MetalKit, **MetalFX**.
- **Precompiled shaders** (75 MB `shaders-iphoneos.metallib` plus reflection data). For MCLA through MoltenVK,
  SPIR-V to MSL conversion happens at run time, so a persistent pipeline/shader cache matters to avoid stutter.
- **BC textures transcoded to ASTC** (`PGBC1BlockToASTC`, `PGDecodeBC`): same plan as MCLA (A12X has no BC).
- Memory-budget functions, texture mip reduction, normal vs increased-memory modes.

### 5b. Upscaling options (assumed until measured)

- MetalFX, measured on the A12X iPad: **spatial upscaling supported, temporal not** (my A14+ guess was wrong).
  Spatial MetalFX on the presenter's final image is an option; it would need a Metal-side pass after MoltenVK.
- The SDK presenter already has FSR 1 and CAS paint effects (Vulkan compute), which do not depend on MetalFX.

## 6. iloader (nab138/iloader, MIT, ~4.3k stars, pushed 2026-10-22)

- Official sources are only the GitHub repo and iloader.app (per its README). Needs iTunes on Windows for
  usbmuxd, Apple ID sign-in, then "Import any IPA".
- Free Apple ID limits (general Apple behaviour, verify for iPadOS 26): 7-day signature, 3 active
  sideloaded apps, no extra capabilities such as increased-memory-limit.
- An unsigned `.ipa` is fine; the signing tool re-signs it. The `.ipa` must be `Payload/<App>.app/` with a
  valid `Info.plist` and a real arm64 executable.

## 7. Memory budget (plan, not measured)

- Device: 4 GB RAM (3786 MB reported). **Measured on the iPad (free Apple ID, nothing else open):
  `os_proc_available_memory` = 2862 MB, recommended working set 2730 MB.** That budget can shrink if other
  apps are open or the system is under pressure, so plan for a ceiling of about 2.2 GB and re-measure.
- Guest memory: Xbox 360 had 512 MB. Commit lazily; never touch the whole guest range.
- Suggested starting budgets within a ~1.5 GB ceiling: guest RAM up to ~512 MB, texture cache
  ~300 MB, render targets/EDRAM emulation at native 720p ~150 MB, shader/pipeline cache ~100 MB,
  audio/host ~100 MB, headroom ~300 MB.
- Rendering defaults: 720p internal, 30 FPS cap, no resolution scaling, motion blur and DoF off.
- If measured available memory is under ~1.2 GB, this target is not realistic without the paid-account
  entitlements. I will say so rather than keep building.

## 8. Proposed milestones

1. **M0**: this document, owner decides on section 4.
2. **M1**: iOS app target (XcodeGen + CI), launches, Files-app import of game folder, log viewer with
   share, prints memory limit and Metal/Vulkan feature report, reaches the recompiled entry point.
3. **M2**: unsigned `.ipa` from GitHub Actions + iloader install steps.
4. **M3**: PC prep script (Python) that validates and arranges extracted files. No upload.
5. **M4**: rendering through MoltenVK in stages (clear, geometry, textures, shaders, lighting).
6. **M5**: audio, then touch and controllers, then A12X tuning.

## 9. Measured on the A12X iPad (M1a, on device)

| Item | Measured | Consequence |
|---|---|---|
| Per-app memory | 2862 MB available | Fits the planned budgets with headroom. |
| BC texture compression | **false** | Xenos textures (DXT/BC) must be decoded to a supported format at upload: roughly 4x to 8x the memory for BC1/BC3 if decoded to RGBA8. The biggest memory risk; size the texture cache with this in mind. |
| Argument buffers | raw value 0 = **MTLArgumentBuffersTier1** (corrected) | Supported at tier 1: MoltenVK logged "Descriptor sets binding resources using Metal argument buffers" on device. Per-stage limits still apply (16 samplers, 96 sampled images per stage reported by MoltenVK). |
| Raster order groups | true | Makes VK_EXT_fragment_shader_interlock plausible through MoltenVK; verify in the MoltenVK feature report. |
| 32-bit float filtering | false | Linear filtering of RGBA32F textures is unavailable; emulate or accept nearest. |
| GPU family | apple5 (not apple6) | No apple6+ features. |

## 10. MoltenVK and SDK on the A12X iPad (M1b probe, on device, 2026-10-25)

MoltenVK 1.4 (instance API 1.4.357, device API 1.3), 124 device extensions, `vkCreateDevice` with every reported
feature: **OK**.

| Item | Measured | Consequence |
|---|---|---|
| vertexPipelineStoresAndAtomics, fragmentStoresAndAtomics | yes | Renderer's hard requirements met. |
| fragmentShaderSampleInterlock / PixelInterlock | yes / yes | The interlock (ROV-style) EDRAM path is available. |
| geometryShader | NO | Use the SDK's macOS fallback (`vulkan_require_geometry_shader` off). |
| tessellationShader, fillModeNonSolid, dynamicRendering, extendedDynamicState | yes | |
| customBorderColors | NO | Border colours limited to the fixed Vulkan set; check the renderer's fallback. |
| shaderCullDistance, logicOp, wideLines, shaderFloat64 | NO | Minor; check renderer use. |
| portability: samplerMipLodBias | **NO** | Xenos LOD bias must be applied in shaders. |
| portability: pointPolygons, tessellationIsolines | NO | Minor. |
| Per-stage limits | samplers 16, sampled images 96, storage buffers 31, resources 127 | **16 samplers per stage** must fit the renderer's layout (Xenos has up to 32 fetch constants). |
| BC1/2/3/5 | unsupported | Decode at upload (or transcode to ASTC/ETC2: sample+linear OK). |
| D24_UNORM_S8_UINT | unsupported | Use D32_SFLOAT_S8_UINT (supported) with 24-bit depth emulation. |
| R32G32B32A32_SFLOAT | sample yes, linear NO | Matches the Metal report. |
| Memory heap | 3786 MB, one device-local heap | Unified memory. |

SDK on device:

| Item | Measured | Consequence |
|---|---|---|
| AllocFixed / Protect / QueryProtect | OK (page size 16 KB) | iOS patch 03 (`vm_region_64`) works. |
| `shm_open` | **EPERM** | SDK's file mapping fails on iOS; patch 04 uses an unlinked temp file (verified on device). |
| 4.5 GB contiguous reservation + 9 guest views + aliasing | **OK** (temp-file backing) | Guest memory layout works with a free Apple ID; no extended-VA entitlement needed. Touching 256 MB did not lower `os_proc_available_memory` (file-backed). |
| SDK fault handler | OK (write, exact address, retry correct) | Guest MMIO trapping works. |
| SDK fibers (ucontext) | `ConvertCurrentThread` fails | Only used by the `rexcrt_CreateFiber` hook; MCLA's generated code, config and source reference no `rexcrt_` hook, so deferred. Needs an ARM64 context switch if a title uses it. |
