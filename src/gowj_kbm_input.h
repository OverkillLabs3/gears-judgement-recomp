// gowj - keyboard+mouse -> Xbox 360 pad driver.
//
// Replaces the rexruntime mnk driver. Both the mnk driver and an app-side
// driver registered through RuntimeConfig::input_factory on top of
// CreateDefaultInputSystem() crash InputSystem::RefreshDevices, because that
// helper has already run Setup() and AddDriver() afterwards leaves the
// per-driver bookkeeping pointing at uninitialised heap. Measured
// under cdb: at vector<DeviceInfo>::clear+0x61 (RefreshDevices+0x970 <-
// GetState+0x40 <- XamInputGetState_entry) rcx=0 with
// rax=baadf00d`00000010 rdx=baadf00d`00000011, and the same race surfaces as
// c0000374 / "Invalid address specified to RtlFreeHeap". GowjInputSystem below
// adds drivers before the single Setup(), which is what removes that class of
// crash. This driver reports exactly one stable synthetic device (no
// enumeration churn) and talks to guest threads through atomics only.

#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace rex::input {

/// Apply the project's preferred keyboard scheme to any keybind_* cvar still at
/// its compiled-in default. Must run after cvar::LoadConfig so a user's config
/// file, REX_* or --keybind_x= always wins.
void ApplyPreferredKeybindings();

