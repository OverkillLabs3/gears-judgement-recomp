// gowj keyboard+mouse -> pad driver implementation. See header for rationale.

#include "gowj_kbm_input.h"
#include "gowj_native_input.h"
#include "gowj_view_accel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>

#ifdef _WIN32
// ClipCursor for the pointer-lock clip; windows.h must precede nothing here,
// but must not leak into the header.
#include <windows.h>
#endif

#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>

REXCVAR_DECLARE(std::string, keybind_a);
REXCVAR_DECLARE(std::string, keybind_b);
REXCVAR_DECLARE(std::string, keybind_x);
REXCVAR_DECLARE(std::string, keybind_y);
REXCVAR_DECLARE(std::string, keybind_start);
REXCVAR_DECLARE(std::string, keybind_back);
REXCVAR_DECLARE(std::string, keybind_guide);
REXCVAR_DECLARE(std::string, keybind_left_shoulder);
REXCVAR_DECLARE(std::string, keybind_right_shoulder);
REXCVAR_DECLARE(std::string, keybind_left_trigger);
REXCVAR_DECLARE(std::string, keybind_right_trigger);
REXCVAR_DECLARE(std::string, keybind_lstick_press);
REXCVAR_DECLARE(std::string, keybind_rstick_press);
REXCVAR_DECLARE(std::string, keybind_lstick_up);
REXCVAR_DECLARE(std::string, keybind_lstick_down);
REXCVAR_DECLARE(std::string, keybind_lstick_left);
REXCVAR_DECLARE(std::string, keybind_lstick_right);
REXCVAR_DECLARE(std::string, keybind_rstick_up);
REXCVAR_DECLARE(std::string, keybind_rstick_down);
REXCVAR_DECLARE(std::string, keybind_rstick_left);
REXCVAR_DECLARE(std::string, keybind_rstick_right);
REXCVAR_DECLARE(std::string, keybind_dpad_up);
REXCVAR_DECLARE(std::string, keybind_dpad_down);
REXCVAR_DECLARE(std::string, keybind_dpad_left);
REXCVAR_DECLARE(std::string, keybind_dpad_right);
REXCVAR_DECLARE(double, mnk_sensitivity);

// Mouse-look model - TRUE RAW MOUSE, and why.
//
// The guest integrates its camera rotation ONCE PER INPUT POLL from the right-
// stick deflection it read that poll. So the turn a physical mouse movement
// produces is sum(deflection_i * guest_step) over the polls it spans. For that
// sum to equal (mouse distance * gain) - the raw PC contract, where a 2-inch
// movement turns the same angle whether the machine runs 30 or 300 fps and
// whether the hand moved slowly or flicked - the delivered deflections must
// account for every pixel, with no time term and no lost remainder.
//
// That is what the stick-unit debt in GetDeviceState does. The earlier model
// here mapped deflection proportionally to one poll's pixels and clamped it,
// which silently deleted the over-range part of every fast flick: slow drags
// and hard flicks turned different angles for the same distance. Before that,
// a velocity model (pixels/dt) made the turn frame-rate coupled, which is the
// "mouse feels even worse with unlocked fps" report.
//
// Momentum: none. The debt is paid out in full to within half a stick unit, so
// the frame the hand stops the deflection is 0. The retired "ice slide" came
// from an old decay timer draining past motion into later frames, not from
// carrying a debt.
REXCVAR_DEFINE_DOUBLE(mnk_full_deflect_pixels, 5.0, "mnk",
                      "Mouse pixels that map to one full-deflection guest poll. "
                      "This is the turn gain: lower turns farther per pixel. It "
                      "also sets the maximum turn rate (full deflection at the "
                      "guest's poll rate); motion above that rate is spread over "
                      "later polls, never dropped.");
REXCVAR_DEFINE_INT32(mnk_look_model, 0, "mnk",
                     "0 = raw pixel-accounting look (frame-rate independent, default). "
                     "1 = saturating proportional model, for A/B feel measurement only.");
REXCVAR_DEFINE_DOUBLE(mnk_yaw_scale, 1.0, "mnk",
                      "Horizontal mouse-look multiplier.");
// The guest's pitch response per stick-unit is weaker than its yaw, so a 1:1
// scale made up/down feel slower than left/right (player report). 1.5 evens the
// two axes out; tune per-axis if a display's aspect makes one feel off again.
REXCVAR_DEFINE_DOUBLE(mnk_pitch_scale, 1.5, "mnk",
                      "Vertical mouse-look multiplier.");
REXCVAR_DEFINE_BOOL(mnk_invert_y, false, "mnk", "Flip vertical mouse-look.");
REXCVAR_DEFINE_BOOL(mnk_fps_meter, true, "mnk",
                    "Log the guest's right-stick poll rate once per second.");
// The guest's own low-pass on every thumbstick axis (sub_8299C3D0, alpha at guest
// 0x82082928, measured 0.5). It exists because a stick a hand rests on drifts; a
// mouse does not, so for mouse look it is only lag. patch_mouse_smoothing.py
// reroutes that alpha load through GowjInputSmoothAlpha below, which by default
// makes the filter an identity. Set true to get the console behaviour back.
REXCVAR_DEFINE_BOOL(mnk_guest_smoothing, false, "mnk",
                    "Keep the guest's thumbstick low-pass filter on. False (default) "
                    "removes it so mouse look reaches the camera unfiltered.");

// Called from the recompiled guest (see tools/patch_mouse_smoothing.py).
// Returning 1.0 makes out = prev + 1.0*(new-prev) = new: the deflection we hand
// the guest is exactly the deflection the camera sees.
extern "C" float GowjInputSmoothAlpha(float guest_alpha) {
  return REXCVAR_GET(mnk_guest_smoothing) ? guest_alpha : 1.0f;
}

// Raw mouse look, the mandatory half of "the mouse is not an emulated stick".
//
// The thumbstick channel is bounded twice over: a deflection cannot exceed
// kStickMax, and the camera integrates angle as sum(deflection * dt). So a flick
// that outruns the console's max turn rate cannot be expressed in the polls it
// actually happened over - the debt in GetDevice carries it into later polls and
// the view keeps moving after the hand stops. That tail is not a tuning problem,
// it is what a rate-limited channel does, and no gain or decay constant removes
// it. The guest patcher (tools/patch_raw_look.py) reroutes the two
// look-axis accumulators through GowjRawLookUnits below so the host can add the
// movement of the whole frame in one dt-free step.
//
// Units are deliberately stick-units (deflection * 1.0, i.e. what one poll of
// full deflection would have contributed) because that is what the accumulator
// already holds: the same pixels-per-degree gain as the stick path, minus the
// ceiling. Slow aim therefore feels unchanged; only fast flicks stop lagging.
// Default TRUE: the guest half is in the build (patch_raw_look.py, wired as the
// rawlook.build.stamp edge). It is safe to default on because GetDeviceState does
// not trust this cvar to know whether the hook is live - it asks the consumer's
// own call history (RawLookConsumerLive). So an exe built without the patch, and
// a menu screen where the look dispatch is not running, both fall back to the
// stick path instead of shipping a dead mouse.
REXCVAR_DEFINE_BOOL(mnk_raw_look, true, "mnk",
                    "Look is a direct angle delta, not an emulated thumbstick. "
                    "Off restores the rate-limited stick path (the console feel).");
// 2 (default) is HYBRID: a move is delivered as an analog deflection
// (angle/dt, which the engine multiplies back by the real dt), so normal aiming
// never touches the time argument at all and never saturates. Only a move that
// demands more turn than the engine's own max rate can express in one frame
// falls back to pinning the axis at full deflection and stretching the time
// argument, because there is no other way to say it. 1 = always stretch the time
// argument: it is exact, but it lies about dt on EVERY mouse move, and dt is the
// engine's own integration step - so slow aiming got a 5x-too-short frame and
// hard flicks a 100x-too-long one, which is what "way too sensitive and slidey"
// is. 0 = the first attempt, which added the angle to the axis argument and was
// clamped back to full deflection. 1 and 0 stay selectable so the difference can
// be measured against the same exe instead of remembered.
REXCVAR_DEFINE_INT32(mnk_raw_look_mode, 2, "mnk",
                     "2 = analog rate, stretch dt only past the engine's max "
                     "rate (raw, default). 1 = always angle-in-dt. "
                     "0 = angle-in-axis (saturates; A/B only).");
