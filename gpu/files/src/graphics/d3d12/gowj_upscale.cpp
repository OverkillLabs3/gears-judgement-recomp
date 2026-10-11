// Temporal upscaler pass runner: camera motion vectors -> DLSS -> write back into the scene colour
// texture, all recorded as one callback in the guest command stream.
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <cstring>
#include <memory>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/gowj_dlss.h>
#include <rex/graphics/d3d12/gowj_fsr.h>
#include <rex/graphics/d3d12/gowj_temporal.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(gowj_dlss, false, "gfx", "Run NVIDIA DLSS on the scene colour (needs gowj_temporal)");
REXCVAR_DEFINE_STRING(gowj_upscaler, "off", "gfx",
                      "Temporal upscaler: off, dlss, fsr3 (FSR 3.1), fsr4, auto (dlss, else fsr3)");
REXCVAR_DEFINE_INT32(gowj_upscale_quality, -1, "gfx",
                     "Upscaler mode: -1 auto (from the quality level: Low performance, Medium balanced, "
                     "High quality), 0 performance, 1 balanced, 2 quality, 3 ultra performance, "
                     "4 ultra quality, 5 native (DLAA)");
REXCVAR_DEFINE_DOUBLE(gowj_render_scale, 0.0, "gfx",
                      "Scene render scale for the upscaler (0 = from the DLSS quality mode, 1 = native)");
REXCVAR_DEFINE_BOOL(gowj_dlss_hdr, false, "gfx", "Tell DLSS the scene colour is HDR/linear");
REXCVAR_DEFINE_INT32(gowj_mv_debug, 0, "gfx",
                     "1 = write the motion vectors into the scene colour instead of the upscaled "
                     "image (debug), 2 = bypass (copy input colour back)");

namespace rex::graphics::d3d12::gowj {

namespace {

using Microsoft::WRL::ComPtr;

const char kMotionHlsl[] = R"(
cbuffer C : register(b0) {
  row_major float4x4 invVP;
  row_major float4x4 prevVP;
  float3 tdelta; float pad0;
  uint2 size; uint2 pad1;
};
Texture2D<float> depthTex : register(t0);
RWTexture2D<float2> mvOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= size.x || id.y >= size.y) return;
  float d = depthTex[id.xy];
  float z = 1.0 - d;
  float2 ndc = float2((id.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (id.y + 0.5) / size.y * 2.0);
  float4 q = mul(float4(ndc, z, 1.0), invVP);
  float3 p = q.xyz / q.w;
  float4 cp = mul(float4(p + tdelta, 1.0), prevVP);
  float2 mv = float2(0.0, 0.0);
  if (cp.w > 1e-4) {
    float2 pn = cp.xy / cp.w;
    float2 ppx = float2((pn.x * 0.5 + 0.5) * size.x, (0.5 - pn.y * 0.5) * size.y);
    mv = ppx - (float2(id.xy) + 0.5);
  }
  mvOut[id.xy] = mv;
}
)";

const char kWritebackHlsl[] = R"(
cbuffer C : register(b0) {
  row_major float4x4 m0; row_major float4x4 m1; float3 t0; float p0;
  uint2 size; uint mode; uint p1;
};
Texture2D<float4> src : register(t0);
RWTexture2D<unorm float4> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= size.x || id.y >= size.y) return;
  float4 o = src[id.xy];
#ifdef KEEP_ALPHA
  float a = dst[id.xy].a;
#else
  float a = 1.0;
#endif
  dst[id.xy] = float4(saturate(o.rgb), a);
}
)";

struct Job {
  ComPtr<ID3D12Resource> color, depth;
  float inv_vp[16], prev_vp[16], tdelta[3];
  uint32_t rw, rh, ow, oh;
  float jx, jy;
  bool reset;
  int quality;
  bool hdr;
  int debug;
  int backend = 0;  // 0 DLSS, 1 FSR
  int fsr_major = 3;
  float cam_near = 10.0f, cam_far = 8000.0f, fov_y = 0.9f;
  D3D12_GPU_DESCRIPTOR_HANDLE table_mv, table_wb;
  ID3D12Resource* mv = nullptr;   // owned by g_mv
  ID3D12Resource* out = nullptr;     // owned by g.out
  ID3D12Resource* final_ = nullptr;  // owned by g.final_: what the presenter shows
  uint64_t submission = 0;
};

