// gowj - ReXGlue Recompiled Project
//
// Final-image pipeline: SMAA 1x -> AMD FSR 1.0 EASU -> FSR RCAS.
//
// The runtime's presenter carries FSR/CAS paths behind REX_HAS_FIDELITYFX_SDK, but
// the prebuilt rexruntime.dll was compiled without them (its present_effect help
// text offers only "bilinear"), so the guest output (2560x1440 at the shipped
// draw_resolution_scale=2) was stretched to the 3840x2160 panel bilinearly, with
// no anti-aliasing beyond whatever the game drew.
//
// This redraws the final image ourselves, without touching runtime internals:
//  * Input: HookRefresh (gowj_perf_watch.cpp) wraps the presenter's refresher and
//    hands the guest output texture - D3D12GuestOutputRefreshContext::
//    resource_uav_capable, R10G10B10A2, left in PIXEL_SHADER_RESOURCE - to
//    GowjFsrNoteGuestOutput.
//  * Output: the runtime's REAL swap chain is patched at creation (factory import +
//    vtable hooks; a probe swap chain's vtable measured NOT to be the runtime's).
//    Right before each Present, on the runtime's own direct queue - captured from
//    the CreateSwapChain* call, because a D3D12 swap chain's GetDevice returns the
//    device, never the queue (that is why the first build never ran):
//       SMAA edges -> weights -> neighborhood blend   (guest size)
//       EASU (or bilinear) -> intermediate            (back-buffer size)
//       RCAS -> back buffer
//  * Skipped while the runtime's ImGui (F4 settings / ` console) is open, so those
//    windows, which are painted into the back buffer, stay visible.
// Shaders: gowj/src/fsr/*.hlsl, precompiled to fsr/g_*.h by fxc.

#include "gowj_fsr.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#define A_CPU 1
#include "../third_party/fsr1/ffx_a.h"
#include "../third_party/fsr1/ffx_fsr1.h"
#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"

#include "fsr/g_fsr_easu_ps.h"
#include "fsr/g_fsr_rcas_ps.h"
#include "fsr/g_fsr_vs.h"
#include "fsr/g_smaa_BilinearPS.h"
#include "fsr/g_smaa_BlendPS.h"
#include "fsr/g_smaa_BlendVS.h"
#include "fsr/g_smaa_EdgePS.h"
#include "fsr/g_smaa_EdgeVS.h"
#include "fsr/g_smaa_PlainVS.h"
#include "fsr/g_smaa_WeightPS.h"
#include "fsr/g_smaa_WeightVS.h"

extern "C" bool GowjOverlayOpen();

REXCVAR_DEFINE_BOOL(gowj_fsr, true, "gfx",
                    "Final-image pipeline (SMAA + FSR 1.0). False = the runtime's own "
                    "bilinear stretch, untouched.");
REXCVAR_DEFINE_BOOL(gowj_smaa, true, "gfx",
                    "SMAA 1x anti-aliasing on the guest output before upscaling.");
REXCVAR_DEFINE_BOOL(gowj_fsr_easu, true, "gfx",
                    "FSR EASU edge-adaptive upscaling to the panel. False = bilinear.");
REXCVAR_DEFINE_DOUBLE(gowj_fsr_sharpness, 0.5, "gfx",
                      "FSR RCAS sharpness reduction in stops: 0 = sharpest, 2 = softest.");

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kFrames = 3;
constexpr UINT kSrvPerFrame = 16;
constexpr UINT kRtvPerFrame = 8;
constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R10G10B10A2_UNORM;

std::mutex g_guest_mu;
ComPtr<ID3D12Resource> g_guest;  // latest refreshed guest output
uint32_t g_guest_w = 0, g_guest_h = 0;

std::mutex g_queue_mu;
std::atomic<bool> g_probing{false};  // our own throwaway swap chain is being created
ComPtr<ID3D12CommandQueue> g_chain_queue;  // queue the runtime created its swap chain on

