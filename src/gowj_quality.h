// gowj - capability-driven quality tier + frame-rate auto-tuner (see gowj_quality.cpp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::glue {

// The temporal upscalers (DLSS, FSR 3.1, FSR 4) are switched off for this release: the picture flickers while
// moving at the render sizes the game can draw, and the work is paused until that is fixed. The code stays;
// set this to true to bring them back (the launcher card and the libraries need restoring too).
inline constexpr bool kTemporalUpscalers = false;

struct QualityPlan {
  int tier = 1;  // 0 low (handheld / integrated), 1 medium, 2 high
  const char* tier_name = "medium";
  int scale = 1;  // draw_resolution_scale_x/y
  bool low_end = false;    // Low tier: apply the lighter engine settings (see gowj_low_end.h)
  std::string upscaler_override;  // non-empty: use this upscaler instead of the configured one (REX_GOWJ_UPSCALER)
  bool upscaler = false;   // a temporal upscaler (DLSS / FSR) is selected in the config
  int upscale_mode = 2;    // 2 quality (guest frame 1440p), 0 performance (1080p), 3 ultra performance (720p)
  uint32_t render_final_h = 0;  // intended render height of the upscaler input (log only)
  int render_w = 0, render_h = 0;  // guest render target override for the upscaler (0 = the guest's 720p)
  bool smaa = true;
  bool fsr = true;
  int aniso = 4;  // anisotropic_override (3 = 4x, 4 = 8x, 5 = 16x)
  bool msaa2x = true;
  int tex_soft_mb = 1024, tex_hard_mb = 2048, tex_rt_mb = 128;
  int pso_threads = 2;
  bool rov = false;  // render_target_path_d3d12 = "rov" (else runtime auto)
  // What the probe saw.
  std::string gpu_name;
  uint32_t vendor = 0;
  uint64_t vram_mb = 0, budget_mb = 0;
  bool uma = false, rov_supported = false;
  unsigned cores = 0;
  std::string source;  // "auto", "override:<x>", "tuned(-N)"
  std::string reason;
  std::string summary;  // one log line describing the decision (logging is not up yet when it is made)
};

// Probes the adapter the runtime will use (cvar d3d12_adapter, else the most-VRAM one),
// and picks a tier. Call before the GPU initializes (ApplyDisplayDefaults).
// `config_path` is read directly (gowj_quality, d3d12_adapter) because the config file is only
// applied to the settings system after the point where the plan is needed.
QualityPlan ChooseQualityPlan(uint32_t monitor_height, const std::filesystem::path& config_path = {},
                              uint32_t monitor_width = 0);

// Called once the config file is known: only a tier-chosen resolution (not one the user
// wrote) may be changed by the tuner.
void QualityStartTuner(const QualityPlan& plan, bool managed);
// Draw-distance steps the tuner has saved for this GPU (0..2); each step multiplies the distance by 0.8.
int QualityDrawDistanceSteps(const std::string& gpu);

// Per guest frame (refresh stream), from the PERFWATCH hooks.
void QualityNoteFrame(uint64_t interval_us);

// Text the FPS overlay shows as its last line ("" = nothing).
std::string QualityOverlayLine();

}  // namespace rex::glue