struct Gpu {
  bool tried = false, ok = false;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> rs;
  ComPtr<ID3D12PipelineState> pso_mv, pso_wb;
  ComPtr<ID3D12DescriptorHeap> heap;
  UINT inc = 0;
  bool keep_alpha = false;
  ComPtr<ID3D12Resource> mv, out, final_;
  uint32_t mv_w = 0, mv_h = 0, out_w = 0, out_h = 0;
  D3D12_RESOURCE_STATES mv_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  D3D12_RESOURCE_STATES out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t slice = 0;
  std::vector<std::unique_ptr<Job>> jobs;
  std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource>>> graveyard;
  ComPtr<ID3D12Resource> depth_tex;
  uint64_t depth_area = 0;
  uint32_t last_frame = 0;
  bool dlss_failed = false;
};
Gpu g;
constexpr uint32_t kSlices = 8;
constexpr uint32_t kPerSlice = 4;

typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                     LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

ComPtr<ID3D12PipelineState> MakePso(ID3D12Device* dev, ID3D12RootSignature* rs, const char* src,
                                    const char* name, const D3D_SHADER_MACRO* defs) {
  static PFN_Compile compile = [] {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    return m ? reinterpret_cast<PFN_Compile>(GetProcAddress(m, "D3DCompile")) : nullptr;
  }();
  if (!compile) {
    REXGPU_WARN("GOWJ: d3dcompiler_47.dll not found");
    return nullptr;
  }
  ComPtr<ID3DBlob> blob, err;
  if (FAILED(compile(src, std::strlen(src), name, defs, nullptr, "main", "cs_5_0", 1 << 15, 0,
                     &blob, &err))) {
    REXGPU_WARN("GOWJ: shader {} failed: {}", name,
                err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return nullptr;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = rs;
  d.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)))) {
    REXGPU_WARN("GOWJ: PSO {} failed", name);
    return nullptr;
  }
  return pso;
}

bool InitGpu(D3D12CommandProcessor& cp) {
  if (g.tried) return g.ok;
  g.tried = true;
  g.device = cp.GetD3D12Provider().GetDevice();
  ID3D12Device* dev = g.device.Get();

  D3D12_FEATURE_DATA_D3D12_OPTIONS opt = {};
  dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof opt);
  D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_R10G10B10A2_UNORM};
  dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs);
  g.keep_alpha = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) &&
                 (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
  REXGPU_WARN("GOWJ upscale: R10G10B10A2 typed UAV load={} store={} (alpha preserved: {})",
              (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0,
              (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0, g.keep_alpha);

  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 1;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.Num32BitValues = 40;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {};
  rsd.NumParameters = 2;
  rsd.pParameters = params;
  ComPtr<ID3DBlob> rsblob, rserr;
  if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &rserr)) ||
      FAILED(dev->CreateRootSignature(0, rsblob->GetBufferPointer(), rsblob->GetBufferSize(),
                                      IID_PPV_ARGS(&g.rs)))) {
    REXGPU_WARN("GOWJ upscale: root signature failed");
    return false;
  }
  g.pso_mv = MakePso(dev, g.rs.Get(), kMotionHlsl, "gowj_motion", nullptr);
  D3D_SHADER_MACRO defs[] = {{"KEEP_ALPHA", "1"}, {nullptr, nullptr}};
  (void)defs;
  g.pso_wb = MakePso(dev, g.rs.Get(), kWritebackHlsl, "gowj_writeback", nullptr);  // alpha = 1: a fresh image
  if (!g.pso_mv || !g.pso_wb) return false;

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = kSlices * kPerSlice;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap)))) return false;
  g.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  g.ok = true;
  REXGPU_WARN("GOWJ upscale: GPU objects ready");
  return true;
}

