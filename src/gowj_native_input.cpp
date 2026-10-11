// gowj - ReXGlue Recompiled Project
//
// Native keyboard and mouse input. The Xbox build only ever receives gamepad key events, so the
// pad emulation used to fake everything through a virtual controller. The engine, however, ships
// its own keyboard and mouse binding table (Xbox360-GearInput.ini: W/A/S/D, Space, C, Ctrl, F, R,
// E, Q, H, T, 1, 2, mouse buttons, ...). This module hands real key transitions to the engine's
// UPlayerInput::InputKey so that table is what drives the game.
//
// What was measured on the running game:
//   * The player input object's InputKey is virtual slot 75 (found by tracing the engine's own pad
//     calls, whose key names are known). The engine's binding table hangs off it.
//   * An FName is {index, number}; in a register it is (index << 32). Key names are looked up in
//     the engine's own name array (found by locating the slot that points back at the XboxTypeS_*
//     entries), never by string matching memory, which collides with lookalike strings.
//   * UPlayerInput's bitfield word is at +228; bit 0x80000000 is bUsingGamepad. The game sets it
//     whenever a pad key arrives, and it selects the controller prompts, so it is cleared while
//     native keys drive the game.

#include "gowj_native_input.h"

#include "gowj_keys.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_BOOL(mnk_native, true, "mnk",
                    "Deliver real keyboard and mouse events to the engine's own input layer in "
                    "gameplay (the game's W/A/S/D, Space, C, Ctrl, F, R, E, Q, H, T, 1, 2 and mouse "
                    "buttons bindings). False keeps the older virtual-gamepad emulation.");

