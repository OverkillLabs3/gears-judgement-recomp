// AMD FidelityFX Super Resolution backend for the temporal pipeline. Nothing from AMD is
// redistributed: amd_fidelityfx_loader_dx12.dll (and the upscaler provider next to it) are loaded
// from the game folder when present.
#include <rex/graphics/d3d12/gowj_fsr.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>

#ifdef GOWJ_HAS_FFX
#define _WINDOWS 1
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_types.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>
#endif

namespace rex::graphics::d3d12::gowj {

#ifdef GOWJ_HAS_FFX
namespace {
ID3D12Device* g_device = nullptr;
HMODULE g_module = nullptr;
ffxFunctions g_fn = {};
bool g_tried = false, g_ok = false;
ffxContext g_ctx = nullptr;
uint32_t g_rw, g_rh, g_ow, g_oh;
int g_major = -1;
std::string g_provider;
bool g_fsr4 = false;
struct Version {
  uint64_t id;
  std::string name;
};
std::vector<Version> g_versions;

void FsrMessage(uint32_t type, const wchar_t* msg) {
  char buf[512];
  size_t n = 0;
  wcstombs_s(&n, buf, sizeof buf, msg, _TRUNCATE);
  REXGPU_WARN("FFX{}: {}", type, buf);
}

// Creation chain: upscale desc -> DX12 backend desc (-> version override).
struct Chain {
  ffxCreateContextDescUpscale up{};
  ffxCreateBackendDX12Desc be{};
  ffxOverrideVersion ov{};
};
}  // namespace

bool FsrInit(ID3D12Device* device) {
  if (g_tried) return g_ok;
  g_tried = true;
  g_device = device;
  // Look in the folder the player started the game from first (the single-file release unpacks the
  // game elsewhere), then in the default search path.
  {
    wchar_t b[1024];
    const DWORD n = GetEnvironmentVariableW(L"GOWJ_HOME", b, 1024);
    if (n && n < 1024) {
      SetDllDirectoryW(b);  // the loader finds the upscaler DLL there too
      g_module = LoadLibraryExW((std::wstring(b) + L"\\amd_fidelityfx_loader_dx12.dll").c_str(), nullptr,
                                LOAD_WITH_ALTERED_SEARCH_PATH);
    }
  }
  if (!g_module) g_module = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
  if (!g_module) {
    REXGPU_WARN("FSR: amd_fidelityfx_loader_dx12.dll not found next to the game");
    return false;
  }
  ffxLoadFunctions(&g_fn, g_module);
  if (!g_fn.CreateContext || !g_fn.Dispatch || !g_fn.Query || !g_fn.DestroyContext) {
    REXGPU_WARN("FSR: loader exports are missing");
    return false;
  }
  // Enumerate the upscaler providers this machine can use.
  uint64_t count = 0;
  ffxQueryDescGetVersions q{};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  q.device = device;
  q.outputCount = &count;
  if (g_fn.Query(nullptr, &q.header) == FFX_API_RETURN_OK && count) {
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    uint64_t cap = count;
    q.outputCount = &cap;
    if (g_fn.Query(nullptr, &q.header) == FFX_API_RETURN_OK) {
      for (uint64_t i = 0; i < cap; ++i) {
        g_versions.push_back({ids[i], names[i] ? names[i] : ""});
        REXGPU_WARN("FSR provider {}: id=0x{:X} '{}'", i, ids[i], names[i] ? names[i] : "?");
        std::string nm = names[i] ? names[i] : "";
        if (nm.find("4.") != std::string::npos && nm.find("FSR") != std::string::npos &&
            nm.find("3.") == std::string::npos)
          g_fsr4 = true;
      }
    }
  }
  if (g_versions.empty()) {
    REXGPU_WARN("FSR: the loader reports no upscaler providers (is the upscaler DLL next to it?)");
    return false;
  }
  g_ok = true;
  return true;
}

bool FsrHasFsr4() { return g_fsr4; }
const char* FsrProviderName() { return g_provider.c_str(); }

static bool CreateCtx(const FsrEval& e) {
  Chain c;
  c.up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  c.up.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
               FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  if (std::getenv("GOWJ_FSR_DEBUG")) c.up.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
  c.up.maxRenderSize = {e.render_w, e.render_h};
  c.up.maxUpscaleSize = {e.out_w, e.out_h};
  c.up.fpMessage = &FsrMessage;
  c.be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  c.be.device = g_device;
  c.up.header.pNext = &c.be.header;
  // Pick a provider by major version (names look like "FSR 3.1.5" / "FSR 4.0.2").
  uint64_t chosen = 0;
  for (const Version& v : g_versions) {
    const size_t p = v.name.find_first_of("0123456789");
    if (p == std::string::npos) continue;
    int major = v.name[p] - '0';
    if (major == e.wanted_major) {
      chosen = v.id;
      g_provider = v.name;
      break;
    }
  }
  if (chosen) {
    c.ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    c.ov.versionId = chosen;
    c.be.header.pNext = &c.ov.header;
  } else {
    g_provider = g_versions.front().name;
  }
  ffxReturnCode_t rc = g_fn.CreateContext(&g_ctx, &c.up.header, nullptr);
  if (rc != FFX_API_RETURN_OK) {
    REXGPU_WARN("FSR: context creation failed ({}), provider '{}'", uint32_t(rc), g_provider);
    g_ctx = nullptr;
    return false;
  }
  g_rw = e.render_w;
  g_rh = e.render_h;
  g_ow = e.out_w;
  g_oh = e.out_h;
  g_major = e.wanted_major;
  REXGPU_WARN("FSR: context created '{}' render {}x{} -> {}x{}", g_provider, g_rw, g_rh, g_ow, g_oh);
  return true;
}

bool FsrEvaluate(ID3D12GraphicsCommandList* list, const FsrEval& e) {
  if (!g_ok) return false;
  if (!g_ctx || g_rw != e.render_w || g_rh != e.render_h || g_ow != e.out_w || g_oh != e.out_h ||
      g_major != e.wanted_major) {
    if (g_ctx) {
      g_fn.DestroyContext(&g_ctx, nullptr);
      g_ctx = nullptr;
    }
    if (!CreateCtx(e)) return false;
  }
  ffxDispatchDescUpscale d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = list;
  d.color = ffxApiGetResourceDX12(e.color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  d.depth = ffxApiGetResourceDX12(e.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  d.motionVectors = ffxApiGetResourceDX12(e.motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  d.output = ffxApiGetResourceDX12(e.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  // The guest scene colour is created TYPELESS; give FidelityFX a concrete format to view it with.
  d.color.description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
  d.jitterOffset = {e.jitter_x, e.jitter_y};
  d.motionVectorScale = {1.0f, 1.0f};
  d.renderSize = {e.render_w, e.render_h};
  d.upscaleSize = {e.out_w, e.out_h};
  d.enableSharpening = false;
  d.sharpness = 0.0f;
  d.frameTimeDelta = e.frame_ms;
  d.preExposure = 1.0f;
  d.reset = e.reset;
  d.cameraNear = e.camera_near;
  d.cameraFar = e.camera_far;
  d.cameraFovAngleVertical = e.fov_y;
  d.viewSpaceToMetersFactor = 0.01f;  // Unreal units are centimetres
  d.flags = 0;
  ffxReturnCode_t rc = g_fn.Dispatch(&g_ctx, &d.header);
  if (rc != FFX_API_RETURN_OK) {
    REXGPU_WARN("FSR: dispatch failed ({})", uint32_t(rc));
    return false;
  }
  return true;
}

void FsrShutdown() {
  if (g_ctx && g_fn.DestroyContext) g_fn.DestroyContext(&g_ctx, nullptr);
  g_ctx = nullptr;
}

#else  // !GOWJ_HAS_FFX

bool FsrInit(ID3D12Device*) { return false; }
bool FsrHasFsr4() { return false; }
const char* FsrProviderName() { return ""; }
bool FsrEvaluate(ID3D12GraphicsCommandList*, const FsrEval&) { return false; }
void FsrShutdown() {}

#endif

}  // namespace rex::graphics::d3d12::gowj