ComPtr<ID3D12Resource> MakeTex(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                               D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d = {};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> r;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                          IID_PPV_ARGS(&r))))
    return nullptr;
  return r;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
             D3D12_RESOURCE_STATES b) {
  if (a == b) return;
  D3D12_RESOURCE_BARRIER br = {};
  br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  br.Transition.pResource = r;
  br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  br.Transition.StateBefore = a;
  br.Transition.StateAfter = b;
  l->ResourceBarrier(1, &br);
}

bool Invert4x4(const double m[16], double out[16]) {
  double inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-30) return false;
  det = 1.0 / det;
  for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
  return true;
}

void RunJob(ID3D12GraphicsCommandList* list, void* user) {
  Job& j = *static_cast<Job*>(user);
  const D3D12_RESOURCE_STATES kSrv =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  ID3D12DescriptorHeap* heaps[1] = {g.heap.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(g.rs.Get());

  uint32_t consts[40] = {};
  std::memcpy(consts, j.inv_vp, 64);
  std::memcpy(consts + 16, j.prev_vp, 64);
  std::memcpy(consts + 32, j.tdelta, 12);
  // layout: invVP 0..15, prevVP 16..31, tdelta 32..34, pad 35, size 36..37
  consts[36] = j.rw;
  consts[37] = j.rh;

  // 1. Camera motion vectors.
  Barrier(list, j.mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->SetPipelineState(g.pso_mv.Get());
  list->SetComputeRoot32BitConstants(0, 40, consts, 0);
  list->SetComputeRootDescriptorTable(1, j.table_mv);
  list->Dispatch((j.rw + 7) / 8, (j.rh + 7) / 8, 1);
  Barrier(list, j.mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  bool have_out = false;
  if (j.debug != 1 && j.debug != 2) {
    // 2. The temporal upscaler (DLSS or FSR).
    if (j.backend == 1) {
      FsrEval e;
      e.color = j.color.Get();
      e.depth = j.depth.Get();
      e.motion = j.mv;
      e.output = j.out;
      e.render_w = j.rw;
      e.render_h = j.rh;
      e.out_w = j.ow;
      e.out_h = j.oh;
      e.jitter_x = j.jx;
      e.jitter_y = j.jy;
      e.reset = j.reset;
      e.camera_near = j.cam_near;
      e.camera_far = j.cam_far;
      e.fov_y = j.fov_y;
      e.wanted_major = j.fsr_major;
      have_out = FsrEvaluate(list, e);
    } else {
      DlssEval e;
      e.color = j.color.Get();
      e.depth = j.depth.Get();
      e.motion = j.mv;
      e.output = j.out;
      e.render_w = j.rw;
      e.render_h = j.rh;
      e.out_w = j.ow;
      e.out_h = j.oh;
      e.jitter_x = j.jx;
      e.jitter_y = j.jy;
      e.reset = j.reset;
      e.hdr = j.hdr;
      e.quality = j.quality;
      have_out = DlssEvaluate(list, e);
    }
    if (!have_out) g.dlss_failed = true;
  }
  // The upscaler (NGX / FidelityFX) rebinds its own heaps and root signature.
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(g.rs.Get());

  if (have_out) {
    // 3. Convert the upscaler's output into the image the presenter shows (R10G10B10A2).
    Barrier(list, j.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, j.final_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(g.pso_wb.Get());
    uint32_t wb[40] = {};
    wb[36] = j.ow;
    wb[37] = j.oh;
    list->SetComputeRoot32BitConstants(0, 40, wb, 0);
    list->SetComputeRootDescriptorTable(1, j.table_wb);
    list->Dispatch((j.ow + 7) / 8, (j.oh + 7) / 8, 1);
    Barrier(list, j.final_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, j.out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
}

}  // namespace

// 0 = off, 1 = DLSS, 2 = FSR 3.1, 3 = FSR 4.
static int SelectedBackend() {
  const std::string& u = REXCVAR_GET(gowj_upscaler);
  if (u == "off") return 0;  // explicit off wins over the legacy gowj_dlss switch
  if (u == "dlss") return 1;
  if (u == "fsr3" || u == "fsr") return 2;
  if (u == "fsr4") return 3;
  if (u == "auto") return g.dlss_failed ? 2 : 1;
  return REXCVAR_GET(gowj_dlss) ? 1 : 0;
}

int SelectedUpscaler() { return SelectedBackend(); }

bool UpscaleEnabled() { return SelectedBackend() != 0; }

bool RenderScale(uint32_t out_w, uint32_t out_h, float* fx, float* fy, uint32_t* rw, uint32_t* rh) {
  *fx = *fy = 1.0f;
  *rw = out_w;
  *rh = out_h;
  return false;  // the whole guest frame is rendered at the render size; nothing is shrunk any more
}

// The game resolves depth more than once per frame (the full-size scene depth, then smaller copies
// for effects). The largest one is the scene depth the upscaler needs.
void NoteDepthTexture(ID3D12Resource* depth) {
  if (!depth) return;
  const D3D12_RESOURCE_DESC d = depth->GetDesc();
  const uint64_t area = d.Width * uint64_t(d.Height);
  if (!g.depth_tex || area >= g.depth_area) {
    g.depth_tex = depth;
    g.depth_area = area;
  }
}

// The scaling ratio picks the DLSS mode: the render size is whatever the game was set to draw at.
static int ModeFromRatio(double r) {
  if (r >= 0.95) return 5;  // DLAA / native
  if (r >= 0.62) return 2;  // quality
  if (r >= 0.55) return 1;  // balanced
  if (r >= 0.42) return 0;  // performance
  return 3;                 // ultra performance
}

static bool TargetSize(uint32_t* w, uint32_t* h) {
  static uint32_t tw = 0, th = 0;
  static bool read = false;
  if (!read) {
    read = true;
    char b[16]{};
    if (GetEnvironmentVariableA("GOWJ_TARGET_W", b, sizeof b)) tw = uint32_t(std::atoi(b));
    if (GetEnvironmentVariableA("GOWJ_TARGET_H", b, sizeof b)) th = uint32_t(std::atoi(b));
  }
  *w = tw;
  *h = th;
  return tw >= 640 && th >= 360;
}

bool UpscaleFinal(D3D12CommandProcessor& cp, ID3D12Resource* color, uint32_t in_w, uint32_t in_h,
                  FinalImage& result) {
  FrameState& st = State();
  // Retire finished jobs and graveyard entries.
  for (size_t i = 0; i < g.jobs.size();) {
    if (cp.GetCompletedSubmission() >= g.jobs[i]->submission)
      g.jobs.erase(g.jobs.begin() + i);
    else
      ++i;
  }
  for (size_t i = 0; i < g.graveyard.size();) {
    if (cp.GetCompletedSubmission() >= g.graveyard[i].first)
      g.graveyard.erase(g.graveyard.begin() + i);
    else
      ++i;
  }
  // This frame's depth; the next frame starts with none.
  ComPtr<ID3D12Resource> frame_depth = std::move(g.depth_tex);
  g.depth_tex.Reset();
  g.depth_area = 0;
  if (!UpscaleEnabled()) return false;
  if (g.dlss_failed) {
    SetUpscaleStatus("Upscaler: failed (see log)");
    return false;
  }
  const SceneCamera& cam = st.ended_camera;
  const SceneCamera& prev = st.ended_prev_camera;
  // No scene this frame (menus, movies, loading): the guest frame is shown unchanged.
  if (!frame_depth || !cam.valid || !prev.valid) return false;
  if (st.last_upscaled_frame == st.ended_frame) return false;  // once per frame
  uint32_t tw = 0, th = 0;
  if (!TargetSize(&tw, &th)) return false;
  if (!InitGpu(cp)) return false;

  D3D12_RESOURCE_DESC cd = color->GetDesc();
  switch (cd.Format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      break;
    default: {
      static int bad = 0;
      if (bad++ < 3)
        REXGPU_WARN("GOWJ upscale: skipping, the frame has an unsupported format (dxgi {})", uint32_t(cd.Format));
      return false;
    }
  }
  D3D12_RESOURCE_DESC dd = frame_depth->GetDesc();
  {
    static int logged = 0;
    if (logged++ < 3)
      REXGPU_WARN("GOWJ upscale: frame colour {}x{} (active {}x{}) dxgi={}; depth {}x{} dxgi={}", uint32_t(cd.Width),
                  cd.Height, in_w, in_h, uint32_t(cd.Format), uint32_t(dd.Width), dd.Height, uint32_t(dd.Format));
  }
  if (dd.Width != in_w || dd.Height != in_h) {
    static int skipped = 0;
    if (skipped++ < 3)
      REXGPU_WARN("GOWJ upscale: skipping, depth {}x{} does not match the frame {}x{}", uint32_t(dd.Width), dd.Height,
                  in_w, in_h);
    return false;
  }

  const int backend_sel = SelectedBackend();  // 1 DLSS, 2 FSR 3.1, 3 FSR 4
  if (backend_sel == 1) {
    if (!DlssInit(g.device.Get())) {
      g.dlss_failed = true;
      SetUpscaleStatus(DlssCompiledIn() ? "DLSS: unavailable (NGX init failed or no nvngx_dlss.dll)"
                                        : "DLSS: not built into this plugin");
      return false;
    }
  } else {
    if (!FsrInit(g.device.Get())) {
      g.dlss_failed = true;
      SetUpscaleStatus("FSR: unavailable (amd_fidelityfx_loader_dx12.dll / upscaler DLL missing)");
      return false;
    }
  }

  // Output: the screen size at the frame's aspect ratio (never below the frame itself).
  uint32_t oh = std::max(th, in_h);
  uint32_t ow = uint32_t(std::lround(double(oh) * in_w / in_h));
  if (ow > tw && tw >= in_w) {
    ow = tw;
    oh = uint32_t(std::lround(double(ow) * in_h / in_w));
  }
  ow = std::max(ow, in_w);
  oh = std::max(oh, in_h);
  const uint32_t rw = in_w, rh = in_h;

  // (Re)create the motion-vector, upscaler-output and final images.
  if (!g.mv || g.mv_w != rw || g.mv_h != rh) {
    if (g.mv) g.graveyard.emplace_back(cp.GetCurrentSubmission(), g.mv);
    g.mv = MakeTex(g.device.Get(), rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.mv_w = rw;
    g.mv_h = rh;
  }
  if (!g.out || g.out_w != ow || g.out_h != oh) {
    if (g.out) g.graveyard.emplace_back(cp.GetCurrentSubmission(), g.out);
    if (g.final_) g.graveyard.emplace_back(cp.GetCurrentSubmission(), g.final_);
    g.out = MakeTex(g.device.Get(), ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.final_ = MakeTex(g.device.Get(), ow, oh, DXGI_FORMAT_R10G10B10A2_UNORM,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.out_w = ow;
    g.out_h = oh;
  }
  if (!g.mv || !g.out || !g.final_) return false;

  auto job = std::make_unique<Job>();
  job->color = color;
  job->depth = frame_depth;
  job->rw = rw;
  job->rh = rh;
  job->ow = ow;
  job->oh = oh;
  job->mv = g.mv.Get();
  job->out = g.out.Get();
  job->final_ = g.final_.Get();
  const int mode = ModeFromRatio(double(rh) / double(oh));
  job->quality = mode;
  job->backend = backend_sel == 1 ? 0 : 1;
  job->fsr_major = (backend_sel == 3 && FsrHasFsr4()) ? 4 : 3;
  {
    const float f = cam.c[2][1] > 1e-3f ? cam.c[2][1] : 2.0f;  // world-z scale of clip.y
    job->fov_y = 2.0f * std::atan(1.0f / f);
    const float a = std::fabs(cam.c[0][3]) > 1e-6f ? cam.c[0][2] / cam.c[0][3] : 1.0f;
    const float b = -(cam.c[3][2] - a * cam.c[3][3]);
    if (b > 0.0f && a > 0.5f && a < 1.0f) {
      job->cam_near = b / a;
      job->cam_far = job->cam_near * a / (1.0f - a);
    }
  }
  job->hdr = false;
  job->debug = 0;
  // The frame was jittered only if earlier swaps went through the upscaler; otherwise this is the
  // first frame of a run and the history has to start over.
  float jp[2] = {0.0f, 0.0f};
  if (st.ended_jittered) JitterPixelsForFrame(st.ended_frame, jp);
  job->jx = jp[0];
  job->jy = jp[1];
  job->reset = !st.ended_jittered || (st.ended_frame - g.last_frame) > 3;
  g.last_frame = st.ended_frame;

  double cur[16], prv[16], inv[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      cur[r * 4 + c] = cam.c[r][c];
      prv[r * 4 + c] = prev.c[r][c];
    }
  if (!Invert4x4(cur, inv)) return false;
  for (int i = 0; i < 16; ++i) {
    job->inv_vp[i] = float(inv[i]);
    job->prev_vp[i] = float(prv[i]);
  }
  for (int k = 0; k < 3; ++k) job->tdelta[k] = prev.c[13][k] - cam.c[13][k];
  float td = std::sqrt(job->tdelta[0] * job->tdelta[0] + job->tdelta[1] * job->tdelta[1] +
                       job->tdelta[2] * job->tdelta[2]);
  if (td > 5000.0f) job->reset = true;  // teleport / camera cut

  // Descriptors (this job's slice of the ring).
  uint32_t slice = (g.slice++) % kSlices;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(slice) * kPerSlice * g.inc;
  gpu.ptr += uint64_t(slice) * kPerSlice * g.inc;
  auto srv = [&](ID3D12Resource* r, DXGI_FORMAT f, uint32_t idx) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = f;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    h.ptr += size_t(idx) * g.inc;
    g.device->CreateShaderResourceView(r, &d, h);
  };
  auto uav = [&](ID3D12Resource* r, DXGI_FORMAT f, uint32_t idx) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = f;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    h.ptr += size_t(idx) * g.inc;
    g.device->CreateUnorderedAccessView(r, nullptr, &d, h);
  };
  srv(job->depth.Get(), DXGI_FORMAT_R32_FLOAT, 0);
  uav(job->mv, DXGI_FORMAT_R16G16_FLOAT, 1);
  srv(job->out, DXGI_FORMAT_R16G16B16A16_FLOAT, 2);
  uav(job->final_, DXGI_FORMAT_R10G10B10A2_UNORM, 3);
  job->table_mv = gpu;
  job->table_wb = gpu;
  job->table_wb.ptr += 2ull * g.inc;

  {
    static const char* kNames[] = {"Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "Native"};
    char buf[160];
    if (job->backend == 0) {
      std::snprintf(buf, sizeof buf, "DLSS %s: render %ux%u, output %ux%u", mode == 5 ? "DLAA" : kNames[mode], rw, rh,
                    ow, oh);
    } else {
      std::snprintf(buf, sizeof buf, "FSR %s %s: render %ux%u, output %ux%u%s", job->fsr_major == 4 ? "4" : "3.1",
                    kNames[mode], rw, rh, ow, oh,
                    (backend_sel == 3 && job->fsr_major != 4) ? "  (FSR 4 unavailable)" : "");
    }
    SetUpscaleStatus(buf);
  }
  job->submission = cp.GetCurrentSubmission();
  st.last_upscaled_frame = st.ended_frame;
  st.jitter_next = true;
  cp.SubmitBarriers();
  cp.GetDeferredCommandList().Callback(&RunJob, job.get());
  cp.InvalidateBindingsAfterExternalCommands();
  result.resource = g.final_.Get();
  result.w = ow;
  result.h = oh;
  g.jobs.push_back(std::move(job));
  return true;
}

}  // namespace rex::graphics::d3d12::gowj