struct Tex {
  ComPtr<ID3D12Resource> res;
  UINT w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

struct State {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> easu, bilinear, rcas, edge, weight, blend;
  DXGI_FORMAT back_format = DXGI_FORMAT_UNKNOWN;
  ComPtr<ID3D12DescriptorHeap> srv_heap, rtv_heap;
  UINT srv_inc = 0, rtv_inc = 0;
  ComPtr<ID3D12CommandAllocator> alloc[kFrames];
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;
  UINT64 frame_fence[kFrames] = {};
  UINT frame = 0;
  Tex inter, edges, weights, smaa_out, area, search;
  bool failed = false;
};
State g;
std::mutex g_state_mu;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;

bool Fail(const char* what, HRESULT hr) {
  REXLOG_WARN("FSR {} failed hr={:08x} - final-image pipeline disabled for this session", what,
              static_cast<uint32_t>(hr));
  g.failed = true;
  return false;
}

// One log line per distinct reason the pipeline did not run, so a silent no-op
// (the first build's failure mode) cannot happen again.
void Skip(int reason, const char* why) {
  static std::atomic<uint32_t> seen{0};
  if (!(seen.fetch_or(1u << reason) & (1u << reason))) {
    REXLOG_WARN("FSR pipeline skipped: {}", why);
  }
}

void WaitAll() {
  for (UINT64 v : g.frame_fence) {
    if (v && g.fence->GetCompletedValue() < v) {
      g.fence->SetEventOnCompletion(v, g.fence_event);
      WaitForSingleObject(g.fence_event, 2000);
    }
  }
}

ComPtr<ID3D12PipelineState> Pso(const void* vs, size_t vs_size, const void* ps, size_t ps_size,
                                DXGI_FORMAT fmt) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = g.root.Get();
  pd.VS = {vs, vs_size};
  pd.PS = {ps, ps_size};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = fmt;
  pd.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  const HRESULT hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
  if (FAILED(hr)) Fail("pipeline", hr);
  return pso;
}

bool BuildRoot() {
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 16};
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;  // SMAA VS reads metrics
  D3D12_STATIC_SAMPLER_DESC samp{};
  samp.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.MaxLOD = D3D12_FLOAT32_MAX;
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rs{2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
                                       D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
  auto serialize = reinterpret_cast<SerializeFn>(
      GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12SerializeRootSignature"));
  ComPtr<ID3DBlob> blob, err;
  HRESULT hr = serialize ? serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err) : E_FAIL;
  if (FAILED(hr)) return Fail("root signature serialize", hr);
  hr = g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                     IID_PPV_ARGS(&g.root));
  if (FAILED(hr)) return Fail("CreateRootSignature", hr);
  g.easu = Pso(g_fsr_vs, sizeof(g_fsr_vs), g_fsr_easu_ps, sizeof(g_fsr_easu_ps), kColor);
  g.bilinear = Pso(g_smaa_PlainVS, sizeof(g_smaa_PlainVS), g_smaa_BilinearPS,
                   sizeof(g_smaa_BilinearPS), kColor);
  g.edge = Pso(g_smaa_EdgeVS, sizeof(g_smaa_EdgeVS), g_smaa_EdgePS, sizeof(g_smaa_EdgePS),
               DXGI_FORMAT_R8G8_UNORM);
  g.weight = Pso(g_smaa_WeightVS, sizeof(g_smaa_WeightVS), g_smaa_WeightPS,
                 sizeof(g_smaa_WeightPS), DXGI_FORMAT_R8G8B8A8_UNORM);
  g.blend = Pso(g_smaa_BlendVS, sizeof(g_smaa_BlendVS), g_smaa_BlendPS, sizeof(g_smaa_BlendPS),
                kColor);
  return !g.failed;
}

bool MakeTex(Tex& t, UINT w, UINT h, DXGI_FORMAT fmt, bool rt) {
  if (t.res && t.w == w && t.h == h && t.fmt == fmt) return true;
  WaitAll();  // earlier frames may still read the old texture
  D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = w;
  rd.Height = h;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = fmt;
  rd.SampleDesc.Count = 1;
  rd.Flags = rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
  t.res.Reset();
  const HRESULT hr = g.device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd,
      rt ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&t.res));
  if (FAILED(hr)) return Fail("texture", hr);
  t.w = w;
  t.h = h;
  t.fmt = fmt;
  return true;
}

