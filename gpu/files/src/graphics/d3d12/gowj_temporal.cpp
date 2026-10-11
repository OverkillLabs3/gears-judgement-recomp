// Project-local temporal-upscaling support: scene camera capture and frame dumps.
#include <rex/graphics/d3d12/gowj_temporal.h>

#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/d3d12/texture_cache.h>
#include <rex/graphics/register_file.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(gowj_temporal, false, "gfx",
                    "Temporal pipeline: sub-pixel jitter of the scene (foundation for DLSS/FSR)");
REXCVAR_DEFINE_STRING(gowj_upscale_status, "", "gfx",
                      "Upscaler status line shown by the FPS overlay (set by the GPU plugin)");
REXCVAR_DEFINE_DOUBLE(gowj_jitter_scale, 1.0, "gfx",
                      "Multiplier on the jitter offsets (debug: a large value makes jitter visible)");
REXCVAR_DEFINE_INT32(gowj_jitter_phases, 8, "gfx", "Length of the Halton jitter sequence");

namespace rex::graphics::d3d12::gowj {

namespace {

struct Readback {
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  ID3D12Resource* source = nullptr;
  D3D12_RESOURCE_STATES state =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 row_size = 0;
  uint64_t submission = 0;
  std::string path;
  uint32_t width = 0, height = 0;
  uint32_t format = 0;
};

FrameState g_state;
std::vector<std::unique_ptr<Readback>> g_pending;
int g_dump_frames_left = 0;      // frames still to capture after the trigger
bool g_dump_this_frame = false;  // capture during the current frame
bool g_depth_dumped = false, g_color_dumped = false;
uint32_t g_dump_seq = 0;

void CopyCallback(ID3D12GraphicsCommandList* list, void* user) {
  auto* rb = static_cast<Readback*>(user);
  const D3D12_RESOURCE_STATES kSrv = rb->state;
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = rb->source;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = kSrv;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &b);
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = rb->buffer.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = rb->footprint;
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = rb->source;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
  list->ResourceBarrier(1, &b);
}

void QueueCapture(D3D12CommandProcessor& cp, ID3D12Resource* resource, const char* what,
                  D3D12_RESOURCE_STATES state =
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
  ID3D12Device* device = cp.GetD3D12Provider().GetDevice();
  D3D12_RESOURCE_DESC desc = resource->GetDesc();
  auto rb = std::make_unique<Readback>();
  UINT64 total = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &rb->footprint, &rb->rows, &rb->row_size, &total);
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb->buffer)))) {
    REXGPU_WARN("GOWJ dump: readback buffer creation failed");
    return;
  }
  rb->source = resource;
  rb->state = state;
  rb->width = uint32_t(desc.Width);
  rb->height = desc.Height;
  rb->format = uint32_t(desc.Format);
  char name[96];
  std::snprintf(name, sizeof name, "gowj_dump/f%u_%s.bin", g_dump_seq, what);
  rb->path = name;
  cp.SubmitBarriers();
  rb->submission = cp.GetCurrentSubmission();
  Readback* raw = rb.get();
  g_pending.push_back(std::move(rb));
  cp.GetDeferredCommandList().Callback(&CopyCallback, raw);
  REXGPU_WARN("GOWJ dump: queued {} {}x{} dxgi={} -> {}", what, raw->width, raw->height,
              raw->format, raw->path);
}

void WriteCamera(uint32_t seq) {
  char name[96];
  std::snprintf(name, sizeof name, "gowj_dump/f%u_cam.txt", seq);
  FILE* f = std::fopen(name, "w");
  if (!f) return;
  auto dump = [&](const char* tag, const SceneCamera& c) {
    std::fprintf(f, "%s valid=%d matching=%u other=%u\n", tag, int(c.valid), c.draws_matching,
                 c.draws_other);
    for (int i = 0; i < 16; ++i)
      std::fprintf(f, "%s c%d %.9g %.9g %.9g %.9g\n", tag, i, c.c[i][0], c.c[i][1], c.c[i][2],
                   c.c[i][3]);
  };
  dump("cur", g_state.camera);
  dump("prev", g_state.prev_camera);
  std::fprintf(f, "scale %u %u\ndepth_dst %08X\ncolor_dst %08X\n", g_state.scale_x,
               g_state.scale_y, g_state.depth_resolve_dst, g_state.color_resolve_dst);
  std::fclose(f);
}

