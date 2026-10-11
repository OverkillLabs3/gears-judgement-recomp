// NVIDIA DLSS (NGX) wrapper for the temporal pipeline. Nothing from NVIDIA is redistributed here:
// nvngx_dlss.dll is loaded at run time from the game folder when it is present.
#include <rex/graphics/d3d12/gowj_dlss.h>

#include <filesystem>

#include <rex/logging.h>

#ifdef GOWJ_HAS_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <nvsdk_ngx_params.h>
#endif

namespace rex::graphics::d3d12::gowj {

#ifdef GOWJ_HAS_DLSS
namespace {
ID3D12Device* g_device = nullptr;
NVSDK_NGX_Parameter* g_caps = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;
bool g_init_tried = false;
bool g_ok = false;
uint32_t g_in_w, g_in_h, g_out_w, g_out_h;
int g_quality = -1;
}  // namespace

static void NgxLog(const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  REXGPU_WARN("NGX: {}", msg);
}

bool DlssInit(ID3D12Device* device) {
  if (g_init_tried) return g_ok;
  g_init_tried = true;
  g_device = device;
  std::filesystem::path dir = std::filesystem::current_path();
  std::wstring dllpath = dir.wstring();
  // The single-file release unpacks the game elsewhere; the vendor DLLs sit in the folder the player
  // started it from (GOWJ_HOME), which is searched first.
  std::wstring home;
  {
    wchar_t b[1024];
    const DWORD n = GetEnvironmentVariableW(L"GOWJ_HOME", b, 1024);
    if (n && n < 1024) home = b;
  }
  if (!home.empty()) SetDllDirectoryW(home.c_str());
  const wchar_t* paths[2] = {home.empty() ? dllpath.c_str() : home.c_str(), dllpath.c_str()};
  NVSDK_NGX_FeatureCommonInfo info = {};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = home.empty() ? 1 : 2;
  info.LoggingInfo.LoggingCallback = &NgxLog;
  info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
  info.LoggingInfo.DisableOtherLoggingSinks = false;
  std::wstring data = (dir / L"gowj_ngx").wstring();
  std::filesystem::create_directories(dir / L"gowj_ngx");
  NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
      "a4d6d1f0-2f6e-4b5e-9b57-0d7c2c3a9b11", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data.c_str(),
      device, &info);
  if (NVSDK_NGX_FAILED(r)) {
    REXGPU_WARN("DLSS: NGX init failed 0x{:08X}", uint32_t(r));
    return false;
  }
  r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_caps);
  if (NVSDK_NGX_FAILED(r)) {
    REXGPU_WARN("DLSS: capability parameters failed 0x{:08X}", uint32_t(r));
    return false;
  }
  int available = 0;
  NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  if (!available) {
    int need_update = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                             &need_update);
    REXGPU_WARN("DLSS: super sampling unavailable (needs updated driver: {})", need_update);
    return false;
  }
  {
    // Diagnostics: can this device create the kind of shared textures NGX needs?
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 256;
    d.Height = 256;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    struct Variant {
      D3D12_HEAP_FLAGS hf;
      D3D12_RESOURCE_FLAGS rf;
      const char* name;
    } variants[] = {
        {D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "uav"},
        {D3D12_HEAP_FLAG_SHARED, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "shared+uav"},
        {D3D12_HEAP_FLAG_SHARED,
         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
         "shared+uav+simultaneous"},
    };
    for (auto& v : variants) {
      d.Flags = v.rf;
      ID3D12Resource* res = nullptr;
      HRESULT hr = device->CreateCommittedResource(&hp, v.hf, &d, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&res));
      REXGPU_WARN("DLSS diag: CreateCommittedResource {} -> 0x{:08X}", v.name, uint32_t(hr));
      if (res) res->Release();
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS opt = {};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof opt);
    REXGPU_WARN("DLSS diag: ResourceHeapTier {} ResourceBindingTier {}", int(opt.ResourceHeapTier),
                int(opt.ResourceBindingTier));
  }
  r = NVSDK_NGX_D3D12_AllocateParameters(&g_params);
  if (NVSDK_NGX_FAILED(r)) return false;
  g_ok = true;
  REXGPU_WARN("DLSS: NGX initialised, super sampling available");
  return true;
}

bool DlssEvaluate(ID3D12GraphicsCommandList* list, const DlssEval& e) {
  if (!g_ok) return false;
  if (!g_feature || g_in_w != e.render_w || g_in_h != e.render_h || g_out_w != e.out_w ||
      g_out_h != e.out_h || g_quality != e.quality) {
    if (g_feature) {
      NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
      g_feature = nullptr;
    }
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = e.render_w;
    cp.Feature.InHeight = e.render_h;
    cp.Feature.InTargetWidth = e.out_w;
    cp.Feature.InTargetHeight = e.out_h;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value(e.quality);
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                              NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (e.hdr) cp.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &g_feature, g_params, &cp);
    if (NVSDK_NGX_FAILED(r)) {
      REXGPU_WARN("DLSS: feature creation failed 0x{:08X} ({}x{} -> {}x{} q{})", uint32_t(r),
                  e.render_w, e.render_h, e.out_w, e.out_h, e.quality);
      g_feature = nullptr;
      return false;
    }
    g_in_w = e.render_w;
    g_in_h = e.render_h;
    g_out_w = e.out_w;
    g_out_h = e.out_h;
    g_quality = e.quality;
    REXGPU_WARN("DLSS: feature created {}x{} -> {}x{} quality {}", e.render_w, e.render_h, e.out_w,
                e.out_h, e.quality);
  }
  NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
  ep.Feature.pInColor = e.color;
  ep.Feature.pInOutput = e.output;
  ep.Feature.InSharpness = 0.0f;
  ep.pInDepth = e.depth;
  ep.pInMotionVectors = e.motion;
  ep.InJitterOffsetX = e.jitter_x;
  ep.InJitterOffsetY = e.jitter_y;
  ep.InRenderSubrectDimensions.Width = e.render_w;
  ep.InRenderSubrectDimensions.Height = e.render_h;
  ep.InReset = e.reset ? 1 : 0;
  ep.InMVScaleX = 1.0f;
  ep.InMVScaleY = 1.0f;
  ep.InPreExposure = 1.0f;
  ep.InExposureScale = 1.0f;
  ep.InFrameTimeDeltaInMsec = e.frame_ms;
  NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(list, g_feature, g_params, &ep);
  if (NVSDK_NGX_FAILED(r)) {
    REXGPU_WARN("DLSS: evaluate failed 0x{:08X}", uint32_t(r));
    return false;
  }
  return true;
}

void DlssShutdown() {
  if (g_feature) NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
  g_feature = nullptr;
  if (g_params) NVSDK_NGX_D3D12_DestroyParameters(g_params);
  g_params = nullptr;
  if (g_device) NVSDK_NGX_D3D12_Shutdown1(g_device);
  g_device = nullptr;
  g_ok = false;
}

bool DlssCompiledIn() { return true; }

#else  // !GOWJ_HAS_DLSS

bool DlssInit(ID3D12Device*) { return false; }
bool DlssEvaluate(ID3D12GraphicsCommandList*, const DlssEval&) { return false; }
void DlssShutdown() {}
bool DlssCompiledIn() { return false; }

#endif

}  // namespace rex::graphics::d3d12::gowj