// SMAA's precomputed area/search lookup textures, uploaded once.
bool UploadLookups() {
  if (!MakeTex(g.area, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, false) ||
      !MakeTex(g.search, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, false)) {
    return false;
  }
  struct Src {
    Tex* t;
    const unsigned char* bytes;
    UINT pitch;
  } srcs[2] = {{&g.area, areaTexBytes, AREATEX_PITCH}, {&g.search, searchTexBytes, SEARCHTEX_PITCH}};
  ComPtr<ID3D12Resource> upload[2];
  g.alloc[0]->Reset();
  g.list->Reset(g.alloc[0].Get(), nullptr);
  for (int i = 0; i < 2; ++i) {
    D3D12_RESOURCE_DESC td = srcs[i].t->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&upload[i]));
    if (FAILED(hr)) return Fail("upload buffer", hr);
    uint8_t* p = nullptr;
    upload[i]->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (UINT y = 0; y < srcs[i].t->h; ++y) {
      std::memcpy(p + fp.Offset + y * fp.Footprint.RowPitch, srcs[i].bytes + y * srcs[i].pitch,
                  srcs[i].pitch);
    }
    upload[i]->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{srcs[i].t->res.Get(),
                                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{upload[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = srcs[i].t->res.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g.list->ResourceBarrier(1, &b);
  }
  g.list->Close();
  ID3D12CommandList* lists[] = {g.list.Get()};
  g.queue->ExecuteCommandLists(1, lists);
  g.queue->Signal(g.fence.Get(), ++g.fence_value);
  g.fence->SetEventOnCompletion(g.fence_value, g.fence_event);
  WaitForSingleObject(g.fence_event, 5000);  // upload buffers die at scope end
  return true;
}

bool EnsureDevice() {
  ComPtr<ID3D12CommandQueue> queue;
  {
    std::lock_guard lock(g_queue_mu);
    queue = g_chain_queue;
  }
  if (!queue) {
    Skip(0, "no command queue captured from the runtime's swap-chain creation");
    return false;
  }
  if (g.queue.Get() == queue.Get() && g.device) return true;
  g = State{};
  g.queue = queue;
  HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&g.device));
  if (FAILED(hr)) return Fail("queue GetDevice", hr);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kFrames * kSrvPerFrame,
                                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srv_heap))))
    return Fail("SRV heap", hr);
  hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrames * kRtvPerFrame, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv_heap))))
    return Fail("RTV heap", hr);
  g.srv_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  g.rtv_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (auto& a : g.alloc) {
    if (FAILED(hr = g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&a))))
      return Fail("allocator", hr);
  }
  if (FAILED(hr = g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0].Get(),
                                              nullptr, IID_PPV_ARGS(&g.list))))
    return Fail("command list", hr);
  g.list->Close();
  if (FAILED(hr = g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
    return Fail("fence", hr);
  g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!BuildRoot() || !UploadLookups()) return false;
  REXLOG_INFO("FSR initialized on the runtime's queue (SMAA lookups uploaded)");
  return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                                  D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

struct Frame {
  UINT f;
  UINT next_srv = 0, next_rtv = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE Srv(UINT i) const {
    auto h = g.srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(f * kSrvPerFrame + i) * g.srv_inc;
    return h;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT i) const {
    auto h = g.srv_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(f * kSrvPerFrame + i) * g.srv_inc;
    return h;
  }
  // A 3-SRV table; unused slots get a null descriptor.
  D3D12_GPU_DESCRIPTOR_HANDLE Table(ID3D12Resource* a, ID3D12Resource* b = nullptr,
                                    ID3D12Resource* c = nullptr) {
    const UINT base = next_srv;
    next_srv += 3;
    ID3D12Resource* rs[3] = {a, b, c};
    for (UINT i = 0; i < 3; ++i) {
      D3D12_SHADER_RESOURCE_VIEW_DESC null_desc{};
      null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      null_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      null_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      null_desc.Texture2D.MipLevels = 1;
      g.device->CreateShaderResourceView(rs[i], rs[i] ? nullptr : &null_desc, Srv(base + i));
    }
    return SrvGpu(base);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(ID3D12Resource* r) {
    auto h = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(f * kRtvPerFrame + next_rtv++) * g.rtv_inc;
    g.device->CreateRenderTargetView(r, nullptr, h);
    return h;
  }
};

// Draws a full-screen triangle into `target` (which the caller has made a
// render target). `clear` zeroes it first (SMAA discards where it has nothing).
void Pass(Frame& fr, ID3D12PipelineState* pso, ID3D12Resource* target, UINT w, UINT h,
          D3D12_GPU_DESCRIPTOR_HANDLE table, const uint32_t* k, bool clear) {
  auto* l = g.list.Get();
  const auto rtv = fr.Rtv(target);
  if (clear) {
    const float zero[4] = {0, 0, 0, 0};
    l->ClearRenderTargetView(rtv, zero, 0, nullptr);
  }
  l->SetPipelineState(pso);
  l->SetGraphicsRootSignature(g.root.Get());
  ID3D12DescriptorHeap* heaps[] = {g.srv_heap.Get()};
  l->SetDescriptorHeaps(1, heaps);
  l->SetGraphicsRootDescriptorTable(0, table);
  l->SetGraphicsRoot32BitConstants(1, 16, k, 0);
  D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
  D3D12_RECT sr{0, 0, LONG(w), LONG(h)};
  l->RSSetViewports(1, &vp);
  l->RSSetScissorRects(1, &sr);
  l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  l->DrawInstanced(3, 1, 0, 0);
}

// Render-target pass on one of our own textures (kept in PIXEL_SHADER_RESOURCE).
void OwnPass(Frame& fr, ID3D12PipelineState* pso, Tex& t, D3D12_GPU_DESCRIPTOR_HANDLE table,
             const uint32_t* k, bool clear) {
  auto b = Transition(t.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  g.list->ResourceBarrier(1, &b);
  Pass(fr, pso, t.res.Get(), t.w, t.h, table, k, clear);
  b = Transition(t.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  g.list->ResourceBarrier(1, &b);
}

void Apply(IDXGISwapChain* sc) {
  if (!REXCVAR_GET(gowj_fsr)) return;
  if (GowjOverlayOpen()) return;  // leave the runtime's ImGui visible
  std::lock_guard lock(g_state_mu);
  if (g.failed || !EnsureDevice() || g.failed) return;
  ComPtr<IDXGISwapChain3> sc3;
  if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return Skip(1, "no IDXGISwapChain3");
  DXGI_SWAP_CHAIN_DESC1 desc{};
  sc3->GetDesc1(&desc);
  if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
      desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
    return Skip(2, "unsupported back-buffer format");
  }
  ComPtr<ID3D12Resource> back;
  if (FAILED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))))
    return Skip(3, "GetBuffer failed");
  const UINT ow = desc.Width, oh = desc.Height;
  ComPtr<ID3D12Resource> guest;
  uint32_t gw = 0, gh = 0;
  {
    std::lock_guard glock(g_guest_mu);
    guest = g_guest;
    gw = g_guest_w;
    gh = g_guest_h;
  }
  if (!guest || !gw || !gh) return Skip(4, "no guest output captured yet");
  if (g.back_format != desc.Format) {
    g.rcas = Pso(g_fsr_vs, sizeof(g_fsr_vs), g_fsr_rcas_ps, sizeof(g_fsr_rcas_ps), desc.Format);
    if (g.failed) return;
    g.back_format = desc.Format;
  }
  // A temporal upscaler already resolves edges; SMAA on top would only soften the picture. The plugin
  // reports a running upscaler as "<name> <mode>: render WxH, output WxH"; a failure has no "render".
  const std::string up_status = rex::cvar::GetFlagByName("gowj_upscale_status");
  const bool smaa = REXCVAR_GET(gowj_smaa) && up_status.find(": render ") == std::string::npos;
  const bool easu = REXCVAR_GET(gowj_fsr_easu) && gw < ow;
  if (!MakeTex(g.inter, ow, oh, kColor, true)) return;
  if (smaa && (!MakeTex(g.edges, gw, gh, DXGI_FORMAT_R8G8_UNORM, true) ||
               !MakeTex(g.weights, gw, gh, DXGI_FORMAT_R8G8B8A8_UNORM, true) ||
               !MakeTex(g.smaa_out, gw, gh, kColor, true))) {
    return;
  }

  Frame fr{g.frame};
  g.frame = (g.frame + 1) % kFrames;
  if (g.frame_fence[fr.f] && g.fence->GetCompletedValue() < g.frame_fence[fr.f]) {
    g.fence->SetEventOnCompletion(g.frame_fence[fr.f], g.fence_event);
    WaitForSingleObject(g.fence_event, 1000);
  }
  g.alloc[fr.f]->Reset();
  g.list->Reset(g.alloc[fr.f].Get(), nullptr);

  // 1. SMAA 1x on the guest output (guest size).
  ID3D12Resource* src = guest.Get();
  uint32_t k[16] = {};
  if (smaa) {
    const float metrics[4] = {1.0f / gw, 1.0f / gh, float(gw), float(gh)};
    std::memcpy(k, metrics, sizeof(metrics));
    OwnPass(fr, g.edge.Get(), g.edges, fr.Table(src), k, true);
    OwnPass(fr, g.weight.Get(), g.weights,
            fr.Table(g.edges.res.Get(), g.area.res.Get(), g.search.res.Get()), k, true);
    OwnPass(fr, g.blend.Get(), g.smaa_out, fr.Table(src, g.weights.res.Get()), k, false);
    src = g.smaa_out.res.Get();
  }
  // 2. Upscale to the back-buffer size: EASU, or bilinear.
  std::memset(k, 0, sizeof(k));
  const D3D12_RESOURCE_DESC sd = src->GetDesc();
  if (easu) {
    FsrEasuCon(reinterpret_cast<AU1*>(&k[0]), reinterpret_cast<AU1*>(&k[4]),
               reinterpret_cast<AU1*>(&k[8]), reinterpret_cast<AU1*>(&k[12]), AF1(gw), AF1(gh),
               AF1(sd.Width), AF1(sd.Height), AF1(ow), AF1(oh));
    OwnPass(fr, g.easu.Get(), g.inter, fr.Table(src), k, false);
  } else {
    OwnPass(fr, g.bilinear.Get(), g.inter, fr.Table(src), k, false);
  }
  // 3. RCAS sharpening into the back buffer.
  std::memset(k, 0, sizeof(k));
  FsrRcasCon(reinterpret_cast<AU1*>(&k[0]),
             AF1(static_cast<float>(REXCVAR_GET(gowj_fsr_sharpness))));
  auto b = Transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  g.list->ResourceBarrier(1, &b);
  Pass(fr, g.rcas.Get(), back.Get(), ow, oh, fr.Table(g.inter.res.Get()), k, false);
  b = Transition(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  g.list->ResourceBarrier(1, &b);
  if (FAILED(g.list->Close())) {
    Fail("command list Close", E_FAIL);
    return;
  }
  ID3D12CommandList* lists[] = {g.list.Get()};
  g.queue->ExecuteCommandLists(1, lists);
  g.queue->Signal(g.fence.Get(), ++g.fence_value);
  g.frame_fence[fr.f] = g.fence_value;

  static std::atomic<uint32_t> last_mode{~0u};
  const uint32_t mode = (smaa ? 1u : 0u) | (easu ? 2u : 0u);
  if (last_mode.exchange(mode) != mode) {
    REXLOG_WARN("FSR active: {}{} {}x{} -> {}x{}, RCAS sharpness {:.2f} stops",
                smaa ? "SMAA + " : "", easu ? "EASU" : "bilinear", gw, gh, ow, oh,
                REXCVAR_GET(gowj_fsr_sharpness));
  }
}

void NoteHookEntered(const char* which) {
  static std::atomic<int> once{1};
  if (once.fetch_sub(1) > 0) {
    REXLOG_INFO("FSR {} hook reached by a swap chain", which);
  }
}

HRESULT STDMETHODCALLTYPE HookPresentChain(IDXGISwapChain* sc, UINT sync, UINT flags) {
  NoteHookEntered("Present");
  if (!(flags & DXGI_PRESENT_TEST)) Apply(sc);
  return g_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1Chain(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                            const DXGI_PRESENT_PARAMETERS* params) {
  NoteHookEntered("Present1");
  if (!(flags & DXGI_PRESENT_TEST)) Apply(sc);
  return g_present1(sc, sync, flags, params);
}

bool PatchSlot(void** vtable, size_t index, void* hook, void** orig) {
  if (vtable[index] == hook) return true;  // already ours
  DWORD old = 0;
  if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
  *orig = vtable[index];
  vtable[index] = hook;
  VirtualProtect(&vtable[index], sizeof(void*), old, &old);
  return true;
}

// Measured 11:46: the probe swap chain's vtable was patched, yet the
// runtime's Present never reached the hook - its swap chain uses another vtable.
// So the swap chain the runtime actually creates is patched at creation, through
// hooks on the factory it is created with.
void PatchSwapChainObject(IUnknown* obj, IUnknown* dev, const char* how) {
  if (!obj || g_probing.load()) return;
  // For D3D12 the CreateSwapChain* "device" argument IS the command queue the
  // chain presents on - the only reliable place to learn it.
  ComPtr<ID3D12CommandQueue> queue;
  if (dev && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&queue)))) {
    std::lock_guard lock(g_queue_mu);
    g_chain_queue = queue;
  }
  ComPtr<IDXGISwapChain1> sc;
  if (FAILED(obj->QueryInterface(IID_PPV_ARGS(&sc)))) return;
  void** vtable = *reinterpret_cast<void***>(sc.Get());
  const bool a = PatchSlot(vtable, 8, reinterpret_cast<void*>(&HookPresentChain),
                           reinterpret_cast<void**>(&g_present));
  const bool b = PatchSlot(vtable, 22, reinterpret_cast<void*>(&HookPresent1Chain),
                           reinterpret_cast<void**>(&g_present1));
  REXLOG_INFO("FSR attached to swap chain via {} (Present={} Present1={} queue={})", how, a, b,
              queue ? "captured" : "MISSING");
}

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*,
                                                      DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                    const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                    IDXGIOutput*, IDXGISwapChain1**);
using CreateForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
                                                           const DXGI_SWAP_CHAIN_DESC1*,
                                                           IDXGIOutput*, IDXGISwapChain1**);
CreateSwapChainFn g_create_sc = nullptr;
CreateForHwndFn g_create_hwnd = nullptr;
CreateForCompositionFn g_create_comp = nullptr;

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* f, IUnknown* dev,
                                              DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
  const HRESULT hr = g_create_sc(f, dev, d, out);
  if (SUCCEEDED(hr) && out) PatchSwapChainObject(*out, dev, "CreateSwapChain");
  return hr;
}
HRESULT STDMETHODCALLTYPE HookCreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w,
                                            const DXGI_SWAP_CHAIN_DESC1* d,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                            IDXGIOutput* o, IDXGISwapChain1** out) {
  const HRESULT hr = g_create_hwnd(f, dev, w, d, fs, o, out);
  if (SUCCEEDED(hr) && out) PatchSwapChainObject(*out, dev, "CreateSwapChainForHwnd");
  return hr;
}
HRESULT STDMETHODCALLTYPE HookCreateForComposition(IDXGIFactory2* f, IUnknown* dev,
                                                   const DXGI_SWAP_CHAIN_DESC1* d,
                                                   IDXGIOutput* o, IDXGISwapChain1** out) {
  const HRESULT hr = g_create_comp(f, dev, d, o, out);
  if (SUCCEEDED(hr) && out) PatchSwapChainObject(*out, dev, "CreateSwapChainForComposition");
  return hr;
}

