// MCLA iOS device probe. See mcla_probe.h. No game code, no game data.
//
// Order is from safest to most likely to crash, and every step logs before it starts:
//   1. Vulkan through MoltenVK (linked statically, called directly; no loader)
//   2. SDK page allocation and protection queries (AllocFixed / Protect / QueryProtect)
//   3. Guest memory arena exactly as the SDK builds it on Apple (shm + 4.5 GB of views)
//   4. SDK fault handler (guest MMIO relies on catching access violations)
//   5. SDK fibers (ucontext; Apple marks it deprecated and iOS may not support it)

#include "mcla_probe.h"

#define VK_ENABLE_BETA_EXTENSIONS 1
#include <vulkan/vulkan.h>

#include <rex/exception_handler.h>
#include <rex/memory/utils.h>
#include <rex/thread/fiber.h>

#include <fcntl.h>
#include <os/proc.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

mcla_log_fn g_log = nullptr;

void P(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_log(buf);
}

double MB(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

// ---------------------------------------------------------------------------
// 1. Vulkan / MoltenVK
// ---------------------------------------------------------------------------

const char* YN(VkBool32 b) { return b ? "yes" : "NO"; }

void LogFormat(VkPhysicalDevice pd, VkFormat format, const char* name) {
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(pd, format, &fp);
  VkFormatFeatureFlags f = fp.optimalTilingFeatures;
  if (!f) {
    P("  format %-28s unsupported", name);
    return;
  }
  P("  format %-28s sample:%s linear:%s color:%s blend:%s depth:%s storage:%s", name,
    YN(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
    YN(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
    YN(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT),
    YN(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT),
    YN(f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT),
    YN(f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT));
}

bool Has(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const auto& e : exts) {
    if (std::strcmp(e.extensionName, name) == 0) return true;
  }
  return false;
}

void TestVulkan() {
  P("[1/5] Vulkan via MoltenVK (static, no loader)");
  uint32_t api = 0;
  vkEnumerateInstanceVersion(&api);
  P("  instance API %u.%u.%u", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
    VK_API_VERSION_PATCH(api));

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "MCLA probe";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, nullptr, &instance);
  if (r != VK_SUCCESS) {
    P("  vkCreateInstance FAILED: %d", int(r));
    return;
  }
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> pds(count);
  vkEnumeratePhysicalDevices(instance, &count, pds.data());
  P("  physical devices: %u", count);
  if (!count) {
    vkDestroyInstance(instance, nullptr);
    return;
  }
  VkPhysicalDevice pd = pds[0];

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(pd, &props);
  P("  device: %s, API %u.%u.%u, driver 0x%08x", props.deviceName,
    VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
    VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);

  uint32_t ext_count = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> exts(ext_count);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());

  // Extensions the ReXGlue Vulkan renderer uses or prefers.
  const char* wanted[] = {
      "VK_KHR_portability_subset",      "VK_KHR_swapchain",
      "VK_EXT_fragment_shader_interlock", "VK_EXT_custom_border_color",
      "VK_KHR_dynamic_rendering",       "VK_EXT_extended_dynamic_state",
      "VK_EXT_shader_stencil_export",   "VK_KHR_sampler_ycbcr_conversion",
      "VK_EXT_memory_budget",           "VK_KHR_shader_float_controls",
      "VK_EXT_subgroup_size_control",   "VK_KHR_spirv_1_4",
      "VK_EXT_4444_formats",            "VK_EXT_shader_demote_to_helper_invocation",
      "VK_KHR_push_descriptor",         "VK_KHR_imageless_framebuffer",
  };
  P("  device extensions: %u total", ext_count);
  for (const char* w : wanted) P("  ext %-44s %s", w, Has(exts, w) ? "yes" : "NO");
  std::string all;
  for (uint32_t i = 0; i < ext_count; ++i) {
    all += exts[i].extensionName;
    all += (i + 1 < ext_count) ? ", " : "";
    if (all.size() > 900 || i + 1 == ext_count) {
      P("  all ext: %s", all.c_str());
      all.clear();
    }
  }

  VkPhysicalDevicePortabilitySubsetFeaturesKHR port{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
  VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
  VkPhysicalDeviceCustomBorderColorFeaturesEXT border{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
  VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
  VkPhysicalDeviceDynamicRenderingFeatures dynr{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  // Only chain structs whose extension is present.
  void** next = &f2.pNext;
  auto chain = [&next](void* s, void** s_next) {
    *next = s;
    next = s_next;
  };
  if (Has(exts, "VK_KHR_portability_subset")) chain(&port, &port.pNext);
  if (Has(exts, "VK_EXT_fragment_shader_interlock")) chain(&interlock, &interlock.pNext);
  if (Has(exts, "VK_EXT_custom_border_color")) chain(&border, &border.pNext);
  if (Has(exts, "VK_EXT_extended_dynamic_state")) chain(&eds, &eds.pNext);
  if (Has(exts, "VK_KHR_dynamic_rendering")) chain(&dynr, &dynr.pNext);
  vkGetPhysicalDeviceFeatures2(pd, &f2);
  const VkPhysicalDeviceFeatures& f = f2.features;

  P("  core features (renderer needs the first two):");
  P("    vertexPipelineStoresAndAtomics %s, fragmentStoresAndAtomics %s",
    YN(f.vertexPipelineStoresAndAtomics), YN(f.fragmentStoresAndAtomics));
  P("    geometryShader %s, tessellationShader %s, fillModeNonSolid %s", YN(f.geometryShader),
    YN(f.tessellationShader), YN(f.fillModeNonSolid));
  P("    fullDrawIndexUint32 %s, independentBlend %s, depthClamp %s, depthBiasClamp %s",
    YN(f.fullDrawIndexUint32), YN(f.independentBlend), YN(f.depthClamp), YN(f.depthBiasClamp));
  P("    sampleRateShading %s, samplerAnisotropy %s, imageCubeArray %s, dualSrcBlend %s",
    YN(f.sampleRateShading), YN(f.samplerAnisotropy), YN(f.imageCubeArray), YN(f.dualSrcBlend));
  P("    shaderClipDistance %s, shaderCullDistance %s, logicOp %s, multiDrawIndirect %s",
    YN(f.shaderClipDistance), YN(f.shaderCullDistance), YN(f.logicOp), YN(f.multiDrawIndirect));
  P("    textureCompressionBC %s, ETC2 %s, ASTC_LDR %s", YN(f.textureCompressionBC),
    YN(f.textureCompressionETC2), YN(f.textureCompressionASTC_LDR));
  P("    shaderStorageImageWriteWithoutFormat %s, shaderInt16 %s, shaderFloat64 %s",
    YN(f.shaderStorageImageWriteWithoutFormat), YN(f.shaderInt16), YN(f.shaderFloat64));
  P("    occlusionQueryPrecise %s, largePoints %s, wideLines %s", YN(f.occlusionQueryPrecise),
    YN(f.largePoints), YN(f.wideLines));
  P("  fragmentShaderSampleInterlock %s, PixelInterlock %s, ShadingRateInterlock %s",
    YN(interlock.fragmentShaderSampleInterlock), YN(interlock.fragmentShaderPixelInterlock),
    YN(interlock.fragmentShaderShadingRateInterlock));
  P("  customBorderColors %s, extendedDynamicState %s, dynamicRendering %s",
    YN(border.customBorderColors), YN(eds.extendedDynamicState), YN(dynr.dynamicRendering));
  P("  portability subset: triangleFans %s, pointPolygons %s, events %s, samplerMipLodBias %s",
    YN(port.triangleFans), YN(port.pointPolygons), YN(port.events), YN(port.samplerMipLodBias));
  P("    imageViewFormatSwizzle %s, imageViewFormatReinterpretation %s, imageView2DOn3DImage %s",
    YN(port.imageViewFormatSwizzle), YN(port.imageViewFormatReinterpretation),
    YN(port.imageView2DOn3DImage));
  P("    constantAlphaColorBlendFactors %s, separateStencilMaskRef %s, mutableComparisonSamplers %s",
    YN(port.constantAlphaColorBlendFactors), YN(port.separateStencilMaskRef),
    YN(port.mutableComparisonSamplers));
  P("    multisampleArrayImage %s, tessellationIsolines %s, vertexAttributeAccessBeyondStride %s",
    YN(port.multisampleArrayImage), YN(port.tessellationIsolines),
    YN(port.vertexAttributeAccessBeyondStride));

  const VkPhysicalDeviceLimits& l = props.limits;
  P("  limits: image2D %u, perStage samplers %u / sampledImages %u / storageBuffers %u / "
    "storageImages %u / uniformBuffers %u / resources %u",
    l.maxImageDimension2D, l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorSampledImages,
    l.maxPerStageDescriptorStorageBuffers, l.maxPerStageDescriptorStorageImages,
    l.maxPerStageDescriptorUniformBuffers, l.maxPerStageResources);
  P("  limits: boundSets %u, setSampledImages %u, storageBufferRange %u MB, computeShared %u KB, "
    "colorAttachments %u, anisotropy %.0f, viewports %u",
    l.maxBoundDescriptorSets, l.maxDescriptorSetSampledImages,
    unsigned(l.maxStorageBufferRange / (1024 * 1024)), l.maxComputeSharedMemorySize / 1024,
    l.maxColorAttachments, double(l.maxSamplerAnisotropy), l.maxViewports);

  // Formats that matter for Xbox 360 render targets and textures.
  LogFormat(pd, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 (DXT1)");
  LogFormat(pd, VK_FORMAT_BC2_UNORM_BLOCK, "BC2 (DXT3)");
  LogFormat(pd, VK_FORMAT_BC3_UNORM_BLOCK, "BC3 (DXT5)");
  LogFormat(pd, VK_FORMAT_BC5_UNORM_BLOCK, "BC5 (DXN)");
  LogFormat(pd, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2 RGBA8");
  LogFormat(pd, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC 4x4");
  LogFormat(pd, VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM");
  LogFormat(pd, VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM");
  LogFormat(pd, VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM");
  LogFormat(pd, VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT");
  LogFormat(pd, VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM");
  LogFormat(pd, VK_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM");
  LogFormat(pd, VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT");
  LogFormat(pd, VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT");
  LogFormat(pd, VK_FORMAT_R32_UINT, "R32_UINT");
  LogFormat(pd, VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT");
  LogFormat(pd, VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT");
  LogFormat(pd, VK_FORMAT_D32_SFLOAT, "D32_SFLOAT");
  LogFormat(pd, VK_FORMAT_D16_UNORM, "D16_UNORM");

  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(pd, &mem);
  for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
    P("  memory heap %u: %.0f MB%s", i, MB(mem.memoryHeaps[i].size),
      (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
  }

  // Try a device with the renderer's hard requirements plus what is available.
  std::vector<const char*> dev_exts;
  for (const char* e : {"VK_KHR_portability_subset", "VK_KHR_swapchain",
                        "VK_EXT_fragment_shader_interlock", "VK_EXT_custom_border_color",
                        "VK_KHR_dynamic_rendering", "VK_EXT_extended_dynamic_state"}) {
    if (Has(exts, e)) dev_exts.push_back(e);
  }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = 0;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &f2;  // enable everything reported above
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = uint32_t(dev_exts.size());
  dci.ppEnabledExtensionNames = dev_exts.data();
  VkDevice device = VK_NULL_HANDLE;
  r = vkCreateDevice(pd, &dci, nullptr, &device);
  P("  vkCreateDevice with all reported features and %zu extensions: %s (%d)", dev_exts.size(),
    r == VK_SUCCESS ? "OK" : "FAILED", int(r));
  if (device) vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
}

// ---------------------------------------------------------------------------
// 2. SDK page allocation
// ---------------------------------------------------------------------------

void TestPageAllocation() {
  using namespace rex::memory;
  P("[2/5] SDK page allocation (AllocFixed / Protect / QueryProtect)");
  P("  page size %zu, allocation granularity %zu", page_size(), allocation_granularity());
  const size_t reserve = size_t(512) << 20;
  void* p = AllocFixed(nullptr, reserve, AllocationType::kReserve, PageAccess::kNoAccess);
  if (!p) {
    P("  reserve 512 MB FAILED: errno %d (%s)", errno, std::strerror(errno));
    return;
  }
  const size_t commit = size_t(64) << 20;
  void* c = AllocFixed(p, commit, AllocationType::kCommit, PageAccess::kReadWrite);
  P("  reserve 512 MB OK, commit 64 MB %s", c ? "OK" : "FAILED");
  if (c) {
    for (size_t off = 0; off < commit; off += page_size()) static_cast<uint8_t*>(c)[off] = 1;
    size_t len = 0;
    PageAccess acc{};
    bool q = QueryProtect(c, len, acc);
    P("  touched 64 MB; QueryProtect (vm_region_64 patch) %s: length %.0f MB, access %d",
      q ? "OK" : "FAILED", MB(len), int(acc));
    bool pr = Protect(c, commit, PageAccess::kReadOnly);
    q = QueryProtect(c, len, acc);
    P("  Protect read-only %s, QueryProtect now access %d", pr ? "OK" : "FAILED", int(acc));
  }
  DeallocFixed(p, 0, DeallocationType::kRelease);
}

// ---------------------------------------------------------------------------
// 3. Guest memory arena, mirroring Memory::Initialize + MapViewsMac in the SDK
// ---------------------------------------------------------------------------

const struct {
  uint64_t start, end, target;
} kViews[] = {
    {0x00000000, 0x3FFFFFFF, 0x0000000000000000ull},  // virtual 4k
    {0x40000000, 0x7EFFFFFF, 0x0000000040000000ull},  // virtual 64k
    {0x7F000000, 0x7FFFFFFF, 0x0000000100000000ull},  // GPU writeback
    {0x80000000, 0x8FFFFFFF, 0x0000000080000000ull},  // xex 64k
    {0x90000000, 0x9FFFFFFF, 0x0000000080000000ull},  // xex 4k
    {0xA0000000, 0xBFFFFFFF, 0x0000000100000000ull},  // physical 64k
    {0xC0000000, 0xDFFFFFFF, 0x0000000100000000ull},  // physical 16mb
    {0xE0000000, 0xFFFFFFFF, 0x0000000100001000ull},  // physical 4k
    {0x100000000, 0x11FFFFFFF, 0x0000000100000000ull},  // physical raw
};

void TestGuestArena(const char* temp_dir) {
  P("[3/5] Guest memory arena (SDK layout: 4.5 GB backing object, 4.5 GB of views)");
  P("  available memory before: %.0f MB", MB(os_proc_available_memory()));
  const size_t gran = rex::memory::allocation_granularity();
  const size_t mapping_size = ((0x120000000ull + gran) + gran - 1) / gran * gran;

  // The SDK's own call first (shm_open + ftruncate).
  const char* shm_name = "/mcla_probe_arena";
  shm_unlink(shm_name);
  int fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
  bool shm = fd >= 0;
  if (!shm) {
    P("  shm_open FAILED: errno %d (%s)", errno, std::strerror(errno));
  } else if (ftruncate(fd, off_t(mapping_size)) != 0) {
    P("  shm_open OK, ftruncate %.2f GB FAILED: errno %d (%s)", mapping_size / 1073741824.0,
      errno, std::strerror(errno));
    close(fd);
    shm_unlink(shm_name);
    fd = -1;
    shm = false;
  } else {
    P("  shm_open + ftruncate %.2f GB OK (the SDK's path works as is)",
      mapping_size / 1073741824.0);
  }
  auto sdk_handle = rex::memory::CreateFileMappingHandle("mcla_probe_sdk", mapping_size,
                                                         rex::memory::PageAccess::kReadWrite,
                                                         false);
  P("  rex::memory::CreateFileMappingHandle: %s",
    sdk_handle != rex::memory::kFileMappingHandleInvalid ? "OK" : "FAILED");
  if (sdk_handle != rex::memory::kFileMappingHandleInvalid) {
    rex::memory::CloseFileMappingHandle(sdk_handle, "mcla_probe_sdk");
  }

  if (fd < 0) {
    // Fallback the port could use: an unlinked sparse file in the app's temp directory.
    std::string path = std::string(temp_dir) + "/mcla_probe_arena.bin";
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) unlink(path.c_str());
    if (fd < 0 || ftruncate(fd, off_t(mapping_size)) != 0) {
      P("  fallback temp file FAILED: errno %d (%s)", errno, std::strerror(errno));
      if (fd >= 0) close(fd);
      return;
    }
    P("  fallback: unlinked sparse temp file of %.2f GB OK", mapping_size / 1073741824.0);
  }

  const size_t total = size_t(kViews[8].end - kViews[0].start + 1);
  void* base = mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (base == MAP_FAILED) {
    P("  reserve %.2f GB contiguous address space FAILED: errno %d (%s)", total / 1073741824.0,
      errno, std::strerror(errno));
    // Find the largest contiguous reservation that does work.
    for (size_t sz = size_t(4) << 30; sz >= (size_t(256) << 20); sz -= size_t(256) << 20) {
      void* t = mmap(nullptr, sz, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (t != MAP_FAILED) {
        munmap(t, sz);
        P("  largest contiguous reservation: %.2f GB", sz / 1073741824.0);
        break;
      }
    }
    close(fd);
    if (shm) shm_unlink(shm_name);
    return;
  }
  P("  reserve %.2f GB contiguous address space OK at 0x%" PRIxPTR, total / 1073741824.0,
    reinterpret_cast<uintptr_t>(base));

  auto* b = static_cast<uint8_t*>(base);
  const uint64_t gran_mask = ~uint64_t(gran - 1);
  bool views_ok = true;
  for (size_t n = 0; n < sizeof(kViews) / sizeof(kViews[0]); ++n) {
    size_t size = size_t(kViews[n].end - kViews[n].start + 1);
    void* want = b + kViews[n].start;
    void* got = mmap(want, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
                     off_t(kViews[n].target & gran_mask));
    if (got != want) {
      P("  view %zu (guest 0x%09" PRIx64 ", %.0f MB) FAILED: errno %d (%s)", n, kViews[n].start,
        MB(size), errno, std::strerror(errno));
      views_ok = false;
      break;
    }
  }
  if (views_ok) {
    P("  all 9 guest views mapped");
    // Physical memory is visible at several guest addresses; check they alias.
    *reinterpret_cast<volatile uint32_t*>(b + 0xA0000100) = 0x12345678u;
    uint32_t c0 = *reinterpret_cast<volatile uint32_t*>(b + 0xC0000100);
    uint32_t raw = *reinterpret_cast<volatile uint32_t*>(b + 0x100000100ull);
    P("  aliasing physical views: %s (0x%08x, 0x%08x)",
      (c0 == 0x12345678u && raw == 0x12345678u) ? "OK" : "WRONG", c0, raw);
    // Touch 256 MB of guest RAM to see what it costs.
    const size_t touch = size_t(256) << 20;
    for (size_t off = 0; off < touch; off += 16384) b[0x40000000 + off] = 1;
    P("  touched 256 MB of guest memory; available now %.0f MB",
      MB(os_proc_available_memory()));
  }
  munmap(base, total);
  close(fd);
  if (shm) shm_unlink(shm_name);
  P("  available after release: %.0f MB", MB(os_proc_available_memory()));
}

// ---------------------------------------------------------------------------
// 4. SDK fault handler
// ---------------------------------------------------------------------------

struct FaultState {
  uint8_t* page;
  size_t size;
  int hits;
  int op;
  uint64_t addr;
};

bool OnFault(rex::arch::Exception* ex, void* data) {
  auto* s = static_cast<FaultState*>(data);
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation) return false;
  uint64_t a = ex->fault_address();
  if (a < uint64_t(uintptr_t(s->page)) || a >= uint64_t(uintptr_t(s->page)) + s->size) {
    return false;
  }
  s->hits++;
  s->op = int(ex->access_violation_operation());
  s->addr = a;
  mprotect(s->page, s->size, PROT_READ | PROT_WRITE);
  return true;  // retry the faulting store
}

void TestFaultHandler() {
  P("[4/5] SDK fault handler (if the app closes here, iOS did not deliver the fault)");
  const size_t size = rex::memory::page_size();
  void* page = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (page == MAP_FAILED) {
    P("  mmap FAILED");
    return;
  }
  FaultState s{static_cast<uint8_t*>(page), size, 0, 0, 0};
  rex::arch::ExceptionHandler::Install(OnFault, &s);
  P("  handler installed; writing to a protected page");
  reinterpret_cast<volatile uint32_t*>(page)[3] = 0xCAFEF00Du;
  uint32_t back = reinterpret_cast<volatile uint32_t*>(page)[3];
  rex::arch::ExceptionHandler::Uninstall(OnFault, &s);
  P("  fault caught %d time(s), operation %s, address %s, value after retry %s",
    s.hits, s.op == 2 ? "write (correct)" : (s.op == 1 ? "read (wrong)" : "unknown"),
    s.addr == uint64_t(uintptr_t(page)) + 12 ? "exact" : "off",
    back == 0xCAFEF00Du ? "correct" : "WRONG");
  munmap(page, size);
}

// ---------------------------------------------------------------------------
// 5. SDK fibers
// ---------------------------------------------------------------------------

rex::thread::Fiber* g_main_fiber = nullptr;
int g_fiber_steps = 0;
double g_fiber_acc = 0;

void FiberEntry(void* arg) {
  double step = *static_cast<double*>(arg);
  for (;;) {
    ++g_fiber_steps;
    g_fiber_acc += step;
    rex::thread::Fiber::SwitchTo(g_main_fiber);
  }
}

void TestFibers() {
  P("[5/5] SDK fibers (ucontext; if the app closes here, fibers need an ARM64 replacement)");
  g_main_fiber = rex::thread::Fiber::ConvertCurrentThread();
  if (!g_main_fiber) {
    P("  ConvertCurrentThread FAILED");
    return;
  }
  double step = 0.25;
  rex::thread::Fiber* f = rex::thread::Fiber::Create(256 * 1024, FiberEntry, &step);
  if (!f) {
    P("  Create FAILED");
    return;
  }
  P("  created a fiber; switching to it 1000 times");
  double local = 1.5;  // must survive the switches (callee-saved FP registers)
  for (int i = 0; i < 1000; ++i) {
    rex::thread::Fiber::SwitchTo(f);
    local *= 1.0;
  }
  P("  fiber ran %d steps, accumulator %.2f (expect 1000, 250.00), local %.2f (expect 1.50)",
    g_fiber_steps, g_fiber_acc, local);
  f->Destroy();
}

}  // namespace

extern "C" void mcla_probe_run(const char* temp_dir, mcla_log_fn log) {
  g_log = log;
  P("probe start (ReXGlue SDK + MoltenVK linked; no game code)");
  TestVulkan();
  TestPageAllocation();
  TestGuestArena(temp_dir);
  TestFaultHandler();
  TestFibers();
  P("probe finished: every step completed");
}