// Mode's gain in the ENGINE's own angle unit: 1.0 == one full-deflection second.
// The stick knobs (mnk_sensitivity, mnk_full_deflect_pixels, per-axis scale) used
// to set this through the stick-unit bank; the bank is now plain mouse pixels, so
// raw look has exactly one gain and it is not coupled to the gamepad path.
// 0.003 puts the engine's full-deflection rate at ~333 px/s of hand speed, which
// is the point where the hybrid starts spilling into the time argument.
REXCVAR_DEFINE_DOUBLE(mnk_raw_look_scale, 0.003, "mnk",
                      "Raw mouse-look gain: engine turn units per mouse pixel. "
                      "Higher turns farther per pixel. F11/F12 change it live.");
// The menu cursor is driven by the SAME two patched sites, so raw look would fling
// it. Measured on the guest's own dispatch target: the camera consumer is
// 0x82638CB8 and the UI consumer is 0x823A9398. Blacklisting the UI one (rather
// than whitelisting the camera one) is deliberate: if some level reaches the camera
// through a different function, the mandatory raw look must stay ON and only an
// unlisted menu would misbehave, which is visible and diagnosable from the
// KBMRATE callee= field.
constexpr uint32_t kUiAxisHandler = 0x823A9398;


namespace {
// 0 = yaw, 1 = pitch. Written by GetDeviceState (which the guest calls from more
// than one thread, see KBMRATE) and drained by the guest's look integrator.
std::atomic<float> g_raw_look_units[2]{ {0.0f}, {0.0f} };
// Whether the guest hook is actually live is the one thing this cannot assume:
// an exe built without patch_raw_look.py banks pixels nobody ever drains, and
// the symptom (mouse does nothing) is identical to a zero gain. `calls` counts
// the guest asking, `bank` is what it left behind last time.
std::atomic<uint32_t> g_raw_look_calls[2]{ {0}, {0} };
std::atomic<float> g_raw_look_last[2]{ {0.0f}, {0.0f} };
// When the guest last ASKED, per axis. This is the only honest answer to "is the
// look hook in this build", and GetDeviceState needs it for a second reason: the
// patched site is the gameplay look dispatch, so a consumer that has not called
// in a while means the guest is somewhere the stick axes drive a UI cursor
// instead of a camera. Zeroing the stick there - which raw look must do to avoid
// double-turning - would leave the menus with no mouse at all.
std::atomic<uint64_t> g_raw_look_tick[2]{ {0}, {0} };

// What the guest's own look constants ACTUALLY are. The injection's whole gain
// model is `bank * f29 / f30`, so f29 is the degrees-per-stick-unit scale and f30
// is the per-frame multiplier that has to cancel. Both were inferred from the
// codegen's shape, never read - and "too sensitive" is exactly what an inferred
// f29 deserves. Accumulated per KBMRATE window and printed, so the calibration is
// arithmetic instead of feel.
// g_note_want is the mode-1 addition: the angle actually handed the engine, in
// the engine's own axis-seconds. If mode 1 still feels rate-limited, this is the
// number that says whether the engine ate it or clamped it.
std::atomic<uint32_t> g_note_n[2]{ {0}, {0} };
std::atomic<float> g_note_f29[2]{ {0.0f}, {0.0f} };
std::atomic<float> g_note_dt[2]{ {0.0f}, {0.0f} };
std::atomic<float> g_note_raw[2]{ {0.0f}, {0.0f} };
std::atomic<float> g_note_want[2]{ {0.0f}, {0.0f} };
// Deflection the frame ASKED for (|angle|/dt) before anything clamped it. <1 means
// the move rode the clean analog channel; >1 means it hit the engine's rate
// ceiling and had to go through the time argument. This is the number that says
// whether a "slidey" report is a ceiling problem or a downstream smoothing one.
std::atomic<float> g_note_demand[2]{ {0.0f}, {0.0f} };

uint64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Running mean, recomputed from the previous mean so no window ever has to hold
// a sum large enough to lose precision.
void NoteMean(std::atomic<float>& slot, uint32_t n, float v) {
  const float w = 1.0f / static_cast<float>(n);
  const float prev = slot.load(std::memory_order_relaxed);
  slot.store(prev + (v - prev) * w, std::memory_order_relaxed);
}
}  // namespace

namespace {
// The guest address the axis virtual call actually went to. The look handler is
// reached through a vtable slot, so it is NOT statically resolvable in the
// generated code - and it is the function that decides whether the injected
// value survives. If it normalises and clamps the axis to +/-1 (UE3's axis
// handler contract), then every unit of a fast flick above full deflection is
// thrown away there, and the whole path behaves exactly like the thumbstick it
// was meant to bypass. Naming the callee is the only way to find out instead of
// assuming, so the last target per axis is kept for the KBMRATE line.
// Measured 00:49: 0x82638CB8 - an input-component axis *dispatcher*
// (it walks a bound-handler array and re-issues each handler through a vtable
// slot), which is why the axis domain is a normalized deflection.
std::atomic<uint32_t> g_callee[2]{ {0}, {0} };
}  // namespace

extern "C" void GowjRawLookTarget(int axis, uint32_t guest_addr) {
  if (axis != 0 && axis != 1) return;
  g_callee[axis].store(guest_addr, std::memory_order_relaxed);
}

// Both axes are consumed by the same function on the same record, so requiring
// both to be recent is stricter than necessary and immune to one axis's call
// being skipped by an early branch. 500 ms is far longer than a real frame gap
// and far shorter than a player spending time in a menu.
static bool RawLookConsumerLive() {
  if (!REXCVAR_GET(mnk_raw_look)) return false;
  const uint64_t now = SteadyMs();
  for (int a = 0; a < 2; ++a) {
    // The UI consumer gets the same two axis calls as the camera, so a raw delta
    // here would throw the menu cursor off screen. Where the last call actually
    // went is the only thing that tells the two apart.
    if (g_callee[a].load(std::memory_order_relaxed) == kUiAxisHandler) {
      return false;
    }
    const uint64_t t = g_raw_look_tick[a].load(std::memory_order_relaxed);
    if (t == 0 || now - t > 500) return false;
  }
  return true;
}

extern "C" bool GowjRawLookLive() { return RawLookConsumerLive(); }

// Mirrors GowjKbmInputDriver::overlay_open_ for the final-image pipeline, which
// must not paint over the runtime's ImGui while it is open.
static std::atomic<bool> g_overlay_open{false};
extern "C" bool GowjOverlayOpen() { return g_overlay_open.load(std::memory_order_relaxed); }

REXCVAR_DEFINE_BOOL(mnk_menu_mouse_nav, true, "mnk",
                    "In menus, mouse movement steps the d-pad (wheel scrolls, left click "
                    "= A, right click = B). Menus have no cursor of their own.");
REXCVAR_DEFINE_DOUBLE(mnk_menu_mouse_step, 120.0, "mnk",
                      "Mouse pixels per menu step when navigating menus with the mouse.");

// Not in gameplay: the camera look dispatch has not asked for a delta recently,
// or the last axis call went to the UI handler (pause menu over gameplay).
// Last time the keyboard-driven virtual pad held a face/shoulder/stick-click button, or any button.
static std::atomic<int64_t> g_virt_face_ms{0};
static std::atomic<int64_t> g_virt_any_ms{0};
extern "C" int64_t GowjVirtualFaceMs() { return g_virt_face_ms.load(std::memory_order_relaxed); }
extern "C" int64_t GowjVirtualAnyMs() { return g_virt_any_ms.load(std::memory_order_relaxed); }

static bool InMenus() { return REXCVAR_GET(mnk_raw_look) && !RawLookConsumerLive(); }

// Signed look angle (engine units: 1.0 = one second at full deflection) the
// mouse asked for since the last take, on this axis.
static std::atomic<double> g_frame_want[2]{ {0.0}, {0.0} };
extern "C" double GowjTakeFrameWant(int axis) {
  if (axis != 0 && axis != 1) return 0.0;
  return g_frame_want[axis].exchange(0.0, std::memory_order_relaxed);
}

// Called from the recompiled guest once per input record. Consume-and-clear is
// what makes the per-record call rate irrelevant: whatever the hand moved between
// two calls is paid out in full exactly once, so the total turn equals the total
// mouse distance at any poll rate and any flick speed.
extern "C" float GowjRawLookUnits(int axis) {
  if (axis != 0 && axis != 1) return 0.0f;
  // Installed from here because the function dispatcher does not exist yet when
  // the app's early hooks run; by the first look poll it always does.
  rex::glue::InstallViewAccelHook();
  g_raw_look_calls[axis].fetch_add(1, std::memory_order_relaxed);
  g_raw_look_tick[axis].store(SteadyMs(), std::memory_order_relaxed);
  if (!REXCVAR_GET(mnk_raw_look)) return 0.0f;
  const float v = g_raw_look_units[axis].exchange(0.0f, std::memory_order_relaxed);
  g_raw_look_last[axis].store(v, std::memory_order_relaxed);
  return v;
}

