/**
 * @file        gowj_render_scale.h
 * @brief       Host-controlled guest render resolution
 *
 * Measured: sub_82990A28 (gowj_recomp.206.cpp:16700) is the ONLY writer
 * of the engine's two render-size globals. It stores a literal 720 into the height
 * word and clamps the width it computes into [904, 1280]. Everything else in the
 * title - the ~40 readers of those words, the present parameters built moments
 * later in the same function, and sub_82BD8178, which actually issues the
 * "Created tiled 1280x720x1 2D k_8_8_8_8" back buffer - derives from that pair.
 *
 * That is why no runtime cvar can raise this game's resolution. Setting
 * video_mode_width/height=1920x1080 demonstrably reaches the guest (confirmed in
 * the cvar read-back with source=cli) and is then overwritten here, so the guest
 * still allocates 1280x720.
 *
 * draw_resolution_scale DOES rasterize and resolve at NxN - an earlier note here
 * claimed it "resolves back down into the fixed 720p target", which was read off
 * a log line that printed the env default instead of the applied value. It is the
 * route this project ships, because the alternative fails: pushing this file's
 * own override past the Xenos EDRAM band budget (~2.62 Mpixel at 32bpp) makes the
 * guest's final resolve draw N shrunken diagonal copies of the frame instead of
 * one image (measured: 3840x2160 and 2560x1440 both shatter, 1920x1080 is clean),
 * which caps the guest-target route at 1080p. At scale 3 the guest still bands
 * 1280x720 correctly and the panel gets a real 3840x2160 grid.
 *
 * patch_render_resolution.py therefore routes the two stores through the
 * accessors below, making the guest's own decision a config value. Returning the
 * guest's computed value when the override is unset keeps behaviour identical to
 * an unpatched build.
 *
 * The same function then asks `if (stored_width != 1280)` and, on the non-stock
 * branch, writes 800x800 into a second global pair - so a custom resolution
 * silently puts the engine on its fallback path. GowjGuestOriginalWidth/Height
 * exist for exactly that comparison: they answer with what the engine computed
 * before the override, so the guard keeps asking the question it was written for.
 *
 * @copyright   Copyright (c) 2026
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>

extern "C" {

// Width/height the guest should use for its render target, given the value the
// guest computed for itself. Returns the override when one is configured,
// otherwise guest_value.
uint32_t GowjGuestRenderWidth(uint32_t guest_value);
uint32_t GowjGuestRenderHeight(uint32_t guest_value);

// The value the guest computed for itself on the most recent store, so a
// stock-resolution guard downstream of the override still sees the guest's own
// decision. Falls back to guest_value before the first store.
uint32_t GowjGuestOriginalWidth(uint32_t guest_value);
uint32_t GowjGuestOriginalHeight(uint32_t guest_value);

}