void FlushCompleted(D3D12CommandProcessor& cp) {
  for (size_t i = 0; i < g_pending.size();) {
    Readback& rb = *g_pending[i];
    if (cp.GetCompletedSubmission() < rb.submission) {
      ++i;
      continue;
    }
    void* mapped = nullptr;
    D3D12_RANGE range = {0, size_t(rb.footprint.Footprint.RowPitch) * rb.rows};
    if (SUCCEEDED(rb.buffer->Map(0, &range, &mapped))) {
      std::filesystem::create_directories("gowj_dump");
      FILE* f = std::fopen(rb.path.c_str(), "wb");
      if (f) {
        uint32_t hdr[6] = {rb.width, rb.height, rb.format, rb.footprint.Footprint.RowPitch,
                           rb.rows, 0};
        std::fwrite(hdr, sizeof hdr, 1, f);
        std::fwrite(mapped, 1, size_t(rb.footprint.Footprint.RowPitch) * rb.rows, f);
        std::fclose(f);
      }
      D3D12_RANGE none = {0, 0};
      rb.buffer->Unmap(0, &none);
    }
    g_pending.erase(g_pending.begin() + i);
  }
}

}  // namespace

FrameState& State() { return g_state; }

void CaptureOutputIfRequested(D3D12CommandProcessor& cp, ID3D12Resource* output,
                              D3D12_RESOURCE_STATES state) {
  static uint32_t counter = 0, shot_seq = 0;
  if ((++counter % 6) != 0) return;
  std::error_code ec;
  if (!std::filesystem::exists("gowj_shot.now", ec)) return;
  std::filesystem::remove("gowj_shot.now", ec);
  std::filesystem::create_directories("gowj_dump");
  char what[32];
  std::snprintf(what, sizeof what, "shot%u", shot_seq++);
  uint32_t keep = g_dump_seq;
  g_dump_seq = 0;
  // QueueCapture names the file f<seq>_<what>.bin
  QueueCapture(cp, output, what, state);
  g_dump_seq = keep;
}

void BeginFrame(D3D12CommandProcessor& cp, uint32_t front_ptr) {
  FlushCompleted(cp);
  // Close out the previous frame's dump (camera text needs both cameras).
  if (g_dump_this_frame) {
    WriteCamera(g_dump_seq);
    ++g_dump_seq;
    g_dump_this_frame = false;
  }
  g_state.ended_camera = g_state.camera;
  g_state.ended_prev_camera = g_state.prev_camera;
  g_state.ended_frame = g_state.frame;
  g_state.ended_jittered = g_state.jitter_this_frame;
  g_state.prev_camera = g_state.camera;
  g_state.camera = SceneCamera();
  g_state.depth_resolve_dst = 0;
  g_state.depth_dst_count = 0;
  g_state.color_resolve_dst = 0;
  g_state.last_front = front_ptr;
  ++g_state.frame;
  g_state.shrink_this_frame = false;  // the whole guest frame is rendered at the render size now
  g_state.jitter_this_frame = g_state.jitter_next;
  g_state.jitter_next = false;

  if (g_dump_frames_left <= 0 && (g_state.frame % 30) == 0) {
    std::error_code ec;
    if (std::filesystem::exists("gowj_dump.now", ec)) {
      std::filesystem::remove("gowj_dump.now", ec);
      g_dump_frames_left = 2;
      g_dump_seq = 0;
    }
  }
  if (g_dump_frames_left > 0) {
    --g_dump_frames_left;
    g_dump_this_frame = true;
    g_depth_dumped = g_color_dumped = false;
    std::filesystem::create_directories("gowj_dump");
  }
}

void NoteResolve(bool is_depth, uint32_t dst, uint32_t width, uint32_t height, uint32_t) {
  if (is_depth) {
    g_state.depth_resolve_dst = dst;
    bool known = false;
    for (uint32_t i = 0; i < g_state.depth_dst_count; ++i) known |= g_state.depth_dsts[i] == dst;
    if (!known && g_state.depth_dst_count < 4) g_state.depth_dsts[g_state.depth_dst_count++] = dst;
  } else if (dst != g_state.last_front) {
    g_state.color_resolve_dst = dst;
    g_state.guest_w = width;
    g_state.guest_h = height;
  }
}

bool Enabled() { return REXCVAR_GET(gowj_temporal) || SelectedUpscaler() != 0; }

void SetUpscaleStatus(const char* text) {
  static std::string last = "\x01";
  if (last == text) return;
  last = text;
  const bool ok = rex::cvar::SetFlagByName("gowj_upscale_status", last);
  REXGPU_WARN("GOWJ upscale status -> '{}' (set ok={}, read back '{}')", last, ok,
              rex::cvar::GetFlagByName("gowj_upscale_status"));
}