// The look injection itself, called at the guest's two right-stick axis sites
// (tools/patch_raw_look.py). It rewrites the two float arguments of the
// axis call, so the engine's own handler stays untouched and only the number it
// is handed changes.
//
// WHAT THE TWO ARGUMENTS ARE. Measured from the guest's own constants, not
// inferred: the handler gets arg1 = float(stick) * f29 with
// f29 = 3.0517578e-05 = 1/32768 - a NORMALIZED deflection whose meaningful domain
// is [-1, 1] - and arg2 = the frame's dt, and the frame's turn is arg1 * arg2. The
// dispatcher at 0x82638CB8 stores them as two separate floats in a parameter
// struct (stfs f31,92(r1) / stfs f30,96(r1)) and re-issues that struct to every
// bound handler, so both are live numbers with their own meaning downstream: arg1
// saturates at +/-1 and arg2 is the integration step anything time-based in the
// handler will use.
//
// WHY THE HYBRID. Mode 0 (adding the angle to arg1) is exact for small moves and
// silently throws away everything above full deflection, so a fast flick turns the
// same angle as a slow one - the player's "it acts exactly like emulating a
// joystick should, especially with fast flicks that it can't handle". Mode 1 is
// exact always, but it does that by lying about dt on every single call: a 1-pixel
// nudge asked the engine for a 0.0003 s frame and a hard flick for a 1.8 s one, so
// every time-based term downstream of this call ran at the wrong step - which is
// "too sensitive and slidey". The hybrid takes the loss only where a loss is
// unavoidable: while the demanded turn fits inside one real frame it goes through
// as an honest analog deflection with the real dt untouched, and only a flick that
// outruns the engine's own maximum turn rate spills into the time argument, because
// "faster than max rate" cannot be expressed any other way in a rate channel.
//
// `bank` is MOUSE PIXELS moved since the last call on this axis (sign already
// applied), and mnk_raw_look_scale converts pixels to the engine's angle unit, so
// the raw gain is one number and is not coupled to the gamepad's stick knobs.
// `axis_sign` is +1 for yaw and -1 for pitch: the guest computes its pitch
// argument as float(-RY) ("neg r11,r4") while the bank holds a stick-convention
// RY-equivalent, so the injection has to reapply that negation itself.
extern "C" int GowjRawLookAxis(int axis, float axis_sign, float stick_angle,
                               float f29, float dt, float* out_axis,
                               float* out_dt) {
  if (axis != 0 && axis != 1 || !out_axis || !out_dt) return 0;
  const float bank = GowjRawLookUnits(axis);
  if (bank == 0.0f) return 0;  // a stick-only frame: leave the guest's own value
  const double scale = REXCVAR_GET(mnk_raw_look_scale);
  const double axis_scale =
      axis == 0 ? REXCVAR_GET(mnk_yaw_scale) : REXCVAR_GET(mnk_pitch_scale);
  // The turn this frame must deliver, in the engine's angle units, added to what
  // the guest already asked for so a gamepad in use at the same time is not
  // clobbered.
  // The turn the mouse asks for, in the engine's angle units, and the total the
  // frame must deliver once the guest's own stick value is added - so a gamepad in
  // use at the same time is not clobbered.
  const double delta = static_cast<double>(bank) * axis_sign * axis_scale * scale;
  const double want = static_cast<double>(stick_angle) * dt + delta;
  const int mode = REXCVAR_GET(mnk_raw_look_mode);
  const bool bad_dt = !(dt > 0.000001f);
  // |want| > dt means the frame asks for more turn than full deflection held for
  // the whole frame - above the engine's own maximum turn rate.
  const bool over_ceiling = std::fabs(want) > static_cast<double>(dt);
  const bool time_domain =
      mode == 1 || (mode == 2 && (bad_dt || over_ceiling));
  if (time_domain) {
    // Past the rate ceiling arg1 cannot express: pin it at exactly full
    // deflection, where there is nothing left to clamp, and carry the whole angle
    // in arg2.
    *out_axis = want < 0.0 ? -1.0f : 1.0f;
    *out_dt = static_cast<float>(std::fabs(want));
  } else if (bad_dt) {
    return 0;  // mode 0 with no usable frame step: leave the guest's own value
  } else {
    *out_axis = static_cast<float>(mode == 0
                                       ? static_cast<double>(stick_angle) +
                                             delta / dt
                                       : want / dt);
    *out_dt = dt;
  }
  // The axis handler behind 0x82638CB8 clamps arg1 to +/-1 and ignores arg2
  // (measured 06:17: aTurn peaked at exactly 1.0 for a 1.08x demand and for a
  // 16x one). So the full angle is also banked here and written straight into
  // aTurn/aLookUp after the clamp, by the ViewAcceleration hook.
  {
    double prev = g_frame_want[axis].load(std::memory_order_relaxed);
    while (!g_frame_want[axis].compare_exchange_weak(prev, prev + want,
                                                     std::memory_order_relaxed)) {
    }
  }
  const uint32_t n =
      g_note_n[axis].fetch_add(1, std::memory_order_relaxed) + 1;
  NoteMean(g_note_f29[axis], n, f29);
  NoteMean(g_note_dt[axis], n, dt);
  NoteMean(g_note_raw[axis], n, std::fabs(bank));
  NoteMean(g_note_want[axis], n, std::fabs(*out_axis) * std::fabs(*out_dt));
  NoteMean(g_note_demand[axis], n,
           std::fabs(want) / std::max(static_cast<double>(dt), 0.000001));
  return 1;
}