void PatchFactoryObject(IUnknown* obj) {
  ComPtr<IDXGIFactory2> f;
  if (!obj || FAILED(obj->QueryInterface(IID_PPV_ARGS(&f)))) return;
  void** vt = *reinterpret_cast<void***>(f.Get());
  // IDXGIFactory::CreateSwapChain 10, IDXGIFactory2::CreateSwapChainForHwnd 15,
  // IDXGIFactory2::CreateSwapChainForComposition 24.
  PatchSlot(vt, 10, reinterpret_cast<void*>(&HookCreateSwapChain),
            reinterpret_cast<void**>(&g_create_sc));
  PatchSlot(vt, 15, reinterpret_cast<void*>(&HookCreateForHwnd),
            reinterpret_cast<void**>(&g_create_hwnd));
  PatchSlot(vt, 24, reinterpret_cast<void*>(&HookCreateForComposition),
            reinterpret_cast<void**>(&g_create_comp));
}

using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
CreateFactory2Fn g_real_factory2 = nullptr;
CreateFactory1Fn g_real_factory1 = nullptr;

HRESULT WINAPI HookCreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
  const HRESULT hr = g_real_factory2(flags, riid, out);
  if (SUCCEEDED(hr) && out) PatchFactoryObject(static_cast<IUnknown*>(*out));
  return hr;
}
HRESULT WINAPI HookCreateDXGIFactory1(REFIID riid, void** out) {
  const HRESULT hr = g_real_factory1(riid, out);
  if (SUCCEEDED(hr) && out) PatchFactoryObject(static_cast<IUnknown*>(*out));
  return hr;
}