bool ShouldJitter(uint64_t vs_float_bitmap0, uint32_t surface_pitch, uint32_t color_fmt) {
  if (!Enabled() || !g_state.jitter_this_frame) return false;
  if ((vs_float_bitmap0 & 0xF) != 0xF) return false;                    // not a world VS
  if (!g_state.guest_w || surface_pitch != g_state.guest_w) return false;  // not the scene surface
  (void)color_fmt;
  return true;
}

static double Halton(uint32_t i, uint32_t b) {
  double f = 1.0, r = 0.0;
  while (i > 0) {
    f /= b;
    r += f * (i % b);
    i /= b;
  }
  return r;
}

void JitterPixels(float out[2]) { JitterPixelsForFrame(g_state.frame, out); }

void JitterPixelsForFrame(uint32_t frame, float out[2]) {
  int phases = std::max(1, REXCVAR_GET(gowj_jitter_phases));
  uint32_t i = (frame % uint32_t(phases)) + 1;
  double s = REXCVAR_GET(gowj_jitter_scale);
  out[0] = float((Halton(i, 2) - 0.5) * s);
  out[1] = float((Halton(i, 3) - 0.5) * s);
}

void JitterNdc(uint32_t host_w, uint32_t host_h, float out[2]) {
  float px[2];
  JitterPixels(px);
  out[0] = 2.0f * px[0] / float(host_w);
  out[1] = -2.0f * px[1] / float(host_h);
}

void NoteSceneDraw(const float* vs_consts, uint32_t color_fmt, uint32_t index_count) {
  if (color_fmt != 3 || index_count < 30) return;
  for (int i = 0; i < 16; ++i) {
    for (int k = 0; k < 4; ++k)
      if (!std::isfinite(vs_consts[i * 4 + k]) || std::fabs(vs_consts[i * 4 + k]) > 1e7f) return;
  }
  SceneCamera& cam = g_state.camera;
  if (!cam.valid) {
    std::memcpy(cam.c, vs_consts, sizeof cam.c);
    cam.valid = true;
    cam.draws_matching = 1;
    return;
  }
  if (std::memcmp(cam.c, vs_consts, 4 * 4 * sizeof(float)) == 0)
    ++cam.draws_matching;
  else
    ++cam.draws_other;
}

int FindCameraMatrix(const float* vs_consts) {
  const SceneCamera& cam = g_state.camera;
  if (!cam.valid) return -1;
  for (int r = 0; r + 4 <= 256; ++r) {
    if (std::memcmp(cam.c, vs_consts + r * 4, 4 * 4 * sizeof(float)) == 0) return r;
  }
  return -1;
}

void OnDrawTextures(D3D12CommandProcessor& cp, D3D12TextureCache& tc, const RegisterFile& regs,
                    uint32_t used_mask) {
  const bool upscale = UpscaleEnabled();
  if (!upscale && (!g_dump_this_frame || (g_depth_dumped && g_color_dumped))) return;
  for (uint32_t i = 0; i < 32; ++i) {
    if (!(used_mask & (1u << i))) continue;
    xenos::xe_gpu_texture_fetch_t tf = regs.GetTextureFetch(i);
    uint32_t base = uint32_t(tf.base_address) << 12;
    if (upscale) {
      bool is_depth_dst = false;
      for (uint32_t k = 0; k < g_state.depth_dst_count; ++k) is_depth_dst |= base == g_state.depth_dsts[k];
      if (is_depth_dst) {
        uint32_t w, h;
        if (ID3D12Resource* r = tc.GetActiveTextureResource(i, &w, &h)) NoteDepthTexture(r);
      } else if (g_state.color_resolve_dst && base == g_state.color_resolve_dst) {
        uint32_t w, h;
        (void)w;
        (void)h;
      }
    }
    if (!g_dump_this_frame || (g_depth_dumped && g_color_dumped)) continue;
    bool want_depth = !g_depth_dumped && g_state.depth_resolve_dst && base == g_state.depth_resolve_dst;
    bool want_color = !g_color_dumped && g_state.color_resolve_dst && base == g_state.color_resolve_dst;
    if (!want_depth && !want_color) continue;
    uint32_t w = 0, h = 0;
    ID3D12Resource* res = tc.GetActiveTextureResource(i, &w, &h);
    if (!res) continue;
    QueueCapture(cp, res, want_depth ? "depth" : "color");
    (want_depth ? g_depth_dumped : g_color_dumped) = true;
  }
}

}  // namespace rex::graphics::d3d12::gowj