namespace rex::input {

namespace {

// Full-scale deflection of an XInput thumbstick axis. Mouse motion maps linearly
// onto this range (see GetDeviceState).
constexpr int32_t kStickMax = 32767;

// PC-game control scheme, replacing the runtime's pad-emulation defaults
// (measured: A="Semicolon,Space", B="Quote,Backspace", X="L", Y="P",
// Start="X,Return", Back="Z,Tab", dpad="Shift+Arrow"). Those are unusable for
// a player who expects WASD + mouse, and every one of them is a string cvar in
// the "Keybinds" category, so the scheme below is only a *default*: anything
// already set by config/gowj.toml, REX_* or --keybind_x= is left alone.
//
// Matches the Gears PC layout the player supplied, translated onto the 360 pad
// the guest actually reads (the guest has no keyboard, so each PC action maps to
// the pad button that performs it):
//   move W/A/S/D            -> left stick
//   cover / interact      -> Space      (pad A, contextual)
//   use / pick up / revive -> E          (pad A too - the one interact button)
//   crouch (hold) / execute -> Ctrl / V   (pad B, hold crouch / tap execute)
//   reload / rev chainsaw   -> R          (pad X)
//   weapon swap             -> (no key)   (pad Y = mouse wheel, see below)
//   melee / vault           -> F          (pad RB)
//   spot / mark             -> Q          (pad LB)
//   walk                    -> Z          (pad L3, move modifier)
//   scoreboard / objectives -> Tab        (pad Back)
//   pause                   -> Esc        (pad Start)
//   fire / aim              -> LMB / RMB  (guest RT / LT, hardcoded)
//   cycle weapon            -> mouse wheel (taps pad Y = the confirmed swap button)
//   menu navigation         -> arrows     (d-pad)
// Not mappable: weapon slots 1-4, scope-toggle MMB, push-to-talk C, chat T and
// the MP ability key - the guest is a console title with no pad button for them
// (wheel-taps-Y is the only weapon-select input). Tac-Com (Shift) is MP-only.
struct DefaultBind {
  const char* name;
  const char* keys;
};
constexpr DefaultBind kPreferredBinds[] = {
    // Pad A is the contextual "interact" button: it enters cover, and (tutorial-
    // measured) picks items up, revives and accepts prompts. The player's PC list
    // splits these across Space (cover) and E (use/pick up/revive), but the
    // console guest has only the one interact button, so BOTH keys drive A.
    {"keybind_a", "Return,Space"},      // cover / interact / use / revive / accept
    {"keybind_b", "Backspace"},          // menu back (gameplay uses the engine's own bindings)
    {"keybind_x", "R"},                  // reload / rev chainsaw
    // Y is weapon-swap (confirmed by the in-game tutorial prompt). There is no
    // keyboard key for it in the player's PC list - the wheel pulses Y directly
    // (OnMouseWheel), so leave the binding empty rather than steal a letter.
    {"keybind_y", "Y"},                  // menu Y action
    {"keybind_start", "Escape"},         // pause menu
    {"keybind_back", "Tab"},             // objectives / scoreboard
    {"keybind_guide", ""},               // no Xbox-guide equivalent
    {"keybind_left_shoulder", "Q"},      // spot / mark
    {"keybind_right_shoulder", "F"},     // melee / vault
    {"keybind_left_trigger", ""},        // RMB aims (guest LT)
    {"keybind_right_trigger", ""},       // LMB fires (guest RT)
    {"keybind_lstick_press", "Z"},       // walk
    {"keybind_rstick_press", ""},
    {"keybind_lstick_up", "W"},
    {"keybind_lstick_down", "S"},
    {"keybind_lstick_left", "A"},
    {"keybind_lstick_right", "D"},
    // The right stick is mouse-look; keyboard axes would only fight it.
    {"keybind_rstick_up", ""},
    {"keybind_rstick_down", ""},
    {"keybind_rstick_left", ""},
    {"keybind_rstick_right", ""},
    {"keybind_dpad_up", "Up"},
    {"keybind_dpad_down", "Down"},
    {"keybind_dpad_left", "Left"},
    {"keybind_dpad_right", "Right"},
};

uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

void ApplyPreferredKeybindings() {
  int applied = 0;
  for (const auto& bind : kPreferredBinds) {
    auto& registry = rex::cvar::GetRegistry();
    bool still_default = false;
    for (auto& entry : registry) {
      if (entry.name == bind.name) {
        still_default = entry.source == rex::cvar::Source::kDefault;
        break;
      }
    }
    // Not registered (wrong SDK) or already set by config/env/CLI: leave be.
    if (!still_default || !rex::cvar::SetFlagByName(bind.name, bind.keys)) {
      continue;
    }
    ++applied;
  }
  REXLOG_INFO("gowj keybinds: {} of {} defaults applied", applied,
              static_cast<int>(std::size(kPreferredBinds)));
  for (const auto& bind : kPreferredBinds) {
    REXLOG_INFO("gowj keybind {} = '{}'", bind.name,
                rex::cvar::GetFlagByName(bind.name));
  }
}

using rex::ui::ParseVirtualKey;
using rex::ui::VirtualKey;

GowjKbmInputDriver::~GowjKbmInputDriver() {
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
}

X_STATUS GowjKbmInputDriver::Setup() {
  RebuildTables();
  REXLOG_INFO("gowj KBM driver Setup complete");
  return X_STATUS_SUCCESS;
}

GowjKbmInputDriver::Binding GowjKbmInputDriver::ParseBinding(
    const std::string& spec, const char* what) {
  Binding b;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    std::string item = spec.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = comma == std::string::npos ? spec.size() : comma + 1;
    item.erase(std::remove_if(item.begin(), item.end(),
                              [](char c) { return c == ' '; }),
               item.end());
    if (item.empty()) {
      continue;
    }
    if (item.size() > 6 && item.compare(0, 6, "Shift+") == 0) {
      b.shift = true;
      item.erase(0, 6);
    }
    auto vk = ParseVirtualKey(item);
    if (vk == VirtualKey::kNone) {
      // Silently dropping a typo is how "my control scheme does nothing" gets
      // shipped: name it in the log instead.
      REXLOG_WARN("gowj keybind {}: '{}' is not a key name, ignored",
                  what ? what : "?", item);
      continue;
    }
    b.keys.push_back(vk);
  }
  return b;
}

void GowjKbmInputDriver::RebuildTables() {
  std::lock_guard lock(bind_mutex_);
  RebuildTablesLocked();
}

void GowjKbmInputDriver::RebuildTablesLocked() {
  std::map<VirtualKey, uint16_t> buttons;
  std::map<VirtualKey, uint16_t> shift_dpads;
  std::map<VirtualKey, uint8_t> axes;
  std::map<VirtualKey, bool> triggers;

  auto add_button = [&](const std::string& spec, const char* name, uint16_t mask) {
    for (auto k : ParseBinding(spec, name).keys) buttons[k] |= mask;
  };
  auto add_dpad = [&](const std::string& spec, const char* name, uint16_t mask) {
    auto b = ParseBinding(spec, name);
    for (auto k : b.keys) {
      if (b.shift) {
        shift_dpads[k] |= mask;
      } else {
        buttons[k] |= mask;
      }
    }
  };
  auto add_axis = [&](const std::string& spec, const char* name, uint8_t code) {
    for (auto k : ParseBinding(spec, name).keys) axes[k] = code;
  };
  auto add_trigger = [&](const std::string& spec, const char* name, bool right) {
    for (auto k : ParseBinding(spec, name).keys) triggers[k] = right;
  };

  add_button(REXCVAR_GET(keybind_a), "keybind_a", kBtnA);
  add_button(REXCVAR_GET(keybind_b), "keybind_b", kBtnB);
  add_button(REXCVAR_GET(keybind_x), "keybind_x", kBtnX);
  add_button(REXCVAR_GET(keybind_y), "keybind_y", kBtnY);
  add_button(REXCVAR_GET(keybind_start), "keybind_start", kBtnStart);
  add_button(REXCVAR_GET(keybind_back), "keybind_back", kBtnBack);
  add_button(REXCVAR_GET(keybind_left_shoulder), "keybind_left_shoulder", kBtnLShoulder);
  add_button(REXCVAR_GET(keybind_right_shoulder), "keybind_right_shoulder", kBtnRShoulder);
  add_button(REXCVAR_GET(keybind_lstick_press), "keybind_lstick_press", kBtnLThumb);
  add_button(REXCVAR_GET(keybind_rstick_press), "keybind_rstick_press", kBtnRThumb);
  add_dpad(REXCVAR_GET(keybind_dpad_up), "keybind_dpad_up", kBtnUp);
  add_dpad(REXCVAR_GET(keybind_dpad_down), "keybind_dpad_down", kBtnDown);
  add_dpad(REXCVAR_GET(keybind_dpad_left), "keybind_dpad_left", kBtnLeft);
  add_dpad(REXCVAR_GET(keybind_dpad_right), "keybind_dpad_right", kBtnRight);

  add_axis(REXCVAR_GET(keybind_lstick_up), "keybind_lstick_up", kLYPos);
  add_axis(REXCVAR_GET(keybind_lstick_down), "keybind_lstick_down", kLYNeg);
  add_axis(REXCVAR_GET(keybind_lstick_left), "keybind_lstick_left", kLXNeg);
  add_axis(REXCVAR_GET(keybind_lstick_right), "keybind_lstick_right", kLXPos);
  add_axis(REXCVAR_GET(keybind_rstick_up), "keybind_rstick_up", kRYPos);
  add_axis(REXCVAR_GET(keybind_rstick_down), "keybind_rstick_down", kRYNeg);
  add_axis(REXCVAR_GET(keybind_rstick_left), "keybind_rstick_left", kRXNeg);
  add_axis(REXCVAR_GET(keybind_rstick_right), "keybind_rstick_right", kRXPos);

  add_trigger(REXCVAR_GET(keybind_left_trigger), "keybind_left_trigger", false);
  add_trigger(REXCVAR_GET(keybind_right_trigger), "keybind_right_trigger", true);

  button_bindings_.swap(buttons);
  shift_dpads_.swap(shift_dpads);
  axis_bindings_.swap(axes);
  trigger_bindings_.swap(triggers);
}

void GowjKbmInputDriver::RecomputeLocked() {
  uint16_t buttons = 0;
  int32_t lx = 0, ly = 0, rx = 0, ry = 0;
  int32_t lt = 0, rt = 0;

  for (auto vk : held_keys_) {
    auto it = button_bindings_.find(vk);
    if (it != button_bindings_.end()) {
      buttons |= it->second;
    }
    auto ax = axis_bindings_.find(vk);
    if (ax != axis_bindings_.end()) {
      switch (ax->second) {
        case kLXPos: lx = kStickMax; break;
        case kLXNeg: lx = -kStickMax; break;
        case kLYPos: ly = kStickMax; break;
        case kLYNeg: ly = -kStickMax; break;
        case kRXPos: rx = kStickMax; break;
        case kRXNeg: rx = -kStickMax; break;
        case kRYPos: ry = kStickMax; break;
        case kRYNeg: ry = -kStickMax; break;
        default: break;
      }
    }
    auto tr = trigger_bindings_.find(vk);
    if (tr != trigger_bindings_.end()) {
      if (tr->second) {
        rt = 255;
      } else {
        lt = 255;
      }
    }
  }

  // Mouse buttons fire the triggers, Gears-on-PC style: LMB=fire=RT,
  // RMB=aim=LT.
  if (lmb_down_.load(std::memory_order_relaxed)) rt = 255;
  if (rmb_down_.load(std::memory_order_relaxed)) lt = 255;

  // Shift+arrow dpad: dpad keys are only live while Shift is actually held.
  // StoreKey records both the delivered side-specific code and the generic one
  // a "Shift" binding parses to, so all three count.
  bool shift = held_keys_.count(VirtualKey::kLShift) ||
               held_keys_.count(VirtualKey::kRShift) ||
               held_keys_.count(VirtualKey::kShift);
  if (shift) {
    for (auto vk : held_keys_) {
      auto dp = shift_dpads_.find(vk);
      if (dp != shift_dpads_.end()) buttons |= dp->second;
    }
  }

  buttons_.store(buttons, std::memory_order_relaxed);
  lt_.store(static_cast<uint8_t>(std::min(lt + 0, 255)), std::memory_order_relaxed);
  rt_.store(static_cast<uint8_t>(rt), std::memory_order_relaxed);
  lx_.store(static_cast<int16_t>(lx), std::memory_order_relaxed);
  ly_.store(static_cast<int16_t>(ly), std::memory_order_relaxed);
  kb_rx_.store(static_cast<int16_t>(rx), std::memory_order_relaxed);
  kb_ry_.store(static_cast<int16_t>(ry), std::memory_order_relaxed);
  packet_.fetch_add(1, std::memory_order_relaxed);
}

VirtualKey GowjKbmInputDriver::PadCodeForButton(uint16_t mask) {
  switch (mask) {
    case kBtnA: return VirtualKey::kXInputPadA;
    case kBtnB: return VirtualKey::kXInputPadB;
    case kBtnX: return VirtualKey::kXInputPadX;
    case kBtnY: return VirtualKey::kXInputPadY;
    case kBtnStart: return VirtualKey::kXInputPadStart;
    case kBtnBack: return VirtualKey::kXInputPadBack;
    case kBtnLShoulder: return VirtualKey::kXInputPadLShoulder;
    case kBtnRShoulder: return VirtualKey::kXInputPadRShoulder;
    case kBtnLThumb: return VirtualKey::kXInputPadLThumbPress;
    case kBtnRThumb: return VirtualKey::kXInputPadRThumbPress;
    case kBtnUp: return VirtualKey::kXInputPadDpadUp;
    case kBtnDown: return VirtualKey::kXInputPadDpadDown;
    case kBtnLeft: return VirtualKey::kXInputPadDpadLeft;
    case kBtnRight: return VirtualKey::kXInputPadDpadRight;
    default: return VirtualKey::kNone;
  }
}

void GowjKbmInputDriver::PushKeystroke(VirtualKey code, bool down) {
  if (code == VirtualKey::kNone) return;
  X_INPUT_KEYSTROKE ks{};
  ks.virtual_key = static_cast<uint16_t>(code);
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = 0;
  std::lock_guard lock(keystroke_mutex_);
  if (keystrokes_.size() < 64) keystrokes_.push_back(ks);
}

void GowjKbmInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  DeviceInfo info;
  info.id = device_id_;
  info.ordinal = 0;
  // One stable device, never added to or removed, so RefreshDevices has no
  // churn to rebuild.
  info.name = "KBM gowj";
  info.guid = "gowj-kbm";
  info.synthetic = true;
  out.push_back(std::move(info));
}