class GowjKbmInputDriver final : public InputDriver,
                                 public rex::ui::WindowListener,
                                 public rex::ui::WindowInputListener {
 public:
  GowjKbmInputDriver() : InputDriver(nullptr, 1000) {}
  ~GowjKbmInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // Focus loss must drop mouse capture *and* the held-key set: Windows routes
  // no WM_KEYUP for a key that was upped while another window owned focus, so
  // without this the guest keeps running forward forever after an alt-tab.
  // Regaining focus re-arms capture, so Alt+Tab out and back is the whole
  // workflow - no key press needed to get aiming back.
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    has_focus_.store(false, std::memory_order_relaxed);
    ReleaseAll();
  }
  void OnGotFocus(rex::ui::UISetupEvent&) override {
    has_focus_.store(true, std::memory_order_relaxed);
    // UpdateCapture, NOT ApplyMouseCapture: the captured flag is only ever
    // recomputed there. Calling ApplyMouseCapture here re-applied the stale
    // flag from OnWindowAvailable (where HasFocus() is still false during
    // window creation), so a window focused after boot never captured the
    // pointer - measured three "capture -> OFF" transitions, zero
    // ON, KBMRATE cap=0 for the whole session.
    UpdateCapture();
  }
  void OnMinimized(rex::ui::UIEvent&) override {
    has_focus_.store(false, std::memory_order_relaxed);
    ReleaseAll();
  }
  void OnRestored(rex::ui::UIEvent&) override {
    has_focus_.store(true, std::memory_order_relaxed);
    UpdateCapture();
  }

  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnKeyChar(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

 private:
  static constexpr uint16_t kBtnUp = X_INPUT_GAMEPAD_DPAD_UP;
  static constexpr uint16_t kBtnDown = X_INPUT_GAMEPAD_DPAD_DOWN;
  static constexpr uint16_t kBtnLeft = X_INPUT_GAMEPAD_DPAD_LEFT;
  static constexpr uint16_t kBtnRight = X_INPUT_GAMEPAD_DPAD_RIGHT;
  static constexpr uint16_t kBtnStart = X_INPUT_GAMEPAD_START;
  static constexpr uint16_t kBtnBack = X_INPUT_GAMEPAD_BACK;
  static constexpr uint16_t kBtnLThumb = X_INPUT_GAMEPAD_LEFT_THUMB;
  static constexpr uint16_t kBtnRThumb = X_INPUT_GAMEPAD_RIGHT_THUMB;
  static constexpr uint16_t kBtnLShoulder = X_INPUT_GAMEPAD_LEFT_SHOULDER;
  static constexpr uint16_t kBtnRShoulder = X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  static constexpr uint16_t kBtnA = X_INPUT_GAMEPAD_A;
  static constexpr uint16_t kBtnB = X_INPUT_GAMEPAD_B;
  static constexpr uint16_t kBtnX = X_INPUT_GAMEPAD_X;
  static constexpr uint16_t kBtnY = X_INPUT_GAMEPAD_Y;

  // Axis codes: which stick component a key pushes and to what extreme.
  enum AxisKey : uint8_t {
    kAxisNone = 0,
    kLXPos, kLXNeg, kLYPos, kLYNeg,  // ly positive = up (XInput convention)
    kRXPos, kRXNeg, kRYPos, kRYNeg,
  };

  struct Binding {
    bool shift = false;
    std::vector<rex::ui::VirtualKey> keys;
  };

  static Binding ParseBinding(const std::string& spec, const char* what);
  void RebuildTables();
  void RebuildTablesLocked();  // caller holds bind_mutex_
  // Recomposes buttons_/lt_/rt_/lx_/ly_/kb_rx_/kb_ry_ from held_keys_ + mouse
  // button atomics. Caller holds bind_mutex_ (UI thread events only).
  void RecomputeLocked();
  void ApplyMouseCapture();
  // Capture follows the user's toggle AND focus, so the pointer is only ever
  // grabbed while the game actually owns the window.
  void UpdateCapture();
  // Called from window listeners on the UI thread; clears everything the guest
  // could otherwise keep acting on.
  void ReleaseAll();
  void PushKeystroke(rex::ui::VirtualKey code, bool down);
  // Momentary button tap, used by the mouse wheel (weapon cycle).
  void PulseButton(uint16_t mask);
  // Track a physical key press/release, plus its generic modifier alias.
  // Returns true when the held set actually changed (i.e. not auto-repeat).
  bool StoreKey(rex::ui::VirtualKey vk, bool down);
  static rex::ui::VirtualKey PadCodeForButton(uint16_t mask);

  DeviceId device_id_ = static_cast<DeviceId>(0x4B424D00ull);

  std::mutex bind_mutex_;
  // Built by RebuildTables from cvars; read by RecomputeLocked.
  std::map<rex::ui::VirtualKey, uint16_t> button_bindings_;
  std::map<rex::ui::VirtualKey, uint16_t> shift_dpads_;  // Shift+key -> dpad
  std::map<rex::ui::VirtualKey, uint8_t> axis_bindings_;
  std::map<rex::ui::VirtualKey, bool> trigger_bindings_;  // true = right
  std::set<rex::ui::VirtualKey> held_keys_;               // UI thread bookkeeping

  std::atomic<uint16_t> buttons_{0};
  // Guest polls at frame rate; a fast tap can fall entirely between two
  // polls. Keep freshly pressed bits visible for a short hold window.
  std::atomic<uint16_t> sticky_buttons_{0};
  std::atomic<uint64_t> sticky_until_ms_{0};
  std::atomic<uint8_t> lt_{0}, rt_{0};
  std::atomic<int16_t> lx_{0}, ly_{0}, kb_rx_{0}, kb_ry_{0};
  uint64_t last_state_us_ = 0;
  uint64_t meter_window_start_us_ = 0;
  uint64_t meter_polls_ = 0;
  // Mouse-motion counters folded into the 1s KBMRATE line: "the mouse does
  // nothing in game" is otherwise invisible, because a dead look path and a
  // never-captured pointer produce identical gameplay. Written by the UI
  // thread's OnMouseMove, read by guest poll threads, hence atomic.
  std::atomic<uint64_t> meter_moves_{0};
  std::atomic<int64_t> meter_dx_{0}, meter_dy_{0};
  // Polls where the look debt paid out a full stick deflection, i.e. the flick
  // outran the maximum turn rate the guest's poll period can express. The
  // motion itself is not lost (it stays in the debt below); this counts how often
  // the rate ceiling binds, so mnk_full_deflect_pixels can be tuned from the log.
  std::atomic<uint64_t> meter_clamp_{0};
  // Times per second the guest called XGetKeystroke on our device. Zero for a
  // whole session means Judgment's native keyboard+mouse scheme is dead code
  // here, which decides how the player's control list can be honoured.
  std::atomic<uint64_t> meter_keystrokes_{0};
  // Unspent mouse-look, in right-stick units (see GetDeviceState). Paid out to
  // within half a unit every poll, so it is ~0 whenever the hand is still.
  float look_debt_x_ = 0.0f;
  float look_debt_y_ = 0.0f;
  std::mutex look_debt_mutex_;
  std::atomic<bool> lmb_down_{false};
  std::atomic<bool> rmb_down_{false};
  std::atomic<uint32_t> packet_{1};
  // look_enabled_ is the user's toggle (Insert / F1); mouse_captured_ is what
  // is actually applied to the window, i.e. the AND of that and focus.
  std::atomic<bool> look_enabled_{true};
  std::atomic<bool> has_focus_{true};
  // The runtime's ImGui windows (F4 settings, ` console) need a visible, free
  // cursor; while either is open the pointer is released.
  std::atomic<bool> overlay_open_{false};
  // Menu mouse navigation: vertical/horizontal travel banked toward a d-pad step.
  std::atomic<float> menu_nav_x_{0.0f};
  std::atomic<float> menu_nav_y_{0.0f};
  std::atomic<bool> mouse_captured_{false};
  std::atomic<float> mouse_dx_{0.0f}, mouse_dy_{0.0f};

  std::mutex keystroke_mutex_;
  std::deque<X_INPUT_KEYSTROKE> keystrokes_;

  rex::ui::Window* window_ = nullptr;
};

/// InputSystem that is built the way InputSystem expects: every driver added
/// before the single Setup() call. Setup() is also made idempotent, because
/// ReXApp drives the returned system through IInputSystem::Setup() too.
class GowjInputSystem final : public rex::input::InputSystem {
 public:
  explicit GowjInputSystem(rex::ui::Window* window)
      : rex::input::InputSystem(window) {}

  X_STATUS Setup() override {
    if (setup_done_) {
      REXLOG_INFO("gowj InputSystem::Setup() re-entry ignored");
      return X_STATUS_SUCCESS;
    }
    setup_done_ = true;
    auto status = rex::input::InputSystem::Setup();
    REXLOG_INFO("gowj InputSystem::Setup() -> {:#x}", status);
    return status;
  }

 private:
  bool setup_done_ = false;
};

}  // namespace rex::input
