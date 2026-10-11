#pragma once
// Project-local temporal-upscaling support for the D3D12 backend (Gears of War: Judgment).
//
// What this module knows about the game (measured on the running game):
//   * One frame = ~2000 scene draws into one HDR render target, then a depth resolve, particle draws
//     that sample the resolved depth, a colour resolve (scene colour texture), and one full-screen
//     tonemap draw that samples only that colour texture, then the HUD.
//   * Scene vertex shaders read the ViewProjection matrix from vertex float constants c0..c3.
#include <cstdint>
#include <cstdio>

#include <rex/graphics/d3d12/gowj_trace.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics {
class RegisterFile;
}

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;
class D3D12TextureCache;

namespace gowj {

// Floats of VS constants c0..c15 captured from the first qualifying scene draw of a frame.
struct SceneCamera {
  bool valid = false;
  float c[16][4] = {};
  uint32_t draws_matching = 0;  // later draws of the frame whose c0..c3 equal the captured ones
  uint32_t draws_other = 0;     // qualifying draws whose c0..c3 differ
};

struct FrameState {
  uint32_t frame = 0;
  SceneCamera camera;       // this frame
  SceneCamera prev_camera;  // previous frame
  uint32_t depth_resolve_dst = 0;
  uint32_t depth_dsts[4] = {};  // every depth resolve destination of this frame
  uint32_t depth_dst_count = 0;
  uint32_t color_resolve_dst = 0;  // scene colour (non-frontbuffer colour resolve)
  uint32_t last_front = 0;
  uint32_t scale_x = 1, scale_y = 1;
  uint32_t last_upscaled_frame = 0xFFFFFFFFu;
  // The frame that just ended (BeginFrame runs at the swap, after its draws): its camera, the camera
  // before it and the frame number its jitter was taken from. The end-of-frame upscaler uses these.
  SceneCamera ended_camera, ended_prev_camera;
  uint32_t ended_frame = 0;
  // Sub-pixel jitter is applied only while the previous frame went through the upscaler, so menus
  // and loading screens are left alone.
  bool ended_jittered = false;  // whether the frame that just ended was jittered
  bool jitter_this_frame = false;
  bool jitter_next = false;  // set by a successful end-of-frame upscale: jitter the next frames
  // Decided once per frame (BeginFrame): rasterise world geometry into the render region only
  // when the previous frame went through the upscaler, so menus and loading screens are left alone.
  bool shrink_this_frame = false;
  uint32_t guest_w = 0, guest_h = 0;  // guest scene size (colour resolve size), persists across frames
};

FrameState& State();

// Dump capture: create "gowj_dump.now" in the working directory. The next two frames write
// gowj_dump/f<k>_{depth,color}.bin and f<k>_cam.txt.
void BeginFrame(D3D12CommandProcessor& cp, uint32_t front_ptr);
void NoteResolve(bool is_depth, uint32_t dst, uint32_t width, uint32_t height, uint32_t fmt);
// Called for each draw with a pixel shader. vs_consts: pointer to 4*256 floats of VS constants.
void NoteSceneDraw(const float* vs_consts, uint32_t color_fmt, uint32_t index_count);
// Text for the FPS overlay (cvar gowj_upscale_status); only touches the cvar when the text changes.
void SetUpscaleStatus(const char* text);
// Create "gowj_shot.now": the next presented frame's final guest output is written to
// gowj_dump/f0_shot<N>.bin (R10G10B10A2, same header as the other dumps). Headless screenshots.
void CaptureOutputIfRequested(D3D12CommandProcessor& cp, ID3D12Resource* output,
                              D3D12_RESOURCE_STATES state);
// Master switch (cvar gowj_temporal).
bool Enabled();
// True for draws that should be sub-pixel jittered: world-geometry vertex shaders (read c0..c3,
// the ViewProjection matrix) drawing into the scene target (surface pitch == guest scene width).
// vs_float_bitmap0: first 64 bits of the vertex shader's float constant bitmap.
bool ShouldJitter(uint64_t vs_float_bitmap0, uint32_t surface_pitch, uint32_t color_fmt);
// Jitter for this frame in NDC units (add to the NDC offset); host_w/h = host render size.
void JitterNdc(uint32_t host_w, uint32_t host_h, float out[2]);
// Jitter for this frame in render-target pixels, as handed to an upscaler (x right, y down).
void JitterPixels(float out[2]);
void JitterPixelsForFrame(uint32_t frame, float out[2]);
// Render-resolution scale of the scene (1.0 = native). World geometry is rasterised into the
// top-left (fx*W, fy*H) region of the target and the upscaler reconstructs the full size.
// Only active while the upscaler is usable. out_w/out_h = full host target size.
bool RenderScale(uint32_t out_w, uint32_t out_h, float* fx, float* fy, uint32_t* rw, uint32_t* rh);
// 0 = off, 1 = DLSS, 2 = FSR 3.1, 3 = FSR 4 (cvar gowj_upscaler).
int SelectedUpscaler();
// Upscaler (DLSS) hooks. UpscaleEnabled(): cvar gowj_dlss && temporal pipeline on.
bool UpscaleEnabled();
void NoteDepthTexture(ID3D12Resource* depth);
// End-of-frame upscaling. Called at the swap with the finished guest frame (colour: the frontbuffer
// texture, in_w x in_h active pixels). On success the upscaled picture is in `out` (output size,
// R10G10B10A2, ready to be sampled) and the presenter should use it instead of the guest frame.
struct FinalImage {
  ID3D12Resource* resource = nullptr;
  uint32_t w = 0, h = 0;
};
bool UpscaleFinal(D3D12CommandProcessor& cp, ID3D12Resource* color, uint32_t in_w, uint32_t in_h,
                  FinalImage& out);
// Index of the first VS constant register at which 4 consecutive rows equal this frame's camera
// matrix (0..252), or -1.
int FindCameraMatrix(const float* vs_consts);
// Called after textures for the draw were requested. fetch constants listed in used_mask.
void OnDrawTextures(D3D12CommandProcessor& cp, D3D12TextureCache& tc,
                    const RegisterFile& regs, uint32_t used_mask);

}  // namespace gowj
}  // namespace rex::graphics::d3d12