#ifdef GOWJ_DEV_TOOLS
namespace {
// Remote pad for unattended test runs (GOWJ_REMOTE_PAD=1): gowj_pad.txt holds
// "<hex buttons> [lx ly rx ry]" (sticks as decimal int16). Read at most every 15 ms.
std::atomic<int16_t> g_remote_ry{0};
struct RemotePad {
  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
RemotePad ReadRemotePad() {
  static const bool enabled = [] {
    const char* e = std::getenv("GOWJ_REMOTE_PAD");
    return e && *e == '1';
  }();
  RemotePad out;
  if (!enabled) return out;
  static std::atomic<int64_t> next_ms{0};
  static std::atomic<uint64_t> packed{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now >= next_ms.load(std::memory_order_relaxed)) {
    next_ms.store(now + 15, std::memory_order_relaxed);
    RemotePad v;
    if (FILE* f = std::fopen("gowj_pad.txt", "r")) {
      unsigned x = 0;
      int a = 0, b = 0, c = 0, d = 0;
      int n = std::fscanf(f, "%x %d %d %d %d", &x, &a, &b, &c, &d);
      if (n >= 1) v.buttons = uint16_t(x);
      if (n >= 5) {
        v.lx = int16_t(a);
        v.ly = int16_t(b);
        v.rx = int16_t(c);
        v.ry = int16_t(d);
      }
      std::fclose(f);
    }
    packed.store(uint64_t(v.buttons) | (uint64_t(uint16_t(v.lx)) << 16) |
                     (uint64_t(uint16_t(v.ly)) << 32) | (uint64_t(uint16_t(v.rx)) << 48),
                 std::memory_order_relaxed);
    g_remote_ry.store(v.ry, std::memory_order_relaxed);
  }
  const uint64_t p = packed.load(std::memory_order_relaxed);
  out.buttons = uint16_t(p);
  out.lx = int16_t(p >> 16);
  out.ly = int16_t(p >> 32);
  out.rx = int16_t(p >> 48);
  out.ry = g_remote_ry.load(std::memory_order_relaxed);
  return out;
}
uint16_t RemotePadButtons() { return ReadRemotePad().buttons; }
}  // namespace
#endif

X_RESULT GowjKbmInputDriver::GetDeviceState(DeviceId id,
                                            X_INPUT_STATE* out_state) {
  if (id != device_id_ || !out_state) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint64_t now_us = NowUs();
  // Clamped: a breakpoint, an alt-tab or a level-load stall must not turn one
  // poll into a multi-second full-deflection frame.
  double dt = last_state_us_
                  ? std::min<double>((now_us - last_state_us_) / 1e6, 0.1)
                  : 0.0;
  last_state_us_ = now_us;

  if (REXCVAR_GET(mnk_fps_meter)) {
    ++meter_polls_;
    if (now_us - meter_window_start_us_ >= 1000000) {
      // cap/moves/dx/dy are the mouse-look proof: cap=0 means the pointer was
      // never captured, moves=0 with cap=1 means no motion events arrived. bank is
      // the pixel debt right now, inj the pixels the last injection actually spent.
      // dem is the whole diagnostic in one number: the deflection the frame ASKED
      // for, so dem<1 means the turn rode the clean analog channel and dem>1 means
      // it hit the engine's rate ceiling and had to go through the time argument.
      // ks counts XGetKeystroke reads by the guest: ks=0 forever means Judgment
      // never polls the keyboard side of XInput, so its native KBM action scheme is
      // unreachable and pad emulation is all we have to map the player's control
      // list onto.
      REXLOG_INFO(
          "KBMRATE guest polls/s={} cap={} moves={} dx={} dy={} clamp={} ks={} "
          "bank={}x{} last={}x{} cl={}/{} mode={} gain={} f29={}x{} dt={}x{} "
          "inj={}x{} want={:.6f}x{:.6f} dem={:.2f}x{:.2f} callee={:08X}x{:08X}",
          meter_polls_, mouse_captured_.load(std::memory_order_relaxed) ? 1 : 0,
          meter_moves_.exchange(0, std::memory_order_relaxed),
          meter_dx_.exchange(0, std::memory_order_relaxed),
          meter_dy_.exchange(0, std::memory_order_relaxed),
          meter_clamp_.exchange(0, std::memory_order_relaxed),
          meter_keystrokes_.exchange(0, std::memory_order_relaxed),
          g_raw_look_last[0].load(std::memory_order_relaxed),
          g_raw_look_last[1].load(std::memory_order_relaxed),
          g_raw_look_units[0].load(std::memory_order_relaxed),
          g_raw_look_units[1].load(std::memory_order_relaxed),
          g_raw_look_calls[0].exchange(0, std::memory_order_relaxed),
          g_raw_look_calls[1].exchange(0, std::memory_order_relaxed),
          REXCVAR_GET(mnk_raw_look_mode),
          REXCVAR_GET(mnk_raw_look_scale),
          g_note_f29[0].load(std::memory_order_relaxed),
          g_note_f29[1].load(std::memory_order_relaxed),
          g_note_dt[0].load(std::memory_order_relaxed),
          g_note_dt[1].load(std::memory_order_relaxed),
          g_note_raw[0].load(std::memory_order_relaxed),
          g_note_raw[1].load(std::memory_order_relaxed),
          g_note_want[0].load(std::memory_order_relaxed),
          g_note_want[1].load(std::memory_order_relaxed),
          g_note_demand[0].load(std::memory_order_relaxed),
          g_note_demand[1].load(std::memory_order_relaxed),
          g_callee[0].load(std::memory_order_relaxed),
          g_callee[1].load(std::memory_order_relaxed));
      // Window the note accumulators with the call counters, so a stale mean from
      // before a settings change cannot be read as the current one.
      for (int a = 0; a < 2; ++a) {
        g_note_n[a].store(0, std::memory_order_relaxed);
        g_note_f29[a].store(0.0f, std::memory_order_relaxed);
        g_note_dt[a].store(0.0f, std::memory_order_relaxed);
        g_note_raw[a].store(0.0f, std::memory_order_relaxed);
        g_note_want[a].store(0.0f, std::memory_order_relaxed);
        g_note_demand[a].store(0.0f, std::memory_order_relaxed);
      }
      meter_polls_ = 0;
      meter_window_start_us_ = now_us;
    }
  }

  // LIVE GAIN TUNING. This runtime has no settings overlay and no console key
  // (measured: bind_settings/bind_debug are absent from the 95-cvar dump), so
  // without a hot key every sensitivity change costs a full restart - and the
  // right number here is a feel judgement the player makes in seconds, not
  // something derivable from the guest's constants. F11 = 20% less, F12 = 20%
  // more, only while the pointer is captured, and the value is logged so the one
  // the player settles on can be read out of the game log and made the default.
  static std::atomic<bool> f11_held{false};
  static std::atomic<bool> f12_held{false};
  if (mouse_captured_.load(std::memory_order_relaxed)) {
    const bool k11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool k12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if ((k11 && !f11_held.exchange(true)) || (k12 && !f12_held.exchange(true))) {
      const double cur = REXCVAR_GET(mnk_raw_look_scale);
      const double next = std::clamp(k11 ? cur / 1.2 : cur * 1.2, 0.00002, 0.5);
      REXCVAR_SET(mnk_raw_look_scale, next);
      REXLOG_INFO("RAWLOOK mnk_raw_look_scale {} -> {} ({}) - not written to "
                  "gowj.toml, so it reverts on restart; copy it there to keep it",
                  cur, next, k11 ? "F11 less sensitive" : "F12 more sensitive");
    }
    if (!k11) f11_held.store(false);
    if (!k12) f12_held.store(false);
  } else {
    f11_held.store(false);
    f12_held.store(false);
  }

  const float sens = static_cast<float>(REXCVAR_GET(mnk_sensitivity));
  const float yaw_scale = static_cast<float>(REXCVAR_GET(mnk_yaw_scale));
  const float pitch_scale = static_cast<float>(REXCVAR_GET(mnk_pitch_scale));
  const bool invert_y = REXCVAR_GET(mnk_invert_y);
  const int32_t look_model = REXCVAR_GET(mnk_look_model);
  float raw_dx = 0.0f, raw_dy = 0.0f;
  if (mouse_captured_.load(std::memory_order_relaxed)) {
    raw_dx = mouse_dx_.exchange(0.0f, std::memory_order_relaxed);
    raw_dy = mouse_dy_.exchange(0.0f, std::memory_order_relaxed);
  } else {
    // Uncaptured mouse motion is dead for aiming.
    mouse_dx_.store(0.0f, std::memory_order_relaxed);
    mouse_dy_.store(0.0f, std::memory_order_relaxed);
  }

  // Raw mouse-look. The guest rotates its camera once per input poll by the
  // deflection it reads, so the mapping has to guarantee
  //     total turn == total mouse pixels
  // for ANY hand speed. A per-poll proportional map (deflection = this poll's
  // pixels * gain) breaks that: once a flick outruns the linear range the
  // clamp silently deletes the rest of the motion, so fast turns under-rotate
  // and slow turns don't - the "aim feels dead on a flick" report, and the
  // reason the clamp counter below existed.
  //
  // So the debt is carried in STICK UNITS, not pixels, and each poll delivers
  // the rounded amount it can actually represent, subtracting exactly what it
  // handed over:
  //   * nothing is ever dropped - the sum of delivered deflections equals the
  //     sum of requested ones to within a half unit, at any speed;
  //   * sub-unit motion is not lost either (a 1-pixel nudge that rounds to 0
  //     stays in the debt and is paid out with the next pixel);
  //   |debt| < 0.5 whenever the hand stops, so the stick returns to exactly 0
  //     on the next poll - no decay term, no tail, no "sliding on ice".
  // The only remaining ceiling is the hardware one: full deflection at the
  // guest's poll rate is the maximum turn rate a thumbstick can express, so an
  // extreme flick is rate-limited rather than lost. `clamp` counts those polls.
  float defl_x, defl_y;
  if (look_model == 1) {
    // Legacy proportional path, for A/B feel measurement only: saturates.
    const float full_px =
        static_cast<float>(std::max(1.0, REXCVAR_GET(mnk_full_deflect_pixels)));
    const float k = static_cast<float>(kStickMax) / full_px * sens;
    defl_x = raw_dx * k * yaw_scale;
    defl_y = -raw_dy * k * pitch_scale;
  } else {
    const float full_px =
        std::max(1.0f, static_cast<float>(REXCVAR_GET(mnk_full_deflect_pixels))) /
        std::max(0.01f, sens);
    const float gain_x = static_cast<float>(kStickMax) / full_px * yaw_scale;
    const float gain_y = static_cast<float>(kStickMax) / full_px * pitch_scale;
    float add_x = raw_dx * gain_x;
    float add_y = -raw_dy * gain_y;
    if (invert_y) {
      add_y = -add_y;
    }
    if (RawLookConsumerLive()) {
      // Bank the movement as MOUSE PIXELS and hand the guest's stick nothing: the
      // injection then converts exactly those pixels into the frame's turn, once,
      // with no deflection ceiling to saturate against and no remainder carried
      // into later frames. Sub-pixel precision is kept because the bank is a
      // float. Deliberately NOT stick units: the stick gain (mnk_sensitivity,
      // mnk_full_deflect_pixels) is a rate-limited-channel calibration and has no
      // business setting the gain of a raw delta, which is why raw look has its
      // own single gain cvar, mnk_raw_look_scale.
      const float bank_y = invert_y ? raw_dy : -raw_dy;
      g_raw_look_units[0].fetch_add(raw_dx, std::memory_order_relaxed);
      g_raw_look_units[1].fetch_add(bank_y, std::memory_order_relaxed);
      defl_x = 0.0f;
      defl_y = 0.0f;
      std::lock_guard<std::mutex> look_guard(look_debt_mutex_);
      look_debt_x_ = 0.0f;
      look_debt_y_ = 0.0f;
    } else {
      // Either mnk_raw_look is off, or the guest's look dispatch has not asked for
      // a delta recently and is therefore somewhere a stick deflection still means
      // something (menus, or an exe built without patch_raw_look.py). Fall back to
      // the rate-limited stick path so the mouse is never dead.
      // The guest polls this from more than one thread (measured), so the debt is
      // a shared resource: read-modify-write it under a lock, which at ~125 polls/s
      // costs nothing and can never double-pay or drop a flick.
      std::lock_guard<std::mutex> legacy_guard(look_debt_mutex_);
      if (dt >= 0.099f) {
        // dt is clamped at 0.1 s, so this is a stall (level load, breakpoint,
        // alt-tab): whatever banked during it is stale, not player intent.
        look_debt_x_ = 0.0f;
        look_debt_y_ = 0.0f;
      }
      const float ceiling = 32.0f * static_cast<float>(kStickMax);
      look_debt_x_ = std::clamp(look_debt_x_ + add_x, -ceiling, ceiling);
      look_debt_y_ = std::clamp(look_debt_y_ + add_y, -ceiling, ceiling);
      defl_x = std::lrintf(look_debt_x_);
      defl_y = std::lrintf(look_debt_y_);
      look_debt_x_ -= defl_x;
      look_debt_y_ -= defl_y;
    }
  }
  const float bound = static_cast<float>(kStickMax);
  float look_x = std::clamp(defl_x, -bound, bound);
  float look_y = std::clamp(defl_y, -bound, bound);
  if ((std::fabs(defl_x) >= bound || std::fabs(defl_y) >= bound) &&
      (raw_dx != 0.0f || raw_dy != 0.0f)) {
    meter_clamp_.fetch_add(1, std::memory_order_relaxed);
  }

  int32_t rx = std::clamp<int32_t>(
      kb_rx_.load(std::memory_order_relaxed) + static_cast<int32_t>(look_x),
      -kStickMax, kStickMax);
  int32_t ry = std::clamp<int32_t>(
      kb_ry_.load(std::memory_order_relaxed) + static_cast<int32_t>(look_y),
      -kStickMax, kStickMax);

  uint16_t buttons = buttons_.load(std::memory_order_relaxed);
#ifdef GOWJ_DEV_TOOLS
  buttons |= RemotePadButtons();  // GOWJ_REMOTE_PAD=1: test rigs drive the pad through gowj_pad.txt
#endif
  if (NowMs() < sticky_until_ms_.load(std::memory_order_relaxed)) {
    buttons |= sticky_buttons_.load(std::memory_order_relaxed);
  } else {
    sticky_buttons_.store(0, std::memory_order_relaxed);
  }

  // While the engine's own keyboard/mouse bindings drive gameplay (see gowj_native_input.cpp),
  // the virtual pad must not ALSO press the same buttons: drop everything the keyboard and
  // mouse used to fake. Start, Back and the d-pad stay (pause, scoreboard and menu keys).
  const bool native_keys = GowjNativeKeysDriving();
  // The face and shoulder buttons stay on the virtual pad: menus read them there and the engine's UI
  // consumes them before they reach the player input. In gameplay the same press also arrives at the
  // player input, where gowj_native_input.cpp drops it because the native key already did the job.
  {
    constexpr uint16_t kFace = kBtnA | kBtnB | kBtnX | kBtnY | kBtnLShoulder | kBtnRShoulder | kBtnLThumb | kBtnRThumb;
    const int64_t now = int64_t(NowMs());
    if (buttons & kFace) g_virt_face_ms.store(now, std::memory_order_relaxed);
    if (buttons) g_virt_any_ms.store(now, std::memory_order_relaxed);
  }
  out_state->packet_number = packet_.load(std::memory_order_relaxed);
  out_state->gamepad.buttons = buttons;
  out_state->gamepad.left_trigger = native_keys ? 0 : lt_.load(std::memory_order_relaxed);
  out_state->gamepad.right_trigger = native_keys ? 0 : rt_.load(std::memory_order_relaxed);
  out_state->gamepad.thumb_lx = native_keys ? 0 : lx_.load(std::memory_order_relaxed);
  out_state->gamepad.thumb_ly = native_keys ? 0 : ly_.load(std::memory_order_relaxed);
  out_state->gamepad.thumb_rx = static_cast<int16_t>(rx);
#ifdef GOWJ_DEV_TOOLS
  {
    const RemotePad rp = ReadRemotePad();
    if (rp.lx || rp.ly || rp.rx || rp.ry) {
      out_state->gamepad.thumb_lx = rp.lx;
      out_state->gamepad.thumb_ly = rp.ly;
      out_state->gamepad.thumb_rx = rp.rx;
      rx = rp.rx;
      ry = rp.ry;
    }
  }
#endif
  out_state->gamepad.thumb_ry = static_cast<int16_t>(ry);
  return X_ERROR_SUCCESS;
}

X_RESULT GowjKbmInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                   X_INPUT_CAPABILITIES* out_caps) {
  if (id != device_id_ || !out_caps) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = XINPUT_DEVTYPE_GAMEPAD;  // XINPUT_DEVSUBTYPE_GAMEPAD
  out_caps->flags = X_INPUT_FLAG_GAMEPAD;
  out_caps->gamepad.buttons = 0xffff;
  out_caps->gamepad.left_trigger = 0xff;
  out_caps->gamepad.right_trigger = 0xff;
  out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xffffu);
  out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xffffu);
  out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xffffu);
  out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xffffu);
  return X_ERROR_SUCCESS;
}