namespace rex::glue {

namespace {

constexpr uint32_t kInputKeySlot = 75;  // GearPlayerInput::InputKey(ControllerId, FName Key, EInputEvent, amount, bGamepad); 83 = IsGamepadKey
constexpr uint32_t kBitfieldOffset = 228;       // UPlayerInput bitfield word
constexpr uint32_t kUsingGamepadBit = 0x80000000u;

std::atomic<bool> g_started{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_active{false};              // window focused and pointer captured
std::atomic<int64_t> g_last_pump_ms{0};
std::atomic<int> g_wheel{0};
std::atomic<int64_t> g_last_pad_ms{0};  // last REAL gamepad button press / stick use
std::atomic<int64_t> g_last_kbm_ms{0};
std::atomic<int64_t> g_last_axis_ms{0};  // last stick axis event the engine's pad path delivered to the player input
  // last key, mouse button, wheel or mouse movement
thread_local bool t_in_native_call = false;
std::unordered_set<uint32_t> g_swallow_buttons;  // face, shoulder and stick-click pad buttons
std::unordered_set<uint32_t> g_pad_buttons;  // name indices of the pad's button keys
std::unordered_set<uint32_t> g_pad_axes;     // ... and of its stick axes                    // pending wheel notches (+up / -down)
std::atomic<uint32_t> g_array_base{0};
std::atomic<uint32_t> g_real_self{0};
std::atomic<uint32_t> g_pi_self{0};  // the player-input object the pump last served
std::mutex g_mu;
std::unordered_map<std::string, uint32_t> g_index;  // key name -> engine name index

uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void PutBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Faulting-safe 32-bit read (array slots can lie in unmapped guest pages while probing).
bool SafeBE32(const uint8_t* p, uint32_t* out) {
  __try {
    *out = BE32(p);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The keys to watch come from the player's bindings (GOWJ_NATIVE_KEYS, set from gowj.toml at
// startup); this fallback is the stock scheme so a missing variable still gives a working game.
std::vector<KeyName> g_keys;
const char* const kWheelNames[] = {kWheelUp, kWheelDown};

void InitKeys() {
  if (!g_keys.empty()) return;
  std::string list = "W,A,S,D,SpaceBar,C,LeftControl,F,R,E,Q,H,T,One,Two,Three,Four,Slash,"
                     "LeftMouseButton,RightMouseButton,MiddleMouseButton,ThumbMouseButton";
  if (const char* e = std::getenv("GOWJ_NATIVE_KEYS")) list = e;
  size_t pos = 0;
  while (pos <= list.size()) {
    const size_t comma = list.find(',', pos);
    const std::string name = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    for (const KeyName& k : kKeyTable)
      if (name == k.name) {
        g_keys.push_back(k);
        break;
      }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
}

std::unordered_map<uint32_t, std::vector<char>> g_down_by_obj;  // per player-input object
uint32_t g_vtbl_seen = 0;
uint32_t g_target = 0;  // the player-input object native keys are delivered to

struct Cand {
  uint32_t index;
  uint32_t addr;  // guest address of the FNameEntry
};

// Walks committed read/write guest memory in 4 MB slices (yielding between slices).
template <typename F>
void ForEachSlice(uint8_t* membase, F&& fn) {
  uint8_t* p = membase;
  uint8_t* const end = membase + 0x100000000ULL;
  MEMORY_BASIC_INFORMATION mbi{};
  while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
    uint8_t* region = static_cast<uint8_t*>(mbi.BaseAddress);
    uint8_t* region_end = region + mbi.RegionSize;
    const DWORD prot = mbi.Protect & 0xFF;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
        (prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE)) {
      for (uint8_t* slice = region; slice < region_end;) {
        uint8_t* slice_end = std::min(region_end, slice + (4u << 20));
        fn(slice, slice_end);
        slice = slice_end;
        Sleep(1);
      }
    }
    p = region_end;
  }
}

// FNameEntry in this build: [index*2 (u32 BE)][hash-next pointer (u32 BE)][ANSI name][NUL],
// byte-packed in the name pool (entries are not 4-aligned). XboxTypeS_A has header 0x2C0A =
// 5637*2, and 5637 is what the game's LastAxisKeyName holds after that pad key.
//
// The engine's name array (FName::Names: entry pointers indexed by name index) is located by
// finding the slot that points back at the XboxTypeS_* entries, then every wanted key name is
// read from it by walking the array, so a lookalike string elsewhere cannot be mistaken for a
// name.
void ScanOnce(uint8_t* membase) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  const ULONGLONG t0 = GetTickCount64();
  const char* const kAnchors[] = {"XboxTypeS_A", "XboxTypeS_B", "XboxTypeS_X",  "XboxTypeS_Y",
                                  "XboxTypeS_Start", "XboxTypeS_Back", "XboxTypeS_LeftX"};
  std::unordered_set<std::string> anchor_set(std::begin(kAnchors), std::end(kAnchors));
  std::unordered_map<std::string, std::vector<Cand>> anchors;
  ForEachSlice(membase, [&](uint8_t* s, uint8_t* e) {
    for (uint8_t* it = s; it + 16 <= e; ++it) {
      if (it[8] != 'X' || it[9] != 'b') continue;
      const uint32_t w0 = BE32(it);
      if ((w0 & 1) || (w0 >> 1) > 400000) continue;
      const uint32_t w1 = BE32(it + 4);
      if (w1 != 0 && (w1 < 0x00100000 || w1 >= 0x90000000)) continue;
      const uint8_t* n = it + 8;
      size_t len = 0;
      while (len < 24 && n + len < e && n[len] >= 32 && n[len] < 127) ++len;
      if (len == 0 || len >= 24 || n[len] != 0) continue;
      std::string name(reinterpret_cast<const char*>(n), len);
      if (anchor_set.count(name)) anchors[name].push_back({w0 >> 1, uint32_t(it - membase)});
    }
  });

  uint32_t array_base = 0;
  // The engine can hold more than one copy of its name table (an older one that stops growing, and the live
  // one that gets the later names such as the one-letter key names). Every table that matches the pad anchors
  // is read and the results are combined, so it does not matter which one is found first.
  std::vector<uint32_t> good_bases;
  std::vector<Cand> first;
  for (const char* k : kAnchors)
    for (const Cand& c : anchors[k]) first.push_back(c);
  {
    // One pass over memory: every word that equals the address of an anchor entry is a possible table slot.
    std::unordered_map<uint32_t, std::vector<uint32_t>> by_addr;  // entry address -> its name indices
    for (const Cand& c : first) by_addr[c.addr].push_back(c.index);
    std::vector<uint32_t> bases;
    ForEachSlice(membase, [&](uint8_t* s, uint8_t* e) {
      for (uint8_t* it = s; it + 4 <= e; it += 4) {
        auto f = by_addr.find(BE32(it));
        if (f == by_addr.end()) continue;
        const uint32_t loc = uint32_t(it - membase);
        for (uint32_t index : f->second)
          if (loc >= index * 4) bases.push_back(loc - index * 4);
      }
    });
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    for (uint32_t b : bases) {
      int ok = 0;
      for (const char* k : kAnchors) {
        for (const Cand& c : anchors[k]) {
          uint32_t v = 0;
          if (SafeBE32(membase + b + c.index * 4, &v) && v == c.addr) {
            ++ok;
            break;
          }
        }
      }
      if (ok >= 5) good_bases.push_back(b);
    }
  }
  if (!good_bases.empty()) array_base = good_bases.front();

  std::unordered_set<std::string> want;
  for (const KeyName& k : g_keys) want.insert(k.name);
  for (const char* w : kWheelNames) want.insert(w);
  static const char* const kPadButtons[] = {
      "XboxTypeS_A", "XboxTypeS_B", "XboxTypeS_X", "XboxTypeS_Y", "XboxTypeS_LeftShoulder",
      "XboxTypeS_RightShoulder", "XboxTypeS_Start", "XboxTypeS_Back", "XboxTypeS_LeftThumbstick",
      "XboxTypeS_RightThumbstick", "XboxTypeS_DPad_Up", "XboxTypeS_DPad_Down", "XboxTypeS_DPad_Left",
      "XboxTypeS_DPad_Right", "XboxTypeS_LeftTrigger", "XboxTypeS_RightTrigger"};
  static const char* const kPadAxes[] = {"XboxTypeS_LeftX", "XboxTypeS_LeftY", "XboxTypeS_RightX",
                                         "XboxTypeS_RightY", "XboxTypeS_LeftTriggerAxis",
                                         "XboxTypeS_RightTriggerAxis"};
  for (const char* w : kPadButtons) want.insert(w);
  for (const char* w : kPadAxes) want.insert(w);
#ifdef GOWJ_DEV_TOOLS
  for (const char* w : {"XboxTypeS_A", "XboxTypeS_X", "XboxTypeS_B", "XboxTypeS_Y"}) want.insert(w);
#endif
  std::unordered_map<std::string, uint32_t> got;
  g_array_base.store(array_base);
  for (const uint32_t ab : good_bases) {
    uint32_t misses = 0;
    uint32_t max_anchor = 1000;
    for (auto& kv : anchors)
      for (const Cand& c : kv.second) max_anchor = std::max(max_anchor, c.index);
    for (uint32_t idx = 0; idx < 400000; ++idx) {
      uint32_t ent = 0;
      if (!SafeBE32(membase + ab + idx * 4, &ent)) {
        idx |= 0x3FF;  // an unreadable page inside the table: skip it (1024 entries) and carry on
        continue;
      }
      if (ent < 0x00100000 || ent >= 0x90000000) {
        if (idx > max_anchor + 64 && ++misses > 64) break;  // end of the array (never before the known anchors)
        continue;
      }
      misses = 0;
      const uint8_t* n = membase + ent + 8;
      uint32_t probe = 0;
      if (!SafeBE32(n, &probe)) continue;
      size_t len = 0;
      while (len < 24 && n[len] >= 32 && n[len] < 127) ++len;
      if (len == 0 || len >= 24 || n[len] != 0) continue;
      std::string name(reinterpret_cast<const char*>(n), len);
      if (want.count(name) && !got.count(name)) got[name] = idx;
    }
  }
  // Names created early in a launch push the key names past the first chunk of the name table (16384
  // entries per chunk). The chunk base is found from the longer names that sit in it, then that whole
  // chunk is read like the first one (this is also where the one-letter key names are found).
  bool keys_missing = false;
  for (const KeyName& k : g_keys) keys_missing |= !got.count(k.name);
  if (keys_missing) {
    constexpr uint32_t kChunk = 16384;
    struct NameCand {
      std::string name;
      uint32_t index, addr;
    };
    std::vector<NameCand> nc;
    std::vector<std::string> need;
    for (const std::string& w : want)
      if (!got.count(w) && w.size() >= 4) need.push_back(w);
    if (!need.empty()) {
      ForEachSlice(membase, [&](uint8_t* s, uint8_t* e) {
        for (uint8_t* it = s; it + 40 <= e; ++it) {
          const uint8_t c0 = it[8];
          if (c0 < 'A') continue;
          for (const std::string& w : need) {
            if (uint8_t(w[0]) != c0 || std::memcmp(it + 8, w.c_str(), w.size() + 1) != 0) continue;
            const uint32_t w0 = BE32(it), w1 = BE32(it + 4);
            if ((w0 & 1) || (w0 >> 1) >= 400000 || (w0 >> 1) == 0) continue;
            if (w1 != 0 && (w1 < 0x00100000 || w1 >= 0x90000000)) continue;
            nc.push_back({w, w0 >> 1, uint32_t(it - membase)});
          }
        }
      });
    }
    std::unordered_map<uint32_t, std::vector<size_t>> by_addr;
    for (size_t i = 0; i < nc.size(); ++i) by_addr[nc[i].addr].push_back(i);
    std::map<std::pair<uint32_t, uint32_t>, int> votes;  // (chunk number, chunk base) -> names agreeing
    if (!nc.empty()) {
      ForEachSlice(membase, [&](uint8_t* s, uint8_t* e) {
        for (uint8_t* it = s; it + 4 <= e; it += 4) {
          auto f = by_addr.find(BE32(it));
          if (f == by_addr.end()) continue;
          const uint32_t loc = uint32_t(it - membase);
          for (size_t i : f->second) {
            const uint32_t off = (nc[i].index % kChunk) * 4;
            if (loc >= off) ++votes[{nc[i].index / kChunk, loc - off}];
          }
        }
      });
    }
    std::map<uint32_t, std::pair<uint32_t, int>> best;  // chunk number -> (base, votes)
    for (const auto& kv : votes) {
      auto& b = best[kv.first.first];
      if (kv.second > b.second) b = {kv.first.second, kv.second};
    }
    for (const auto& kv : best) {
      if (kv.second.second < 2) continue;
      const uint32_t base0 = kv.second.first;
      for (uint32_t i = 0; i < kChunk; ++i) {
        uint32_t ent = 0;
        if (!SafeBE32(membase + base0 + i * 4, &ent) || ent < 0x00100000 || ent >= 0x90000000) continue;
        const uint8_t* nm = membase + ent + 8;
        uint32_t probe = 0;
        if (!SafeBE32(nm, &probe)) continue;
        size_t len = 0;
        while (len < 24 && nm[len] >= 32 && nm[len] < 127) ++len;
        if (len == 0 || len >= 24 || nm[len] != 0) continue;
        std::string name(reinterpret_cast<const char*>(nm), len);
        if (want.count(name) && !got.count(name)) got[name] = kv.first * kChunk + i;
      }
      REXLOG_INFO("NATIVEINPUT name table chunk {} found at 0x{:08X} ({} names agree)", kv.first, base0, kv.second.second);
    }
  }
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_index = std::move(got);
    for (const char* w : kPadButtons) {
      auto f = g_index.find(w);
      if (f != g_index.end()) g_pad_buttons.insert(f->second);
    }
    for (const char* w : {"XboxTypeS_A", "XboxTypeS_B", "XboxTypeS_X", "XboxTypeS_Y", "XboxTypeS_LeftShoulder",
                          "XboxTypeS_RightShoulder", "XboxTypeS_LeftThumbstick", "XboxTypeS_RightThumbstick"}) {
      auto f = g_index.find(w);
      if (f != g_index.end()) g_swallow_buttons.insert(f->second);
    }
    for (const char* w : kPadAxes) {
      auto f = g_index.find(w);
      if (f != g_index.end()) g_pad_axes.insert(f->second);
    }
  }
  std::string missing;
  for (const std::string& w : want)
    if (!g_index.count(w)) missing += w + " ";
  REXLOG_INFO("NATIVEINPUT name array base=0x{:08X} resolved {}/{} in {} ms{}{}", array_base,
              g_index.size(), want.size(), GetTickCount64() - t0, missing.empty() ? "" : "; missing: ",
              missing);
  g_ready.store(g_index.size() >= want.size() / 2, std::memory_order_release);
}

// The name table is not complete early in a launch (a scan right after the first player input object
// appears has read an empty or stale table and resolved nothing), so a failed scan is repeated.
void ScanThread(uint8_t* membase) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    ScanOnce(membase);
    if (g_ready.load(std::memory_order_acquire)) return;
    REXLOG_WARN("NATIVEINPUT name scan {} did not resolve the key names; retrying in 10 s", attempt + 1);
    Sleep(10000);
  }
}