// Rewrites `module`'s import of dxgi.dll!`name` to `hook`; returns whether found.
bool PatchImport(HMODULE module, const char* name, void* hook) {
  if (!module) return false;
  auto* base = reinterpret_cast<uint8_t*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
       imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "dxgi.dll") != 0) continue;
    auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(
        base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    for (; orig->u1.AddressOfData; ++orig, ++thunk) {
      if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
      auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) != 0) continue;
      DWORD old = 0;
      VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
      thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
      VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
      return true;
    }
  }
  return false;
}

}  // namespace

extern "C" void GowjFsrNoteGuestOutput(ID3D12Resource* resource, uint32_t width,
                                       uint32_t height) {
  std::lock_guard lock(g_guest_mu);
  if (g_guest.Get() != resource || g_guest_w != width || g_guest_h != height) {
    static std::atomic<int> budget{4};
    if (budget.fetch_sub(1) > 0 && resource) {
      const D3D12_RESOURCE_DESC d = resource->GetDesc();
      REXLOG_INFO("FSR guest output {}x{} (texture {}x{} format {})", width, height,
                  d.Width, d.Height, static_cast<int>(d.Format));
    }
  }
  g_guest = resource;
  g_guest_w = width;
  g_guest_h = height;
}

namespace rex::glue {

void InstallFsr() {
  static bool done = false;
  if (done) return;
  done = true;
  // Throwaway D3D12 device + swap chain, only to read DXGI's swap-chain vtable
  // (shared by every swap chain of this implementation, including the runtime's).
  HMODULE d3d12 = LoadLibraryA("d3d12.dll");
  HMODULE dxgi = LoadLibraryA("dxgi.dll");
  using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
  auto create_device = d3d12 ? reinterpret_cast<CreateDeviceFn>(
                                   GetProcAddress(d3d12, "D3D12CreateDevice"))
                             : nullptr;
  auto create_factory = dxgi ? reinterpret_cast<CreateFactoryFn>(
                                   GetProcAddress(dxgi, "CreateDXGIFactory2"))
                             : nullptr;
  if (!create_device || !create_factory) {
    REXLOG_WARN("FSR: d3d12/dxgi entry points missing, not installed");
    return;
  }
  // Attach to the swap chain the runtime really creates: hook factory creation
  // in each module that may create the presenter's factory.
  g_real_factory2 = reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
  g_real_factory1 = reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  int imports = 0;
  const HMODULE modules[] = {GetModuleHandleA("rexruntime.dll"),
                             GetModuleHandleA("rexgpu-xenos.dll"), GetModuleHandleA(nullptr)};
  for (HMODULE m : modules) {
    imports += PatchImport(m, "CreateDXGIFactory2", reinterpret_cast<void*>(&HookCreateDXGIFactory2));
    imports += PatchImport(m, "CreateDXGIFactory1", reinterpret_cast<void*>(&HookCreateDXGIFactory1));
  }
  REXLOG_INFO("FSR factory import hooks installed: {}", imports);
  ComPtr<ID3D12Device> device;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
      FAILED(create_factory(0, IID_PPV_ARGS(&factory)))) {
    REXLOG_WARN("FSR: throwaway device/factory failed, not installed");
    return;
  }
  PatchFactoryObject(factory.Get());  // shared factory vtable, as a backstop
  D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
  HWND hwnd = CreateWindowExW(0, L"STATIC", L"gowj_fsr_probe", WS_OVERLAPPED, 0, 0, 8, 8,
                              nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = 8;
  sd.Height = 8;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> sc;
  g_probing = true;
  HRESULT hr = queue ? factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr,
                                                       nullptr, &sc)
                     : E_FAIL;
  g_probing = false;
  if (SUCCEEDED(hr)) {
    void** vtable = *reinterpret_cast<void***>(sc.Get());
    // IDXGISwapChain::Present is slot 8, IDXGISwapChain1::Present1 is slot 22.
    const bool a = PatchSlot(vtable, 8, reinterpret_cast<void*>(&HookPresentChain),
                             reinterpret_cast<void**>(&g_present));
    const bool b = PatchSlot(vtable, 22, reinterpret_cast<void*>(&HookPresent1Chain),
                             reinterpret_cast<void**>(&g_present1));
    REXLOG_INFO("FSR present hooks installed (Present={} Present1={})", a, b);
  } else {
    REXLOG_WARN("FSR: probe swap chain failed hr={:08x}, not installed",
                static_cast<uint32_t>(hr));
  }
  sc.Reset();
  if (hwnd) DestroyWindow(hwnd);
}

}  // namespace rex::glue