X_RESULT GowjKbmInputDriver::SetDeviceVibration(DeviceId id,
                                                X_INPUT_VIBRATION* vibration) {
  return id == device_id_ ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT GowjKbmInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                                X_INPUT_KEYSTROKE* out_keystroke) {
  if (id != device_id_ || !out_keystroke) {
    return X_ERROR_EMPTY;
  }
  // Counted before the empty check: whether the guest ASKS is the question, and
  // an EMPTY answer is still an ask. See the KBMRATE line.
  meter_keystrokes_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(keystroke_mutex_);
  if (keystrokes_.empty()) {
    return X_ERROR_EMPTY;
  }
  *out_keystroke = keystrokes_.front();
  keystrokes_.pop_front();
  return X_ERROR_SUCCESS;
}

void GowjKbmInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  window_ = window;
  if (window_) {
    window_->AddInputListener(this, window_z_order());
    window_->AddListener(this);  // focus/minimize, for ReleaseAll
    has_focus_.store(window_->HasFocus(), std::memory_order_relaxed);
    UpdateCapture();
  }
}

void GowjKbmInputDriver::UpdateCapture() {
  bool want = look_enabled_.load(std::memory_order_relaxed) &&
              has_focus_.load(std::memory_order_relaxed) &&
              !overlay_open_.load(std::memory_order_relaxed);
  if (mouse_captured_.exchange(want, std::memory_order_relaxed) == want) {
    return;
  }
  GowjNativeSetActive(want);
  ApplyMouseCapture();
}