// Runs a guest function and reports whether it faulted (a host access violation inside the
// recompiled code). The context passed in is a private copy, so a fault cannot corrupt the
// caller's registers.
bool SafeInvoke(PPCFunc* f, PPCContext* c, uint8_t* base) {
  __try {
    f(*c, base);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::unordered_set<uint32_t> g_bad_objects;  // player-input objects whose InputKey faulted

bool CallInputKey(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t name_index, int event) {
  auto* rt = rex::Runtime::instance();
  auto* fd = rt ? rt->function_dispatcher() : nullptr;
  if (!fd) return false;
  const uint32_t vt = BE32(base + self);
  const uint32_t fn = BE32(base + vt + kInputKeySlot * 4);
  PPCFunc* f = fd->GetFunction(fn);
  if (!f) return false;
  if (std::getenv("GOWJ_NATIVEINPUT_LOG"))
    REXLOG_INFO("NATIVEINPUT call self=0x{:08X} fn=0x{:08X} name={} ev={} r1=0x{:08X}", self, fn, name_index, event, ctx.r1.u32);
  PPCContext c = ctx;
  c.r3.u64 = self;
  c.r4.u64 = 0;                           // ControllerId
  c.r5.u64 = uint64_t(name_index) << 32;  // FName {index, number 0}
  c.r6.u64 = uint64_t(event);             // EInputEvent: 0 pressed, 1 released
  c.r7.u64 = 0;                           // bGamepad = false
  c.r8.u64 = 0;                           // (the game also reads bGamepad from r8)
  c.f1.f64 = event == 0 ? 1.0 : 0.0;      // amount
  t_in_native_call = true;
  const bool ok = SafeInvoke(f, &c, base);
  t_in_native_call = false;
  if (!ok) {
    REXLOG_WARN("NATIVEINPUT InputKey faulted: object 0x{:08X} name {} event {}", self, name_index, event);
    if (!std::getenv("GOWJ_NATIVEINPUT_NOBAD")) {
      g_bad_objects.insert(self);
      if (g_target == self) g_target = 0;  // pick the next candidate
    }
  }
  return ok;
}


// The engine sets bUsingGamepad inside InputKey / InputAxis whenever a pad key arrives (the
// virtual pad still sends stick axes every frame). While native keys are live, wrap both slots
// and put the bit back, so the prompt code never sees a pad.
PPCFunc* g_orig_key = nullptr;
PPCFunc* g_orig_axis = nullptr;
std::atomic<bool> g_hooks_installed{false};

void ClearGamepadBit(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  if (!self || self != g_pi_self.load(std::memory_order_relaxed)) return;
  const uint32_t w = BE32(base + self + kBitfieldOffset);
  if (w & kUsingGamepadBit) PutBE32(base + self + kBitfieldOffset, w & ~kUsingGamepadBit);
}
extern "C" int64_t GowjVirtualFaceMs();
extern "C" int64_t GowjVirtualAnyMs();
void HookKey(PPCContext& ctx, uint8_t* base) {
  if (!t_in_native_call) {
    const uint32_t idx = uint32_t(ctx.r5.u64 >> 32);
    const int64_t now = NowMs();
    if (std::getenv("GOWJ_NATIVEINPUT_LOG")) {
      static int seen = 0;
      if (seen++ < 40)
        REXLOG_INFO("NATIVEINPUT engine key idx={} ev={} driving={} swallow_set={} face_ago={} any_ago={} pad_btn={} pad_axis={}", idx, ctx.r6.u32,
                    GowjNativeKeysDriving(), g_swallow_buttons.count(idx), now - GowjVirtualFaceMs(), now - GowjVirtualAnyMs(),
                    g_pad_buttons.count(idx), g_pad_axes.count(idx));
    }
    // A pad press the keyboard produced (the virtual pad keeps menus working). When native keys are
    // driving gameplay the same action was already sent as a key, so this duplicate is dropped; it is
    // only ever seen here when the UI did not take it (menus consume it first). It is not real pad use.
    if (GowjNativeKeysDriving() && g_swallow_buttons.count(idx) && now - GowjVirtualFaceMs() < 250) {
      static int logged = 0;
      if (logged++ < 6) REXLOG_INFO("NATIVEINPUT dropped duplicate pad press from the virtual pad (name index {}, event {})", idx, ctx.r6.u32);
      ctx.r3.u64 = 0;
      return;
    }
    if (now - GowjVirtualAnyMs() < 250) {
      // A keyboard press that reaches the engine as a pad button (the virtual pad keeps menus working).
      // Tell the engine it is not from a gamepad, otherwise it switches the menus to controller art.
#ifdef GOWJ_DEV_TOOLS
      static const bool keep_gamepad_arg = std::getenv("GOWJ_NATIVEINPUT_KEEP_BGAMEPAD") != nullptr;
      if (!keep_gamepad_arg)
#endif
      {
        ctx.r7.u64 = 0;
        ctx.r8.u64 = 0;
      }
      g_orig_key(ctx, base);
      if (GowjNativePreferKbm()) ClearGamepadBit(ctx, base);
      return;
    }
    // A call from the engine's own pad path: real pad use (button press, or a stick pushed past
    // a deadzone) marks the pad as the last-used device.
    const bool button = g_pad_buttons.count(idx) != 0 && ctx.r6.u32 == 0;
    const bool axis = g_pad_axes.count(idx) != 0 && std::fabs(ctx.f1.f64) > 0.35;
    if (button || axis) g_last_pad_ms.store(NowMs(), std::memory_order_relaxed);
  }
  g_orig_key(ctx, base);
  if (GowjNativePreferKbm()) ClearGamepadBit(ctx, base);
}
void HookAxis(PPCContext& ctx, uint8_t* base) {
  // Only the controller that is really being fed (stick axes arrive here every frame) is the
  // player's input object; the look hook can also fire for other, uninitialised instances.
  if (!t_in_native_call) g_last_axis_ms.store(NowMs(), std::memory_order_relaxed);
  g_orig_axis(ctx, base);
  if (GowjNativeKeysLive()) ClearGamepadBit(ctx, base);
}

// The prompt strings ("press [key] to ...") are built by these two datastore natives, which
// consult the same flag. Clear it right before they run so the bound-key text is chosen.
PPCFunc* g_orig_bound = nullptr;
PPCFunc* g_orig_field = nullptr;
void ClearForPrompts(PPCContext& ctx, uint8_t* base) {
  if (!GowjNativeKeysLive()) return;
  const uint32_t self = g_pi_self.load(std::memory_order_relaxed);
  if (!self) return;
  const uint32_t w = BE32(base + self + kBitfieldOffset);
  if (w & kUsingGamepadBit) PutBE32(base + self + kBitfieldOffset, w & ~kUsingGamepadBit);
}
void HookBound(PPCContext& ctx, uint8_t* base) {
  ClearForPrompts(ctx, base);
  g_orig_bound(ctx, base);
}
void HookField(PPCContext& ctx, uint8_t* base) {
  ClearForPrompts(ctx, base);
  g_orig_field(ctx, base);
}

void InstallSlotHooks(uint8_t* base, uint32_t vt) {
  if (g_hooks_installed.exchange(true)) return;
  if (std::getenv("GOWJ_NATIVEINPUT_NOHOOKS")) return;
  auto* rt = rex::Runtime::instance();
  auto* fd = rt ? rt->function_dispatcher() : nullptr;
  if (!fd) return;
  const uint32_t key_fn = BE32(base + vt + kInputKeySlot * 4);
  g_orig_key = fd->GetFunction(key_fn);
  if (!g_orig_key) return;
  const bool a = fd->SetFunction(key_fn, &HookKey);
  REXLOG_INFO("NATIVEINPUT InputKey hook at 0x{:08X}: {}", key_fn, a);
}

#ifdef GOWJ_DEV_TOOLS
// Dev tracer (GOWJ_NATIVEINPUT_TRACE=1): wraps vtable slots 66..90 and logs which one receives pad
// button names (not the per-frame stick axes) - that is the real InputKey.
constexpr int kTF = 66, kTL = 90;
PPCFunc* g_trace_orig[kTL + 1] = {};
std::atomic<int> g_trace_hits[kTL + 1] = {};

std::string NameOfIndex(uint8_t* base, uint32_t idx) {
  const uint32_t ab = g_array_base.load();
  if (!ab || idx > 400000) return "?";
  uint32_t ent = 0;
  if (!SafeBE32(base + ab + idx * 4, &ent) || ent < 0x00100000 || ent >= 0x90000000) return "?";
  const uint8_t* n = base + ent + 8;
  uint32_t probe = 0;
  if (!SafeBE32(n, &probe)) return "?";
  std::string s;
  for (int i = 0; i < 40 && n[i] >= 32 && n[i] < 127; ++i) s.push_back(char(n[i]));
  return s;
}
template <int kSlot>
void TraceSlot(PPCContext& ctx, uint8_t* base) {
  if (g_trace_hits[kSlot].load() < 3) {
    const std::string nm = NameOfIndex(base, uint32_t(ctx.r4.u64 >> 32));
    const std::string nm5 = NameOfIndex(base, uint32_t(ctx.r5.u64 >> 32));
    if (nm5 == "XboxTypeS_X" || nm5 == "XboxTypeS_B") {
      g_trace_hits[kSlot].fetch_add(1);
      REXLOG_INFO("SLOTTRACE slot={} key-in-r5={} r4=0x{:X} r6=0x{:X} f1={:.3f}", kSlot, nm5, ctx.r4.u32, ctx.r6.u32, ctx.f1.f64);
    }
    if (nm == "XboxTypeS_X" || nm == "XboxTypeS_B") {
      g_trace_hits[kSlot].fetch_add(1);
      REXLOG_INFO("SLOTTRACE slot={} key={} r5=0x{:X} f1={:.3f} f2={:.3f}", kSlot, nm, ctx.r5.u32,
                  ctx.f1.f64, ctx.f2.f64);
    }
  }
  g_trace_orig[kSlot](ctx, base);
}
template <int... S>
void InstallTrace(std::integer_sequence<int, S...>, uint8_t* base, uint32_t vt) {
  auto* rt = rex::Runtime::instance();
  auto* fd = rt ? rt->function_dispatcher() : nullptr;
  if (!fd) return;
  auto one = [&](int slot, PPCFunc* hook) {
    const uint32_t target = BE32(base + vt + slot * 4);
    PPCFunc* orig = fd->GetFunction(target);
    if (!orig || g_trace_orig[slot]) return;
    g_trace_orig[slot] = orig;
    fd->SetFunction(target, hook);
  };
  (one(kTF + S, &TraceSlot<kTF + S>), ...);
}
#endif

void Send(PPCContext& ctx, uint8_t* base, uint32_t self, const std::string& name, int event) {
  static const bool dbg = std::getenv("GOWJ_NATIVEINPUT_LOG") != nullptr;
  auto f = g_index.find(name);
  if (dbg) REXLOG_INFO("NATIVEINPUT send {} ev={} bad={} found={}", name, event, g_bad_objects.count(self) != 0, f != g_index.end());
  if (g_bad_objects.count(self)) return;
  if (f == g_index.end()) return;
  CallInputKey(ctx, base, self, f->second, event);
}

#ifdef GOWJ_DEV_TOOLS
// Dev key source for headless tests: gowj_keys.txt holds the key names currently held.
std::mutex g_dev_mu;
std::unordered_set<std::string> g_dev_keys;
std::once_flag g_dev_once;
void DevKeyThread() {
  for (;;) {
    std::unordered_set<std::string> r;
    {
      std::ifstream f("gowj_keys.txt");
      std::string t;
      while (f >> t) r.insert(t);
    }
    {
      std::lock_guard<std::mutex> l(g_dev_mu);
      g_dev_keys = std::move(r);
    }
    Sleep(20);
  }
}
std::unordered_set<std::string> ReadDevKeys() {
  std::call_once(g_dev_once, [] { std::thread(DevKeyThread).detach(); });
  std::lock_guard<std::mutex> l(g_dev_mu);
  return g_dev_keys;
}
std::unordered_set<std::string> g_dev_held;
#endif

}  // namespace

// The pump ran within the last 400 ms: the engine's bindings are driving gameplay, so the virtual
// pad must not also press the same buttons.
extern "C" bool GowjNativeKeysDriving() {
#ifdef GOWJ_DEV_TOOLS
  // Test rigs: GOWJ_NATIVEINPUT_FAKE_DRIVING=1 behaves as if the key pump were live (as it is in a release
  // build once the key names resolve), so the menu path can be tested with an injected pad.
  static const bool fake = std::getenv("GOWJ_NATIVEINPUT_FAKE_DRIVING") != nullptr;
  if (fake) return true;
#endif
  if (!REXCVAR_GET(mnk_native) || !g_ready.load(std::memory_order_acquire)) return false;
  return NowMs() - g_last_pump_ms.load(std::memory_order_relaxed) < 400;
}

// Keyboard and mouse was the most recently used device (a real pad used after it wins until the
// next key or mouse movement).
extern "C" bool GowjNativePreferKbm() {
  return g_last_kbm_ms.load(std::memory_order_relaxed) >= g_last_pad_ms.load(std::memory_order_relaxed);
}

extern "C" bool GowjNativeKeysLive() { return GowjNativeKeysDriving() && GowjNativePreferKbm(); }

extern "C" void GowjNativeNoteKbmActivity() {
  g_last_kbm_ms.store(NowMs(), std::memory_order_relaxed);
}

// Called around the engine's own look code every frame: while the keyboard and mouse are the active
// device the bUsingGamepad bit must never be left set, because prompts and HUD parts read it from other
// threads and would draw the controller version for as long as it stays on (visible as flicker).
extern "C" void GowjNativeClearGamepadFlag(uint8_t* base, uint32_t self) {
  if (!self || !GowjNativeKeysLive()) return;
  const uint32_t w = BE32(base + self + kBitfieldOffset);
  if (w & kUsingGamepadBit) PutBE32(base + self + kBitfieldOffset, w & ~kUsingGamepadBit);
}

extern "C" void GowjNativeSetActive(bool active) { g_active.store(active, std::memory_order_relaxed); }

extern "C" void GowjNativeNoteWheel(int direction) {
  g_wheel.fetch_add(direction > 0 ? 1 : -1, std::memory_order_relaxed);
}

void NativeInputStartNameScan(uint8_t* base) {
  if (g_started.exchange(true)) return;
  InitKeys();
  std::thread(ScanThread, base).detach();
}

void NativeInputPump(PPCContext& ctx, uint8_t* base, uint32_t self) {
  if (const char* f = std::getenv("GOWJ_NATIVEINPUT_FORCE")) self = uint32_t(std::strtoul(f, nullptr, 16));
  static const bool dbg = std::getenv("GOWJ_NATIVEINPUT_LOG") != nullptr;
  if (dbg) {
    static int64_t last_ms = 0;
    const int64_t now = NowMs();
    if (now - last_ms > 2000) {
      last_ms = now;
      char cwd[260] = {};
      GetCurrentDirectoryA(260, cwd);
      std::ifstream kf("gowj_keys.txt");
      std::string first;
      kf >> first;
      REXLOG_INFO("NATIVEINPUT axis_ms_ago={}", now - g_last_axis_ms.load());
      REXLOG_INFO("NATIVEINPUT pump self=0x{:08X} ready={} native={} active={} target=0x{:08X} cwd={} keysfile_open={} first='{}'", self,
                  g_ready.load(), REXCVAR_GET(mnk_native), g_active.load(), g_target, cwd, kf.is_open(), first);
    }
  }
  if (!self || !g_ready.load(std::memory_order_acquire) || !REXCVAR_GET(mnk_native)) return;
  // The object must still carry the vtable it had when first seen (guards a freed or recycled
  // player-input object across level changes).
  if (dbg) { static int n_S1 = 0; if (n_S1++ < 2) REXLOG_INFO("NATIVEINPUT stage S1 self=0x{:08X} vtseen=0x{:08X}", self, g_vtbl_seen); }
  const uint32_t vt = BE32(base + self);
  if (g_vtbl_seen == 0) g_vtbl_seen = vt;
  if (vt != g_vtbl_seen) return;
  if (dbg) { static int n_S2 = 0; if (n_S2++ < 2) REXLOG_INFO("NATIVEINPUT stage S2 self=0x{:08X} vtseen=0x{:08X}", self, g_vtbl_seen); }
  InstallSlotHooks(base, vt);
#ifdef GOWJ_DEV_TOOLS
  { static bool once = false; if (!once && std::getenv("GOWJ_NATIVEINPUT_TRACE")) { once = true; InstallTrace(std::make_integer_sequence<int, kTL - kTF + 1>{}, base, vt); } }
#endif
  // One target object only. The look hook fires for every GearPlayerInput instance that is alive
  // (an old controller can linger for a moment across a level change); serving both would deliver
  // each key transition twice. The target is the object that is visited continuously: when the
  // current target has not been visited for 500 ms, the next visitor takes over.
  if (g_bad_objects.count(self)) return;
  static const bool serve_all = std::getenv("GOWJ_NATIVEINPUT_ALL") != nullptr;
  if (!serve_all) {
    static int64_t last_visit_ms = 0;
    const int64_t now_ms = NowMs();
    if (g_target == 0 || (self != g_target && now_ms - last_visit_ms > 500)) {
      if (g_target != self) {
        REXLOG_INFO("NATIVEINPUT target player input object is now 0x{:08X}", self);
        g_down_by_obj[self].clear();
      }
      g_target = self;
    }
    if (self != g_target) return;
    last_visit_ms = now_ms;
  }
  std::vector<char>& g_down = g_down_by_obj[self];
  if (g_down.size() != g_keys.size()) g_down.assign(g_keys.size(), 0);
  // The pump only runs while gameplay look frames run. After a gap (pause menu, cutscene, level
  // change) the engine may still think keys are held: release them before reading the keyboard.
  {
    const int64_t gap_now = NowMs();
    const int64_t gap = gap_now - g_last_pump_ms.load(std::memory_order_relaxed);
    if (gap > 400) {
      for (size_t i = 0; i < g_keys.size(); ++i)
        if (g_down[i]) {
          Send(ctx, base, self, g_keys[i].name, 1);
          g_down[i] = 0;
        }
    }
  }
  g_pi_self.store(self, std::memory_order_relaxed);

  std::vector<char> want_down(g_keys.size(), 0);
  const bool active = g_active.load(std::memory_order_relaxed);
  if (active) {
    for (size_t i = 0; i < g_keys.size(); ++i)
      want_down[i] = (GetAsyncKeyState(g_keys[i].vk) & 0x8000) != 0;
  }
#ifdef GOWJ_DEV_TOOLS
  const auto dev = ReadDevKeys();
  for (size_t i = 0; i < g_keys.size(); ++i)
    if (dev.count(g_keys[i].name)) want_down[i] = true;
  const bool any_dev = !dev.empty() || !g_dev_held.empty();
  { static int calls = 0, nonempty = 0; static int64_t t0 = 0; ++calls; if (!dev.empty()) ++nonempty; const int64_t n = NowMs(); if (dbg && n - t0 > 1500) { t0 = n; REXLOG_INFO("NATIVEINPUT devread calls={} nonempty={}", calls, nonempty); } }
  if (!dev.empty() && dbg) { static int n = 0; if (n++ < 3) REXLOG_INFO("NATIVEINPUT dev keys present: {} (first={}) self=0x{:08X}", dev.size(), *dev.begin(), self); }
  for (const std::string& k : dev) {
    bool known = false;
    for (const KeyName& kd : g_keys) known |= (k == kd.name);
    if (!known && !g_dev_held.count(k) && !g_bad_objects.count(self) && (g_target == 0 || self == g_target || std::getenv("GOWJ_NATIVEINPUT_ALL"))) Send(ctx, base, self, k, 0);
  }
  for (const std::string& k : g_dev_held) {
    bool known = false;
    for (const KeyName& kd : g_keys) known |= (k == kd.name);
    if (!known && !dev.count(k) && !g_bad_objects.count(self)) Send(ctx, base, self, k, 1);
  }
  g_dev_held = dev;
#else
  const bool any_dev = false;
#endif
  if (dbg && any_dev) { static int n4 = 0; if (n4++ < 4) REXLOG_INFO("NATIVEINPUT stage S4 any_dev={} active={}", any_dev, active); }
  if (!active && !any_dev) {
    // Focus lost or capture released: let go of everything that is still held.
    for (size_t i = 0; i < g_keys.size(); ++i)
      if (g_down[i]) {
        Send(ctx, base, self, g_keys[i].name, 1);
        g_down[i] = false;
      }
    return;
  }

  // Native keys mean the player is on keyboard and mouse: the game must not think a pad is in use.
  {
    uint32_t w = BE32(base + self + kBitfieldOffset);
    static const bool keep_gp = std::getenv("GOWJ_NATIVEINPUT_KEEPGP") != nullptr;
    if ((w & kUsingGamepadBit) && !keep_gp && GowjNativePreferKbm()) PutBE32(base + self + kBitfieldOffset, w & ~kUsingGamepadBit);
  }
  for (size_t i = 0; i < g_keys.size(); ++i) {
    if (want_down[i] == g_down[i]) continue;
    Send(ctx, base, self, g_keys[i].name, want_down[i] ? 0 : 1);
    g_down[i] = want_down[i];
    if (want_down[i]) g_last_kbm_ms.store(NowMs(), std::memory_order_relaxed);
  }
  int wheel = g_wheel.exchange(0, std::memory_order_relaxed);
  if (wheel != 0) g_last_kbm_ms.store(NowMs(), std::memory_order_relaxed);
  while (wheel != 0) {
    const char* nm = wheel > 0 ? kWheelNames[0] : kWheelNames[1];
    Send(ctx, base, self, nm, 0);
    Send(ctx, base, self, nm, 1);
    wheel += wheel > 0 ? -1 : 1;
  }
  g_last_pump_ms.store(NowMs(), std::memory_order_relaxed);
  {
    // Remember which device was used last so the menu prompts of the next launch match it (Auto prompts).
    static int64_t next_ms = 0;
    static int last_written = -1;
    const int64_t t = NowMs();
    if (t >= next_ms) {
      next_ms = t + 3000;
      const int pref = GowjNativePreferKbm() ? 1 : 0;
      if (pref != last_written) {
        wchar_t base[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
          std::wstring dir = std::wstring(base) + L"\\GearsOfWarJudgmentPC\\user";
          CreateDirectoryW((std::wstring(base) + L"\\GearsOfWarJudgmentPC").c_str(), nullptr);
          CreateDirectoryW(dir.c_str(), nullptr);
          std::ofstream out(dir + L"\\last_device.txt", std::ios::trunc);
          out << (pref ? "kbm" : "pad") << "\n";
          last_written = pref;
        }
      }
    }
  }
  {
    static bool last_pref = true;
    const bool pref = GowjNativePreferKbm();
    if (pref != last_pref && std::getenv("GOWJ_NATIVEINPUT_LOG")) REXLOG_INFO("NATIVEINPUT preferred device -> {}", pref ? "keyboard+mouse" : "gamepad");
    last_pref = pref;
  }
  static const bool diag = std::getenv("GOWJ_NATIVEINPUT_LOG") != nullptr;
  if (diag) {
    static float last_base_y[8] = {};
    static uint32_t ids[8] = {};
    int slot = -1;
    for (int i = 0; i < 8; ++i)
      if (ids[i] == self || ids[i] == 0) { ids[i] = self; slot = i; break; }
    uint32_t raw = BE32(base + self + 256);
    float v;
    std::memcpy(&v, &raw, 4);
    if (slot >= 0 && v != last_base_y[slot]) {
      last_base_y[slot] = v;
      REXLOG_INFO("NATIVEINPUT object 0x{:08X} aBaseY -> {:.2f}", self, v);
    }
  }
}

}  // namespace rex::glue
