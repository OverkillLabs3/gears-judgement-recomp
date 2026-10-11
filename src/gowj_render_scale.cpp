/**
 * @file        gowj_render_scale.cpp
 * @brief       Host-controlled guest render resolution
 * @copyright   BSD 3-Clause License
 */

#include "gowj_render_scale.h"

#include <rex/cvar.h>
#include <rex/logging.h>

// 0 = leave the guest's own computed value alone (its 1280x720 default).
REXCVAR_DEFINE_INT32(gowj_render_width, 0, "Display",
                     "Render width the guest engine allocates its back buffer at "
                     "(0 = the game's own 720p default). This is the guest's real "
                     "render resolution, not an upscale: the engine's whole render "
                     "pipeline derives its target sizes from it.");
REXCVAR_DEFINE_INT32(gowj_render_height, 0, "Display",
                     "Render height the guest engine allocates its back buffer at "
                     "(0 = the game's own 720p default).");

namespace {
// Log the first substitution per axis, so a boot either proves the override
// reached the guest or proves it never fired - a silent no-op here is exactly the
// kind of change that previously let a build claim a resolution it did not have.
void LogOnce(bool& logged, const char* what, uint32_t from, uint32_t to) {
  if (logged) return;
  logged = true;
  REXLOG_WARN("gowj RESSCALE guest {} {} -> {}", what, from, to);
}

// What the engine itself computed, remembered on every store so the reads that
// follow in the SAME guest function can still ask the original question.
// sub_82990A28 ends with `if (stored_width != 1280) { ... = 800x800 ... }` - a
// stock-vs-nonstandard guard. Once the store is overridden that guard fires for
// every custom resolution and puts the engine on its fallback path, so the
// comparison has to be fed the pre-override value (measured with the
// guard live, a 2560x1440 target showed the frame tiled H/720 times).
uint32_t g_guest_width = 0;
uint32_t g_guest_height = 0;
}  // namespace

extern "C" uint32_t GowjGuestRenderWidth(uint32_t guest_value) {
  g_guest_width = guest_value;
  const int32_t w = REXCVAR_GET(gowj_render_width);
  static bool logged = false;
  if (w < 256) return guest_value;
  LogOnce(logged, "width", guest_value, static_cast<uint32_t>(w));
  return static_cast<uint32_t>(w);
}

extern "C" uint32_t GowjGuestRenderHeight(uint32_t guest_value) {
  g_guest_height = guest_value;
  const int32_t h = REXCVAR_GET(gowj_render_height);
  static bool logged = false;
  if (h < 128) return guest_value;
  LogOnce(logged, "height", guest_value, static_cast<uint32_t>(h));
  return static_cast<uint32_t>(h);
}

extern "C" uint32_t GowjGuestOriginalWidth(uint32_t as_stored) {
  return g_guest_width ? g_guest_width : as_stored;
}

extern "C" uint32_t GowjGuestOriginalHeight(uint32_t as_stored) {
  return g_guest_height ? g_guest_height : as_stored;
}