void GowjKbmInputDriver::ReleaseAll() {
  {
    std::lock_guard lock(bind_mutex_);
    if (!held_keys_.empty()) {
      for (auto vk : held_keys_) {
        auto it = button_bindings_.find(vk);
        if (it != button_bindings_.end()) {
          for (uint16_t bit = 1; bit; bit <<= 1) {
            if (it->second & bit) PushKeystroke(PadCodeForButton(bit), false);
          }
        }
      }
      held_keys_.clear();
      RecomputeLocked();
    }
  }
  lmb_down_.store(false, std::memory_order_relaxed);
  rmb_down_.store(false, std::memory_order_relaxed);
  mouse_dx_.store(0.0f, std::memory_order_relaxed);
  mouse_dy_.store(0.0f, std::memory_order_relaxed);
  // Re-evaluates against focus rather than forcing the user's toggle off.
  UpdateCapture();
}

void GowjKbmInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  auto vk = e.virtual_key();

  // Insert (or F1) is the pointer-lock toggle: capture is on by default so the
  // mouse aims the moment gameplay starts, and this hands the cursor back for
  // the ImGui overlays and Alt+Tabbing without leaving the window.
  if ((vk == VirtualKey::kInsert || vk == VirtualKey::kF1) &&
      e.repeat_count() <= 1) {
    bool enabled = !look_enabled_.load(std::memory_order_relaxed);
    look_enabled_.store(enabled, std::memory_order_relaxed);
    REXLOG_INFO("gowj mouse-look {}", enabled ? "ON (pointer captured)"
                                              : "OFF (cursor released)");
    UpdateCapture();
    return;
  }

  // F4 (settings) and ` (console) open the runtime's ImGui windows, which are
  // driven with a mouse cursor: hand the pointer back while one is open, take it
  // again when it is closed with the same key. The key still reaches the runtime.
  if ((vk == VirtualKey::kF4 || vk == VirtualKey::kOem3) && e.repeat_count() <= 1) {
    const bool open = !overlay_open_.load(std::memory_order_relaxed);
    overlay_open_.store(open, std::memory_order_relaxed);
    g_overlay_open.store(open, std::memory_order_relaxed);
    REXLOG_INFO("gowj overlay {} - cursor {}", open ? "opened" : "closed",
                open ? "released" : "captured");
    UpdateCapture();
    return;
  }

  std::lock_guard lock(bind_mutex_);
  if (button_bindings_.empty() && shift_dpads_.empty() &&
      axis_bindings_.empty()) {
    // InputSystem::Setup() ran before AddDriver, so our Setup() never fired - 
    // build tables lazily on first input.
    RebuildTablesLocked();
    REXLOG_INFO("gowj KBM tables built (lazy)");
  }
  const bool any_new = StoreKey(vk, true);
  if (!any_new) {
    return;  // auto-repeat
  }
  uint16_t pressed = 0;
  auto it = button_bindings_.find(vk);
  if (it != button_bindings_.end()) pressed = it->second;
  RecomputeLocked();
  if (pressed) {
    sticky_buttons_.fetch_or(pressed, std::memory_order_relaxed);
    sticky_until_ms_.store(NowMs() + 80, std::memory_order_relaxed);
    for (uint16_t bit = 1; bit; bit <<= 1) {
      if (pressed & bit) PushKeystroke(PadCodeForButton(bit), true);
    }
  }
}

void GowjKbmInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  auto vk = e.virtual_key();
  std::lock_guard lock(bind_mutex_);
  if (!StoreKey(vk, false)) {
    return;
  }
  uint16_t released = 0;
  auto it = button_bindings_.find(vk);
  if (it != button_bindings_.end()) released = it->second;
  RecomputeLocked();
  if (released) {
    for (uint16_t bit = 1; bit; bit <<= 1) {
      if (released & bit) PushKeystroke(PadCodeForButton(bit), false);
    }
  }
}

void GowjKbmInputDriver::OnKeyChar(rex::ui::KeyEvent& e) {
  // Forward printable characters as pad keystrokes (text-entry dialogs).
  auto vk = e.virtual_key();
  if (vk == VirtualKey::kNone) return;
  X_INPUT_KEYSTROKE ks{};
  ks.virtual_key = static_cast<uint16_t>(vk);
  ks.unicode = 0;
  ks.flags = X_INPUT_KEYSTROKE_KEYDOWN;
  ks.user_index = 0;
  std::lock_guard lock(keystroke_mutex_);
  if (keystrokes_.size() < 64) keystrokes_.push_back(ks);
}

void GowjKbmInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  using Button = rex::ui::MouseEvent::Button;
  if (InMenus()) {
    // Menus have no cursor (Scaleform, pad-driven): left click accepts, right
    // click goes back.
    if (e.button() == Button::kLeft) PulseButton(kBtnA);
    if (e.button() == Button::kRight) PulseButton(kBtnB);
    return;
  }
  if (e.button() == Button::kLeft) {
    lmb_down_.store(true, std::memory_order_relaxed);
  } else if (e.button() == Button::kRight) {
    rmb_down_.store(true, std::memory_order_relaxed);
  } else {
    return;
  }
  {
    std::lock_guard lock(bind_mutex_);
    RecomputeLocked();
  }
  PushKeystroke(e.button() == Button::kLeft ? VirtualKey::kXInputPadRTrigger
                                            : VirtualKey::kXInputPadLTrigger,
                true);
}

void GowjKbmInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  using Button = rex::ui::MouseEvent::Button;
  if (e.button() == Button::kLeft) {
    lmb_down_.store(false, std::memory_order_relaxed);
  } else if (e.button() == Button::kRight) {
    rmb_down_.store(false, std::memory_order_relaxed);
  } else {
    return;
  }
  {
    std::lock_guard lock(bind_mutex_);
    RecomputeLocked();
  }
  PushKeystroke(e.button() == Button::kLeft ? VirtualKey::kXInputPadRTrigger
                                            : VirtualKey::kXInputPadLTrigger,
                false);
}

void GowjKbmInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!mouse_captured_.load(std::memory_order_relaxed)) return;
  if (InMenus() && REXCVAR_GET(mnk_menu_mouse_nav)) {
    // Mouse travel steps the menu focus one d-pad press per mnk_menu_mouse_step
    // pixels, on the dominant axis, so a flick moves several entries and a small
    // tremor moves none.
    const float step = static_cast<float>(std::max(20.0, REXCVAR_GET(mnk_menu_mouse_step)));
    float nx = menu_nav_x_.load(std::memory_order_relaxed) + e.dx();
    float ny = menu_nav_y_.load(std::memory_order_relaxed) + e.dy();
    if (std::fabs(ny) >= step && std::fabs(ny) >= std::fabs(nx)) {
      PulseButton(ny > 0 ? kBtnDown : kBtnUp);
      ny -= ny > 0 ? step : -step;
      nx = 0;
    } else if (std::fabs(nx) >= step) {
      PulseButton(nx > 0 ? kBtnRight : kBtnLeft);
      nx -= nx > 0 ? step : -step;
      ny = 0;
    }
    menu_nav_x_.store(nx, std::memory_order_relaxed);
    menu_nav_y_.store(ny, std::memory_order_relaxed);
  }
  GowjNativeNoteKbmActivity();
  mouse_dx_.store(mouse_dx_.load(std::memory_order_relaxed) + e.dx(),
                  std::memory_order_relaxed);
  mouse_dy_.store(mouse_dy_.load(std::memory_order_relaxed) + e.dy(),
                  std::memory_order_relaxed);
  ++meter_moves_;
  meter_dx_.fetch_add(static_cast<int64_t>(e.dx()), std::memory_order_relaxed);
  meter_dy_.fetch_add(static_cast<int64_t>(e.dy()), std::memory_order_relaxed);
}

void GowjKbmInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (e.scroll_y() == 0) {
    return;
  }
  // Weapon swap. The in-game tutorial prompt confirmed pad Y is "swap between
  // your primary weapons"; the bumpers are melee/grenade, so a wheel detent must
  // tap Y, not LB/RB. Judgment carries a limited kit, so either direction just
  // toggles the swap button.
  if (GowjNativeKeysDriving()) {
    // Native keys drive the game: the wheel becomes MouseScrollUp/Down key events.
    GowjNativeNoteWheel(e.scroll_y() > 0 ? 1 : -1);
  } else if (InMenus()) {
    PulseButton(e.scroll_y() > 0 ? kBtnUp : kBtnDown);  // wheel scrolls menus
  } else {
    PulseButton(kBtnY);
  }
  e.set_handled(true);
}

void GowjKbmInputDriver::PulseButton(uint16_t mask) {
  sticky_buttons_.fetch_or(mask, std::memory_order_relaxed);
  sticky_until_ms_.store(NowMs() + 80, std::memory_order_relaxed);
  auto code = PadCodeForButton(mask);
  PushKeystroke(code, true);
  PushKeystroke(code, false);
}

bool GowjKbmInputDriver::StoreKey(VirtualKey vk, bool down) {
  auto track = [&](VirtualKey k) {
    return down ? held_keys_.insert(k).second : held_keys_.erase(k) != 0;
  };
  bool changed = track(vk);
  // The key names in the binding table are the generic modifier codes, but
  // Windows delivers kLShift/kRShift &c., so a press counts as both.
  VirtualKey generic = VirtualKey::kNone;
  switch (vk) {
    case VirtualKey::kLShift:
    case VirtualKey::kRShift:
      generic = VirtualKey::kShift;
      break;
    case VirtualKey::kLControl:
    case VirtualKey::kRControl:
      generic = VirtualKey::kControl;
      break;
    case VirtualKey::kLMenu:
    case VirtualKey::kRMenu:
      generic = VirtualKey::kMenu;
      break;
    default:
      break;
  }
  if (generic != VirtualKey::kNone) {
    changed |= track(generic);
  }
  return changed;
}

void GowjKbmInputDriver::ApplyMouseCapture() {
  if (!window_) return;
  bool captured = mouse_captured_.load(std::memory_order_relaxed);
  bool relative = window_->SetRelativeMouseMode(captured);
  if (captured && !relative) {
    // Without relative mode SDL keeps sending absolute positions, dx/dy stay
    // zero and the look backlog never fills - the exact signature of "the
    // mouse does nothing in game". Say so instead of failing silently.
    REXLOG_WARN("gowj SetRelativeMouseMode(true) refused; mouse look will be dead");
  }
  if (captured) {
    window_->CaptureMouse();
  } else {
    window_->ReleaseMouse();
  }
  window_->SetCursorVisibility(captured
                                   ? rex::ui::Window::CursorVisibility::kHidden
                                   : rex::ui::Window::CursorVisibility::kVisible);
#ifdef _WIN32
  // SDL's relative mode does not confine the OS cursor: an accumulated look
  // delta parks the invisible cursor on the title bar, where the next LMB hits
  // the close button (measured 15:33 - a scripted look+fire burst
  // closed the window mid-combat). Clip to the client rect while captured; the
  // clip is process-global, so it must be dropped with the capture, which the
  // focus-loss path guarantees via ReleaseAll -> UpdateCapture.
  HWND hwnd = static_cast<HWND>(window_->GetNativeWindowHandle());
  if (captured && hwnd) {
    RECT r;
    if (GetClientRect(hwnd, &r)) {
      ClientToScreen(hwnd, reinterpret_cast<POINT*>(&r.left));
      ClientToScreen(hwnd, reinterpret_cast<POINT*>(&r.right));
      ClipCursor(&r);
    }
  } else {
    ClipCursor(nullptr);
  }
#endif
  REXLOG_INFO("gowj capture -> {} (relative={} hwnd={})", captured ? "ON" : "OFF",
              relative, reinterpret_cast<uintptr_t>(window_->GetNativeWindowHandle()));
}

}  // namespace rex::input
