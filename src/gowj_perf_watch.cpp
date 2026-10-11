// gowj - ReXGlue Recompiled Project
//
// Host frame-cadence tracer.
//
// "Frame pacing feels awful" is a claim about the INTERVALS between frames, and
// nothing in the shipped build measures those: mnk_fps_meter counts the guest's
// right-stick polls (a proxy for the guest's own tick rate), and the runtime's
// own perf CSV (cvar perf_log_csv) produced no file across two attempts. So
// before changing anything, measure the two series that actually matter:
//
//   refresh - D3D12Presenter::RefreshGuestOutputImpl, called by the thread that
//             finished the emulated GPU command buffer. Its interval is how fast
//             the GUEST produces a frame (the fps the player sees), and its
//             spread is where a dip comes from.
//   present - D3D12Presenter::PaintAndPresentImpl, the host swap-chain commit.
//             Its interval is what the DISPLAY gets, and the time spent INSIDE it
//             is how long the call blocked on vertical sync - the number that
//             separates "guest is slow" from "pacing is wrong".
//
// Both are virtual overrides of rex::ui::Presenter, so they are reached through
// the class vtable, which rexruntime.dll exports as a data symbol
// (??_7D3D12Presenter@d3d12@ui@rex@@6B@). Patching one 8-byte slot is the same
// mechanism as the input guard's export-thunk rewrite, but needs no instruction
// relocation because a vtable entry is a pointer.
//
// Also read live here: rex::perf's counter registry (frame_time_us, draw_calls,
// command_buffer_stalls, texture/pipeline cache hits+misses, critical-region
// contentions). Those counter names exist in the release DLL, so the registry is
// compiled in and GetCounter is callable - which is what makes a dip attributable
// to a shader-compile storm vs. a GPU stall vs. a lock.
//
// GOWJ_PERF_WATCH=0 disables the whole thing (no slot is rewritten).
// GOWJ_PERF_CSV=<path> additionally writes one row per event for offline
// analysis, and asks rex::perf to write its own CSV beside it as
// <path>.runtime.csv, which settles whether that path works at all.
//
// Two interventions ride on top of that measurement. Both are shipping defaults
// and both are runtime cvars, so they can be changed from the settings overlay
// without a relaunch:
//
//   gowj_present_limit (bool, true)  - drop a swap commit when no new guest frame
//     exists. Measured on the same Act 1 route: guest 51.3 -> 61.4 fps, p99 71.5
//     -> 30.8 ms, and the refresh-quantum histogram went from 24 distinct buckets
//     to 15 with 66% of frames on the 4-refresh mode.
//   gowj_pace_hz (int, 60; 0 = off)  - release the guest render thread on a fixed
//     wall-clock cadence. Measured on the same route with both on: p50 lands on
//     16667us exactly and the 4-refresh (60 Hz) share goes 58% -> 74%. An earlier
//     version counted D3DKMT vertical-blank edges instead; that is what the
//     scanout-rate probe below is left over from, and it failed - the edge
//     interval is 4182 us on the desktop but ~11500 us in fullscreen, so a 4-edge
//     grid throttled Act 1 to 17.1 fps. The wall-clock version is why the probe is
//     now log-only.

#include "gowj_pgo.h"
#ifdef GOWJ_DEV_TOOLS
#include "gowj_profiler.h"
#endif
#include "gowj_perf_watch.h"
#include "gowj_fps_overlay.h"
#include "gowj_fps_unlock.h"
#include "gowj_quality.h"
#include "gowj_fsr.h"
#include "gowj_pso_library.h"
#include "gowj_system_tuning.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>

// Both interventions are shipping defaults now, and both are runtime cvars rather
// than env gates so the player can see and change them in the F4 settings list
// without relaunching. They are read per call, not latched at install, because
// InstallPerfWatch runs BEFORE the config overlay is applied (measured: the hook
// install logs at 04:48:06.555 and the authoritative "IN EFFECT" read-back at
// 04:48:07.609) - a value captured at install would be the SDK default whatever
// the toml says.

// The measured case for suppressing a redundant swap commit: the host committed
// 139-220 swaps/s while the emulated GPU produced 51-59 guest frames/s, each one a
// full 3840x2160 pass through the gamma ramp, the FXAA-extreme resolve and the
// dither. Same pixels on same glass, ~2.6 times per guest frame, from the one
// stage that scales with the panel rather than the game. On the same Act 1 route
// this took the guest from 51.3 to 61.4 fps and p99 from 71.5 to 30.8 ms.
REXCVAR_DEFINE_BOOL(gowj_present_limit, true, "perf",
                    "Skip a swap commit when no new guest frame exists. The runtime "
                    "otherwise re-commits the same image several times per guest "
                    "frame; each one costs a full-resolution present pass.");

// The measured case for the cadence grid: with the limiter alone the guest free
// ran at 61.4 fps (16.29 ms) against a 240 Hz panel, so its submission phase
// walked across the refresh boundaries and frame lifetimes alternated 3/4/5
// refreshes. That alternation is the "fps says 100+, feels like 30" complaint -
// the average is fine, the spacing is not. Locked to the grid, the same session
// measures p50 = 16667 us exactly with 74% of frames on the 4-refresh mode.
// 0 turns the grid off and lets the guest free-run again.
REXCVAR_DEFINE_INT32(gowj_pace_hz, 60, "perf",
                     "Guest frame release cadence in Hz (0 = free-run). A frame that "
                     "misses its slot is released immediately and skips it, so this "
                     "cannot collapse an over-budget scene onto half the rate.");

REXCVAR_DEFINE_INT32(gowj_pace_lead_us, 3000, "perf",
                     "How long before a scanout edge the pacer releases a guest frame "
                     "(0 = do not phase-lock the grid to scanout). Must cover the "
                     "present's own cost; too small and frames slip a refresh.");

REXCVAR_DEFINE_INT32(gowj_pace_refresh_hz, 0, "perf",
                     "Panel refresh used to phase-lock the pacer (0 = read the "
                     "primary display's current mode).");

namespace {

// rex::perf::CounterId, from tools/rexglue/include/rex/perf/counter.h. Only the
// ones that can explain a dip are read.
constexpr uint16_t kCtrFrameTimeUs = 0;
constexpr uint16_t kCtrFps = 1;
constexpr uint16_t kCtrDrawCalls = 2;
constexpr uint16_t kCtrCmdStalls = 3;
constexpr uint16_t kCtrCritContentions = 12;
constexpr uint16_t kCtrTexCacheHits = 13;
constexpr uint16_t kCtrTexCacheMisses = 14;
constexpr uint16_t kCtrPipeCacheHits = 15;
constexpr uint16_t kCtrPipeCacheMisses = 16;

constexpr size_t kVtableScanSlots = 96;
constexpr size_t kRing = 4096;  // power of two
constexpr size_t kWindow = 600;  // ~10 s at 60 fps, ~2.5 s at 240 Hz
constexpr uint64_t kReportEvery = 240;
// A frame that takes more than this to arrive is a hitch the player feels as a
// stutter, whatever the average says.
constexpr uint64_t kHitchUs = 50000;
// More than this inside the real call means the thread was blocked, most
// plausibly waiting for vertical sync.
constexpr uint64_t kBlockedUs = 8000;

using GetCounterFn = int64_t (*)(uint16_t id);
using SetCsvPathFn = void (*)(const std::string& path);

GetCounterFn g_get_counter = nullptr;

struct Cadence {
  const char* name;
  // Written by whichever thread calls the hook; a torn interval in a histogram
  // is not worth a lock on the present path.
  LARGE_INTEGER last{};
  bool have_last = false;
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> in_hook_sum_us{0};
  std::atomic<uint64_t> in_hook_max_us{0};
  std::atomic<uint64_t> blocked{0};
  std::atomic<uint64_t> hitches{0};
  std::atomic<uint32_t> tid{0};
  std::atomic<uint32_t> tid_switches{0};
  uint32_t ring[kRing]{};
  std::atomic<uint32_t> pos{0};
};

Cadence g_cad[2];
LARGE_INTEGER g_qpc_freq{};
std::atomic<uint64_t> g_guest_seq{0};
std::atomic<uint64_t> g_shown_seq{0};
std::atomic<uint64_t> g_skipped{0};
std::atomic<uint64_t> g_redundant{0};
LARGE_INTEGER g_last_present{};
// Repaint ceiling for a stale guest image, so UI that animates without the
// guest producing a frame (menu fades, toasts, a modal that pauses
// RefreshGuestOutput entirely) still moves at >=30 Hz.
constexpr uint64_t kUiFloorUs = 33333;

FILE* g_csv = nullptr;
const uint16_t kCtrIds[9] = {kCtrFrameTimeUs,  kCtrFps,         kCtrDrawCalls,
                             kCtrCmdStalls,   kCtrCritContentions, kCtrTexCacheHits,
                             kCtrTexCacheMisses, kCtrPipeCacheHits, kCtrPipeCacheMisses};

uint64_t QpcUs(LARGE_INTEGER a, LARGE_INTEGER b) {
  if (g_qpc_freq.QuadPart == 0) {
    return 0;
  }
  return static_cast<uint64_t>((b.QuadPart - a.QuadPart) * 1000000 /
                               g_qpc_freq.QuadPart);
}

uint64_t NowUs() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (g_qpc_freq.QuadPart == 0) {
    return 0;
  }
  return static_cast<uint64_t>(now.QuadPart * 1000000 / g_qpc_freq.QuadPart);
}

int64_t Counter(uint16_t id) {
  return g_get_counter ? g_get_counter(id) : 0;
}

void NoteEvent(Cadence& c, uint64_t interval_us, uint64_t in_hook_us) {
  const uint32_t tid = GetCurrentThreadId();
  uint32_t prev = c.tid.exchange(tid, std::memory_order_relaxed);
  if (prev && prev != tid) {
    c.tid_switches.fetch_add(1, std::memory_order_relaxed);
  }
  c.ring[c.pos.fetch_add(1, std::memory_order_relaxed) & (kRing - 1)] =
      static_cast<uint32_t>(std::min<uint64_t>(interval_us, 0xFFFFFFFFu));
  c.count.fetch_add(1, std::memory_order_relaxed);
  c.in_hook_sum_us.fetch_add(in_hook_us, std::memory_order_relaxed);
  uint64_t hm = c.in_hook_max_us.load(std::memory_order_relaxed);
  while (in_hook_us > hm && !c.in_hook_max_us.compare_exchange_weak(
                               hm, in_hook_us, std::memory_order_relaxed)) {
  }
  if (in_hook_us > kBlockedUs) {
    c.blocked.fetch_add(1, std::memory_order_relaxed);
  }
  if (interval_us > kHitchUs) {
    c.hitches.fetch_add(1, std::memory_order_relaxed);
  }
}

void WriteCsv(const char* kind, uint64_t interval_us, uint64_t in_hook_us) {
  if (!g_csv) {
    return;
  }
  // t_us,kind,interval_us,in_hook_us,tid, then the runtime's own counters.
  std::fprintf(g_csv,
               "%llu,%s,%llu,%llu,%u,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
               static_cast<unsigned long long>(NowUs()), kind,
               static_cast<unsigned long long>(interval_us),
               static_cast<unsigned long long>(in_hook_us), GetCurrentThreadId(),
               static_cast<long long>(Counter(kCtrFrameTimeUs)),
               static_cast<long long>(Counter(kCtrFps)),
               static_cast<long long>(Counter(kCtrDrawCalls)),
               static_cast<long long>(Counter(kCtrCmdStalls)),
               static_cast<long long>(Counter(kCtrCritContentions)),
               static_cast<long long>(Counter(kCtrTexCacheHits)),
               static_cast<long long>(Counter(kCtrTexCacheMisses)),
               static_cast<long long>(Counter(kCtrPipeCacheHits)),
               static_cast<long long>(Counter(kCtrPipeCacheMisses)));
  // The 256 KB buffer is ~2200 rows, so a run that dies or is closed early leaves
  // an empty file - which is exactly how pw_grid2.csv read as "no data" and
  // briefly looked like the pacer had produced nothing. Flush once a second at
  // panel rate, and immediately on a hitch, which is the row worth recovering.
  static std::atomic<uint64_t> rows{0};
  const uint64_t n = rows.fetch_add(1, std::memory_order_relaxed);
  if (interval_us >= kHitchUs || (n & 255) == 0) {
    std::fflush(g_csv);
  }
}

void Report(Cadence& c) {
  const uint64_t n = c.count.load(std::memory_order_relaxed);
  if (n < kReportEvery) {
    return;
  }
  const uint32_t pos = c.pos.load(std::memory_order_relaxed) & (kRing - 1);
  const size_t take = std::min<uint64_t>(kWindow, n);
  std::vector<uint32_t> v;
  v.reserve(take);
  for (size_t i = 0; i < take; ++i) {
    v.push_back(c.ring[(pos - 1 - i + kRing) & (kRing - 1)]);
  }
  std::vector<uint32_t> s = v;
  std::sort(s.begin(), s.end());
  auto pct = [&](double p) {
    return s[static_cast<size_t>(p * (s.size() - 1) + 0.5)];
  };
  double sum = 0;
  size_t over_1_5x = 0;
  const uint32_t med = pct(0.5);
  for (uint32_t x : v) {
    sum += x;
    if (med && x > med * 3 / 2) {
      ++over_1_5x;
    }
  }
  const double mean = sum / double(v.size());
  const uint64_t window_us = static_cast<uint64_t>(sum);
  const double inst_fps = window_us ? double(v.size()) * 1000000.0 / double(window_us) : 0.0;

  int64_t ctr_now[9];
  for (size_t i = 0; i < 9; ++i) {
    ctr_now[i] = Counter(kCtrIds[i]);
  }

  REXLOG_WARN(
      "PERFWATCH {} n={} fps={:.1f} interval_us p50={} p90={} p99={} max={:.0f} "
      "mean={:.0f} jitter_gt_1_5x_p50={}/{} hitches_total={} tid={:X} tid_switches={} "
      "in_hook_us avg={:.1f} max={} blocked_total={} skipped={} redundant={} | ctr frame_us={} fps={} draws={} "
      "stalls={} crit={} tex_hit_miss={}/{} pipe_hit_miss={}/{}",
      c.name, n, inst_fps, pct(0.5), pct(0.9), pct(0.99), double(s.back()), mean,
      over_1_5x, v.size(), c.hitches.load(std::memory_order_relaxed),
      c.tid.load(std::memory_order_relaxed),
      c.tid_switches.load(std::memory_order_relaxed),
      // Report runs on an exact kReportEvery boundary, so exchanging the
      // accumulators makes these two the average and worst of THIS window.
      c.in_hook_sum_us.exchange(0, std::memory_order_relaxed) / double(kReportEvery),
      c.in_hook_max_us.exchange(0, std::memory_order_relaxed),
      c.blocked.load(std::memory_order_relaxed),
      (&c == &g_cad[0]) ? g_skipped.load(std::memory_order_relaxed) : 0ULL,
      (&c == &g_cad[0]) ? g_redundant.load(std::memory_order_relaxed) : 0ULL,
      ctr_now[0], ctr_now[1], ctr_now[2],
      ctr_now[3], ctr_now[4], ctr_now[5], ctr_now[6], ctr_now[7], ctr_now[8]);
}

// Per-event body shared by both hooks. in_hook_us is the time spent inside the
// runtime's real call - the vsync wait.
void OnEvent(size_t kind, uint64_t in_hook_us) {
  Cadence& c = g_cad[kind];
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const uint64_t interval_us = c.have_last ? QpcUs(c.last, now) : 0;
  c.last = now;
  c.have_last = true;
  rex::glue::FpsOverlayNote(kind, interval_us);
  if (kind == 1) rex::glue::QualityNoteFrame(interval_us);
#ifdef GOWJ_DEV_TOOLS
  rex::glue::FrameLogNote(kind, interval_us);
#endif
  NoteEvent(c, interval_us, in_hook_us);
  WriteCsv(c.name, interval_us, in_hook_us);
  if (c.count.load(std::memory_order_relaxed) % kReportEvery == 0) {
    Report(c);
  }
  if (interval_us > kHitchUs) {
    static std::atomic<int> hitch_budget{5000};  // every hitch, for attribution
    if (hitch_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_WARN("PERFWATCH HITCH {} gap={:.1f}ms (p50-class frame is 16.7ms) tid={:X}",
                  c.name, interval_us / 1000.0, GetCurrentThreadId());
    }
  }
}

// uint32_t D3D12Presenter::PaintAndPresentImpl(bool execute_ui_drawers)
using PresentFn = uint32_t (*)(void* self, bool execute_ui_drawers);
PresentFn g_present_orig = nullptr;

// Redundant-present suppression; gowj_present_limit is the runtime switch.
//
// Measured in the baseline run: the host committed 139-220 swaps/s while the
// emulated GPU produced 51-59 guest frames/s, each present a full 3840x2160 pass
// through the gamma ramp, the FXAA-extreme resolve and the dither. So ~2.6 of
// those passes per guest frame put identical pixels on identical glass - pure
// waste from the one stage that scales with the panel rather than the guest.
// Skipping them hands that GPU time back to the guest renderer, which is what a
// dense-location dip actually is.
//
// The gate is NOT the caller's execute_ui_drawers flag. Measured on the first
// limited run: that argument was true on 100% of ~44 000 presents sampled, so
// gating on it skipped exactly nothing. The runtime composites its UI draw pass
// on every tick regardless of whether anything is overlaid (and with the fps
// overlay the user is reading, something always is). "The UI layer exists" is
// therefore not evidence that new pixels exist - only the guest frame counter
// is, which is what g_guest_seq/g_shown_seq compare.

// Scanout phase estimate. A vsynced fresh present returns just after an edge, so
// where completions land modulo the refresh period is the panel's phase. Kept as a
// decaying unit-vector sum (circular mean): its length says how trustworthy the
// phase is, and PaceToGrid refuses to steer unless that length is high. Written by
// the UI thread only, read by the guest render thread; a torn pair of relaxed
// loads only costs one frame of a slow loop.
std::atomic<double> g_phase_cos{0.0};
std::atomic<double> g_phase_sin{0.0};
std::atomic<uint64_t> g_phase_n{0};
std::atomic<uint64_t> g_edge_us{0};

uint64_t RefreshPeriodUs() {
  int32_t hz = REXCVAR_GET(gowj_pace_refresh_hz);
  if (hz <= 0) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    hz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)
             ? static_cast<int32_t>(dm.dmDisplayFrequency)
             : 0;
  }
  return hz >= 24 && hz <= 1000 ? 1000000ULL / uint32_t(hz) : 0;
}

void NoteScanoutPhase(uint64_t done_us) {
  uint64_t period = g_edge_us.load(std::memory_order_relaxed);
  if (period == 0 || (g_phase_n.load(std::memory_order_relaxed) & 1023) == 0) {
    period = RefreshPeriodUs();
    g_edge_us.store(period, std::memory_order_relaxed);
  }
  if (period == 0) {
    return;
  }
  const double theta = 6.283185307179586 * double(done_us % period) / double(period);
  constexpr double kDecay = 0.985;
  g_phase_cos.store(g_phase_cos.load(std::memory_order_relaxed) * kDecay + std::cos(theta),
                    std::memory_order_relaxed);
  g_phase_sin.store(g_phase_sin.load(std::memory_order_relaxed) * kDecay + std::sin(theta),
                    std::memory_order_relaxed);
  g_phase_n.fetch_add(1, std::memory_order_relaxed);
}

// Programmatic PIX capture: with GOWJ_PIX loaded, creating the file named by
// GOWJ_PIX_TRIGGER makes the next frame be captured to "<trigger>.wpix".
void PollPixTrigger() {
  static int countdown = 0;
  if (++countdown < 30) return;
  countdown = 0;
  char trig[MAX_PATH]{};
  if (!GetEnvironmentVariableA("GOWJ_PIX_TRIGGER", trig, sizeof(trig))) return;
  if (GetFileAttributesA(trig) == INVALID_FILE_ATTRIBUTES) return;
  DeleteFileA(trig);
  using CaptureFn = HRESULT(WINAPI*)(PCWSTR, UINT32);
  HMODULE cap = GetModuleHandleA("WinPixGpuCapturer.dll");
  auto fn = cap ? reinterpret_cast<CaptureFn>(GetProcAddress(cap, "CaptureNextFrame"))
                : nullptr;
  wchar_t out[MAX_PATH + 8]{};
  MultiByteToWideChar(CP_UTF8, 0, trig, -1, out, MAX_PATH);
  wcscat_s(out, L".wpix");
  const HRESULT hr = fn ? fn(out, 1) : E_NOINTERFACE;
  REXLOG_WARN("PIX programmatic capture requested -> hr={:08x}", static_cast<uint32_t>(hr));
}

uint32_t HookPresent(void* self, bool execute_ui_drawers) {
  PollPixTrigger();
  LARGE_INTEGER owned;
  QueryPerformanceCounter(&owned);
  const bool fresh = g_guest_seq.load(std::memory_order_relaxed) !=
                     g_shown_seq.load(std::memory_order_relaxed);
  if (!fresh) {
    // Counted even when limiting is off: this is the waste itself, and having it
    // on the record in a baseline run is what sizes the fix.
    g_redundant.fetch_add(1, std::memory_order_relaxed);
    if (REXCVAR_GET(gowj_present_limit) &&
        QpcUs(g_last_present, owned) < kUiFloorUs) {
      g_skipped.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<int> note{3};
      if (g_skipped.load(std::memory_order_relaxed) % 2048 == 0 &&
          note.fetch_sub(1, std::memory_order_relaxed) > 0) {
        REXLOG_INFO("PERFWATCH present limit active, {} redundant presents skipped "
                    "of {} total re-commits",
                    g_skipped.load(std::memory_order_relaxed),
                    g_redundant.load(std::memory_order_relaxed));
      }
      return 0;  // PaintResult::kPresented - nothing changed, nothing to show
    }
  }
  LARGE_INTEGER done;
  const uint32_t result = g_present_orig(self, execute_ui_drawers);
  QueryPerformanceCounter(&done);
  g_shown_seq.store(g_guest_seq.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
  g_last_present = done;
  if (fresh) {
    NoteScanoutPhase(NowUs());
  }
  OnEvent(0, QpcUs(owned, done));
  return result;
}

// bool D3D12Presenter::RefreshGuestOutputImpl(uint32_t mailbox_index,
//                                             uint32_t frontbuffer_width,
//                                             uint32_t frontbuffer_height,
//                                             const std::function<bool(GuestOutputRefreshContext&)>& refresher,
//                                             bool& is_8bpc_out_ref)
using RefreshFn = bool (*)(void* self, uint32_t mailbox_index, uint32_t frontbuffer_w,
                           uint32_t frontbuffer_h, void* refresher, bool* is_8bpc_out);
RefreshFn g_refresh_orig = nullptr;

// ---------------------------------------------------------------------------
// Integer-refresh pacing. gowj_pace_hz is the runtime switch (0 = free-run).
//
// With the redundant presents removed, the measured frame-lifetime histogram on
// the 3840x2160@240 panel went from 24 buckets to 15 and from 42% to 66% on the
// 4-refresh (60 Hz) mode - but 34% of frames still landed on 2, 3, 5 or 6
// refreshes. That residue is not load, it is PHASE: the guest's own render loop
// free-runs at 61.4 fps (16.29 ms/frame) against a 60 Hz grid (16.667 ms), so its
// submission time walks slowly through the refresh boundaries and each frame
// rounds to whichever side it happens to fall on. Alternating 3- and 4-refresh
// displays is a 12.5/16.7 ms cadence, and that is the "feels like 30" the player
// reports at a measured 60 fps: the average is fine, the spacing is not.
//
// The fix is to close the loop between the guest and the presenter: release the
// guest's frame on a stable cadence that is an exact multiple of the scanout
// quantum, so every frame arrives in the same phase and a genuine over-budget
// frame becomes one clean 2-slot display instead of a 3/4/5 wobble. That is the
// difference between "dropped frames" and "bad pacing", and only the second one
// reads as judder. PaceToGrid below explains why the cadence is wall-clock rather
// than counted vertical blanks.
//
// Deliberately on the REFRESH side (the guest's render thread, tid measured
// 3830) and not the present side (the UI thread, 32412): sleeping the guest
// thread is what a native 60 Hz-capped title does, and it feeds back - the guest
// stops queueing GPU work while it waits. The UI thread stays at panel rate so
// menus and the overlay keep their own responsiveness.
//
// Safety: the wait is bounded by the cadence itself (at most one interval, and
// zero for a frame that already missed its slot), and it is a waitable timer plus
// a spin rather than a blocking kernel wait on a display event - so an adaptive
// or dead scanout cannot hang the guest thread. The D3DKMT calls left in this
// file measure what the scanout actually costs and gate nothing.

// Layouts taken from Windows Kits\10\Include\10.0.26100.0\shared\d3dkmthk.h, not
// from memory: the first attempt here passed {void*,void*} for
// D3DKMT_WAITFORVERTICALBLANKEVENT and got STATUS_INVALID_PARAMETER, because the
// real struct is three 4-byte fields (hAdapter, hDevice, VidPnSourceId) and
// hAdapter=0 is not "the primary adapter", it is no adapter at all. The static
// asserts are what keep this honest if the SDK ever changes under us.
struct D3DKMTAdapterInfo {
  uint32_t adapter;
  int32_t luid_high;
  uint32_t luid_low;
  uint32_t num_sources;
  uint32_t precise_present_regions;
};
struct D3DKMTEnumAdapters {
  uint32_t num_adapters;
  D3DKMTAdapterInfo adapters[16];  // MAX_ENUM_ADAPTERS
};
struct D3DKMTWaitVBlank {
  uint32_t adapter;
  uint32_t device;  // optional, 0
  uint32_t source;  // VidPnSourceId
};
static_assert(sizeof(D3DKMTAdapterInfo) == 20, "D3DKMT_ADAPTERINFO layout");
static_assert(sizeof(D3DKMTEnumAdapters) == 4 + 16 * 20, "D3DKMT_ENUMADAPTERS layout");
static_assert(sizeof(D3DKMTWaitVBlank) == 12, "D3DKMT_WAITFORVERTICALBLANKEVENT layout");

using WaitVBlankFn = LONG (*)(const D3DKMTWaitVBlank*);
using EnumAdaptersFn = LONG (*)(D3DKMTEnumAdapters*);

WaitVBlankFn g_wait_vblank = nullptr;
EnumAdaptersFn g_enum_adapters = nullptr;
uint32_t g_pace_adapter = 0;
uint32_t g_pace_source = 0;
uint32_t g_pace_period_us = 0;
bool g_pace_on = false;
uint64_t g_pace_interval_us = 16667;
HANDLE g_pace_timer = nullptr;
uint64_t g_pace_anchor = 0;  // grid origin; only the owning thread touches these
uint64_t g_pace_due = 0;
std::atomic<uint64_t> g_pace_owner{0};
std::atomic<uint64_t> g_pace_frames{0};
std::atomic<uint64_t> g_pace_wait_us{0};
std::atomic<uint64_t> g_pace_wait_max_us{0};
std::atomic<uint64_t> g_pace_release_slip_max{0};
std::atomic<uint64_t> g_pace_contend{0};  // entered off the owning thread
std::atomic<uint64_t> g_pace_missed{0};   // frames already past their deadline
std::atomic<uint64_t> g_pace_locked{0};   // slots steered onto scanout phase

LONG WaitOneEdge(uint32_t adapter, uint32_t source) {
  const D3DKMTWaitVBlank args{adapter, 0, source};
  return g_wait_vblank(&args);
}

// Release the guest on a fixed wall-clock cadence instead of counting refreshes.
//
// Edge counting was tried first and measured worse than no pacing at all: with
// grid=4 on the feed this code picks, Act 1 ran at 17.1 fps with a 58 ms median
// frame, because during fullscreen the D3DKMT vblank signals every ~9 ms rather
// than the 4.18 ms it reports on the desktop. Whatever the panel is doing, it is
// not a stable grid to divide by.
//
// A QPC cadence does not need the display at all. The runtime's own presenter is
// vsynced - the title screen's present interval measures 8324 us = exactly two
// 4.17 ms refreshes - so it already quantises to the scanout. Handing it a frame
// that is *late* (guest render finished mid-quantum) is what produces the 3/4/5
// refresh lifetimes the player feels as judder. Releasing on a stable 16.667 ms
// grid, an exact multiple of that quantum, keeps every frame in the same phase.
//
// No catch-up spiral and no throughput tax: a frame that has already passed its
// slot is released immediately and simply skips that slot, so an over-budget dense
// scene keeps free-running at its real ~55 fps instead of collapsing onto every
// second grid line at 30.
void PaceToGrid() {
  if (!g_pace_on) {
    return;
  }
  const uint32_t tid = GetCurrentThreadId();
  const uint32_t owner = g_pace_owner.load(std::memory_order_relaxed);
  if (owner != 0 && owner != tid) {
    g_pace_contend.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_pace_owner.store(tid, std::memory_order_relaxed);

  // Read per call, and re-anchor whenever the cadence changes: g_pace_anchor and
  // g_pace_due belong to this thread alone, so this is also the only place the
  // cvar is allowed to touch them.
  const int32_t hz = REXCVAR_GET(gowj_pace_hz);
  if (hz <= 0) {
    g_pace_anchor = 0;
    return;
  }
  if (const uint64_t want = (1000000UL + uint32_t(hz) / 2) / uint32_t(hz);
      want != g_pace_interval_us) {
    g_pace_interval_us = want;
    g_pace_anchor = 0;
  }

  const uint64_t now = NowUs();
  if (g_pace_anchor == 0) {
    g_pace_anchor = now;
    g_pace_due = now + g_pace_interval_us;
  } else {
    g_pace_due += g_pace_interval_us;
  }
  // Phase lock: pull each slot onto (scanout edge - lead) so the present lands
  // just before a blank rather than just after one. Only steers once the phase
  // estimate is coherent; an adaptive or unknown scanout leaves the plain grid.
  const uint64_t period = g_edge_us.load(std::memory_order_relaxed);
  const int32_t lead = REXCVAR_GET(gowj_pace_lead_us);
  bool locked = false;
  if (period && lead > 0 && uint64_t(lead) < period * 2 &&
      g_phase_n.load(std::memory_order_relaxed) >= 128) {
    const double c = g_phase_cos.load(std::memory_order_relaxed);
    const double s = g_phase_sin.load(std::memory_order_relaxed);
    const double coherence = std::sqrt(c * c + s * s) * (1.0 - 0.985);
    if (coherence > 0.6) {
      double frac = std::atan2(s, c) / 6.283185307179586;
      if (frac < 0) frac += 1.0;
      const uint64_t edge = uint64_t(frac * double(period));
      const uint64_t want = (edge + period * 4 - uint64_t(lead) % period) % period;
      const uint64_t have = g_pace_due % period;
      int64_t delta = int64_t(want) - int64_t(have);
      if (delta > int64_t(period / 2)) delta -= int64_t(period);
      if (delta < -int64_t(period / 2)) delta += int64_t(period);
      g_pace_due = uint64_t(int64_t(g_pace_due) + delta);
      locked = true;
      g_pace_locked.fetch_add(1, std::memory_order_relaxed);
    }
  }
  (void)locked;
  if (now >= g_pace_due) {
    // This frame already missed the slot it was aiming at: releasing it now costs
    // no latency and keeps the real frame rate, while quietly moving the aim point
    // back ahead of the clock. Holding anyway would put a slightly-over-budget
    // scene on every second grid line - a 30 fps collapse out of a 55 fps game.
    g_pace_missed.fetch_add(1, std::memory_order_relaxed);
    const uint64_t slots = (now - g_pace_anchor) / g_pace_interval_us + 1;
    g_pace_due = g_pace_anchor + slots * g_pace_interval_us;
    if (g_pace_due <= now) {
      g_pace_due = now + g_pace_interval_us;  // clock jumped backwards
    }
    return;
  }
  const uint64_t target = g_pace_due;

  LARGE_INTEGER start;
  QueryPerformanceCounter(&start);
  uint64_t remaining = target - now;
  // Sleep to 1.5 ms short of the slot, then spin. A bare waitable timer lands
  // ~1-2 ms late, which is enough to cross an 8.3 ms present quantum and put back
  // the very jitter this removes; ~1.5 ms of spin on the render thread is the
  // price of sub-100 us release accuracy.
  constexpr uint64_t kSpinUs = 1500;
  while (remaining > kSpinUs) {
    const uint64_t wait_us = remaining - kSpinUs;
    LARGE_INTEGER rel;
    rel.QuadPart = -static_cast<LONGLONG>(wait_us * 10);  // 100 ns units, relative
    if (!g_pace_timer ||
        !SetWaitableTimer(g_pace_timer, &rel, 0, nullptr, nullptr, FALSE)) {
      break;  // no timer: fall through to the spin, which is correct but hot
    }
    WaitForSingleObject(g_pace_timer, 10);
    remaining = target - NowUs();
  }
  while (NowUs() < target) {
  }
  LARGE_INTEGER done;
  QueryPerformanceCounter(&done);

  const uint64_t waited = QpcUs(start, done);
  g_pace_wait_us.fetch_add(waited, std::memory_order_relaxed);
  g_pace_frames.fetch_add(1, std::memory_order_relaxed);
  uint64_t prev_max = g_pace_wait_max_us.load(std::memory_order_relaxed);
  while (waited > prev_max &&
         !g_pace_wait_max_us.compare_exchange_weak(prev_max, waited,
                                                   std::memory_order_relaxed)) {
  }
  const uint64_t slip = NowUs() - target;
  prev_max = g_pace_release_slip_max.load(std::memory_order_relaxed);
  while (slip > prev_max &&
         !g_pace_release_slip_max.compare_exchange_weak(prev_max, slip,
                                                        std::memory_order_relaxed)) {
  }
  if (g_pace_frames.load(std::memory_order_relaxed) % kReportEvery == 0) {
    REXLOG_WARN(
        "PERFWATCH PACE interval={}us frames={} waited_us avg={:.0f} max={} "
        "slip_max={} hold_missed={} contend={} locked={} edge={}us tid={:X}",
        g_pace_interval_us, g_pace_frames.load(std::memory_order_relaxed),
        g_pace_wait_us.exchange(0, std::memory_order_relaxed) / double(kReportEvery),
        g_pace_wait_max_us.exchange(0, std::memory_order_relaxed),
        g_pace_release_slip_max.exchange(0, std::memory_order_relaxed),
        g_pace_missed.load(std::memory_order_relaxed),
        g_pace_contend.load(std::memory_order_relaxed),
        g_pace_locked.load(std::memory_order_relaxed),
        g_edge_us.load(std::memory_order_relaxed), tid);
  }
}

bool HookRefresh(void* self, uint32_t mailbox_index, uint32_t frontbuffer_w,
                 uint32_t frontbuffer_h, void* refresher, bool* is_8bpc_out) {
  LARGE_INTEGER owned, done;
  QueryPerformanceCounter(&owned);
  // Wrap the refresher so FSR (gowj_fsr.cpp) learns which texture holds this
  // frame's guest output; the size arguments are the scaled output size.
  using Refresher = std::function<bool(rex::ui::Presenter::GuestOutputRefreshContext&)>;
  const Refresher& inner = *static_cast<const Refresher*>(refresher);
  const Refresher wrapped = [&inner, frontbuffer_w,
                             frontbuffer_h](rex::ui::Presenter::GuestOutputRefreshContext& c) {
    const bool ok = inner(c);
    if (ok) {
      GowjFsrNoteGuestOutput(
          static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(c)
              .resource_uav_capable(),
          frontbuffer_w, frontbuffer_h);
    }
    return ok;
  };
  const bool result =
      g_refresh_orig(self, mailbox_index, frontbuffer_w, frontbuffer_h,
                     const_cast<Refresher*>(&wrapped), is_8bpc_out);
  QueryPerformanceCounter(&done);
  if (result) {
    // Only a true return means new guest pixels exist to show.
    g_guest_seq.fetch_add(1, std::memory_order_release);
    PaceToGrid();
  }
  OnEvent(1, QpcUs(owned, done));
  return result;
}

struct ProbeRequest {
  uint32_t adapter;
  uint32_t source;
  LONG status;  // out: first wait's NTSTATUS
  uint32_t period_us;  // out: 0 unless two edges were measured
};

// Two edges, timed. A source that never signals is indistinguishable from a hang,
// which is why this runs on a throwaway thread with a deadline rather than inline.
// The request is heap-allocated and the thread never frees it: on timeout the
// caller must be able to walk away while the parked wait may still write back.
DWORD WINAPI VBlankProbeEntry(LPVOID p) {
  auto* r = reinterpret_cast<ProbeRequest*>(p);
  LARGE_INTEGER ta{}, tb{};
  r->status = WaitOneEdge(r->adapter, r->source);
  QueryPerformanceCounter(&ta);
  if (r->status == 0 && WaitOneEdge(r->adapter, r->source) == 0) {
    QueryPerformanceCounter(&tb);
    r->period_us = static_cast<uint32_t>(QpcUs(ta, tb));
  }
  return 0;
}

// Probe one (adapter, source) pair under a hard deadline. Returns the measured
// refresh period in us, or 0 for "no signal" (call failed, or never returned).
uint32_t ProbePair(uint32_t adapter, uint32_t source, uint32_t timeout_ms) {
  auto* r = new ProbeRequest{adapter, source, static_cast<LONG>(0xDEAD'BEEF), 0};
  HANDLE th = CreateThread(nullptr, 0, &VBlankProbeEntry, r, 0, nullptr);
  if (!th) {
    delete r;
    return 0;
  }
  const bool finished = WaitForSingleObject(th, timeout_ms) == WAIT_OBJECT_0;
  CloseHandle(th);
  if (!finished) {
    // The thread may stay parked in the kernel wait forever, holding its request.
    // One leaked thread in a session that has already lost its vsync source is the
    // cheaper failure - but the pair is unusable, so report it as no signal.
    REXLOG_INFO("PERFWATCH vblank probe adapter={} source={} TIMED OUT after {}ms",
                adapter, source, timeout_ms);
    return 0;
  }
  const LONG status = r->status;
  const uint32_t period = r->period_us;
  delete r;
  if (status != 0) {
    REXLOG_INFO("PERFWATCH vblank probe adapter={} source={} status={:#x}", adapter,
                source, static_cast<unsigned>(status));
    return 0;
  }
  return period;
}

// On a machine with two GPUs the adapter that
// actually scans out to the 240 Hz panel is not guaranteed to be index 0 - the
// DXGI choice has been measured to vary between boots. So every candidate is
// measured and logged, and the fastest signalling one wins: on a 240 Hz panel the
// 4.17 ms quantum is what makes an integer-refresh grid possible at all, and a
// 60 Hz head would make grid=4 a hard 15 fps. Returns the chosen feed's period in
// us, or 0 when nothing signalled.
uint32_t SelectVBlankFeed() {
  D3DKMTEnumAdapters ea{};
  uint32_t adapters = 0;
  if (g_enum_adapters && g_enum_adapters(&ea) == 0 && ea.num_adapters > 0 &&
      ea.num_adapters <= 16) {
    adapters = ea.num_adapters;
  } else {
    REXLOG_INFO("PERFWATCH D3DKMTEnumAdapters unavailable - probing adapter 0 only");
    ea.num_adapters = 1;
    ea.adapters[0].adapter = 0;
    ea.adapters[0].num_sources = 1;
    adapters = 1;
  }

  uint32_t best_period = 0;
  REXLOG_INFO("PERFWATCH probing {} adapter(s) for a live vblank feed", adapters);
  for (uint32_t a = 0; a < adapters; ++a) {
    const uint32_t adapter = ea.adapters[a].adapter;
    uint32_t sources = ea.adapters[a].num_sources;
    if (sources == 0) {
      sources = 1;
    } else if (sources > 8) {
      sources = 8;
    }
    uint32_t misses = 0;
    for (uint32_t s = 0; s < sources; ++s, ++misses) {
      const uint32_t period = ProbePair(adapter, s, 1000);
      if (period) {
        REXLOG_INFO("PERFWATCH vblank adapter={} (luid {:#x}:{:#x}) source={} -> "
                    "{}us ({:.1f} Hz)",
                    adapter, static_cast<unsigned>(ea.adapters[a].luid_high),
                    static_cast<unsigned>(ea.adapters[a].luid_low), s, period,
                    1e6 / double(period));
      } else {
        REXLOG_INFO("PERFWATCH vblank adapter={} (luid {:#x}:{:#x}) source={} -> no "
                    "signal",
                    adapter, static_cast<unsigned>(ea.adapters[a].luid_high),
                    static_cast<unsigned>(ea.adapters[a].luid_low), s);
      }
      if (period == 0) {
        if (misses >= 1) {
          break;  // two sources in a row never signalled - stop walking this adapter
        }
        continue;
      }
      misses = 0;
      if (best_period == 0 || period < best_period) {
        best_period = period;
        g_pace_adapter = adapter;
        g_pace_source = s;
      }
    }
  }
  if (best_period) {
    REXLOG_WARN("PERFWATCH vblank feed chosen: adapter={} source={} {}us ({:.1f} Hz)",
                g_pace_adapter, g_pace_source, best_period, 1e6 / double(best_period));
  }
  return best_period;
}

bool ProbeVBlank() {
  if (!g_wait_vblank) {
    return false;
  }
  g_pace_period_us = SelectVBlankFeed();
  return g_pace_period_us != 0;
}

bool WriteSlot(void** slot, void* value) {  DWORD old = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
    return false;
  }
  *slot = value;
  DWORD tmp = 0;
  VirtualProtect(slot, sizeof(void*), old, &tmp);
  FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
  return true;
}

// Find `target` among the vtable's first slots and point it at `replacement`.
// Searching instead of hard-coding an index means a runtime rebuild that reorders
// the class shows up as "not found" rather than as a wrong-function hook.
bool HookVtable(void** vtable, void* target, void* replacement, const char* what) {
  uintptr_t base = reinterpret_cast<uintptr_t>(vtable);
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<void*>(base), &mbi, sizeof(mbi)) ||
      mbi.State != MEM_COMMIT) {
    REXLOG_WARN("PERFWATCH {} vtable {:#x} unreadable", what, base);
    return false;
  }
  for (size_t i = 0; i < kVtableScanSlots; ++i) {
    void* slot = nullptr;
    if (reinterpret_cast<uintptr_t>(vtable + i + 1) >
        reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
      break;
    }
    slot = vtable[i];
    if (slot == target) {
      if (!WriteSlot(&vtable[i], replacement)) {
        REXLOG_WARN("PERFWATCH {} slot[{}] unprotect failed (gle={})", what, i,
                    GetLastError());
        return false;
      }
      REXLOG_INFO("PERFWATCH {} hooked vtable[{}] {:#x} -> wrapper {:#x}", what, i,
                  reinterpret_cast<uintptr_t>(target),
                  reinterpret_cast<uintptr_t>(replacement));
      return true;
    }
  }
  REXLOG_WARN("PERFWATCH {} target {:#x} not in vtable[0..{}] - slot moved or class "
              "subclassed elsewhere",
              what, reinterpret_cast<uintptr_t>(target), kVtableScanSlots);
  return false;
}

}  // namespace

namespace rex::glue {

void InstallPerfWatch() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  // GOWJ_PIX=<path to WinPixGpuCapturer.dll>: load the PIX GPU capturer before any
  // D3D12 device exists (ours below included), so `pixtool attach` can capture.
  if (char pix[MAX_PATH]{}; GetEnvironmentVariableA("GOWJ_PIX", pix, sizeof(pix))) {
    REXLOG_WARN("PIX GPU capturer {}: {}", pix, LoadLibraryA(pix) ? "loaded" : "FAILED");
  }
  InstallFsr();
  InstallPsoLibrary();
  InstallFpsOverlay();
  InstallFpsUnlock();
  InstallSystemTuning();
  InstallPgoFlusher();
#ifdef GOWJ_DEV_TOOLS
  InstallProfiler();
#endif

  char gate[8]{};
  if (GetEnvironmentVariableA("GOWJ_PERF_WATCH", gate, sizeof(gate)) &&
      std::strcmp(gate, "0") == 0) {
    REXLOG_INFO("PERFWATCH disabled via GOWJ_PERF_WATCH=0");
    return;
  }
  if (!QueryPerformanceFrequency(&g_qpc_freq) || g_qpc_freq.QuadPart == 0) {
    g_qpc_freq.QuadPart = 0;
    REXLOG_WARN("PERFWATCH no performance counter available");
  }
  g_cad[0].name = "present";
  g_cad[1].name = "refresh";

  REXLOG_WARN("PERFWATCH redundant-present suppression {} (a 33ms floor still "
              "repaints UI that animates without the guest); set gowj_present_limit "
              "to change it at runtime",
              REXCVAR_GET(gowj_present_limit) ? "ON" : "OFF");

  if (HMODULE gdi = GetModuleHandleA("gdi32.dll")) {
    g_wait_vblank = reinterpret_cast<WaitVBlankFn>(
        GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"));
    g_enum_adapters = reinterpret_cast<EnumAdaptersFn>(
        GetProcAddress(gdi, "D3DKMTEnumAdapters"));
  }
  // Log-only: what the scanout actually costs right now. It is NOT the timebase,
  // because the value changes between the desktop (4182 us) and fullscreen
  // (~11500 us) on the test machine - which is why the grid is a QPC cadence.
  ProbeVBlank();
  g_pace_timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (!g_pace_timer) {
    g_pace_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  }
  g_pace_on = true;  // gowj_pace_hz=0 still turns it off, per call
  REXLOG_WARN("PERFWATCH cadence pacing available at {}Hz: guest frame released "
              "every {}us, high-res timer {}, scanout reads {}us",
              REXCVAR_GET(gowj_pace_hz), g_pace_interval_us,
              g_pace_timer ? "ok" : "ABSENT (spin only)", g_pace_period_us);

  HMODULE mod = GetModuleHandleA("rexruntime.dll");
  if (!mod) {
    REXLOG_WARN("PERFWATCH rexruntime.dll not loaded");
    return;
  }

  g_get_counter = reinterpret_cast<GetCounterFn>(
      GetProcAddress(mod, "?GetCounter@perf@rex@@YA_JW4CounterId@12@@Z"));
  REXLOG_INFO("PERFWATCH perf counters {}", g_get_counter ? "readable" : "NOT exported");

  char csv[MAX_PATH * 2]{};
  if (GetEnvironmentVariableA("GOWJ_PERF_CSV", csv, sizeof(csv)) && csv[0]) {
    g_csv = std::fopen(csv, "wb");
    if (g_csv) {
      std::setvbuf(g_csv, nullptr, _IOFBF, 1 << 18);
      std::fprintf(g_csv,
                   "t_us,kind,interval_us,in_hook_us,tid,frame_time_us,fps,draw_calls,"
                   "cmd_stalls,crit_contentions,tex_hits,tex_misses,pipe_hits,pipe_misses\n");
      REXLOG_INFO("PERFWATCH per-frame CSV -> {}", csv);
    } else {
      REXLOG_WARN("PERFWATCH could not open CSV '{}' (errno={})", csv, errno);
    }
    // Settle whether rex::perf's own CSV writer works when the path is handed to
    // it directly instead of through the perf_log_csv cvar, which produced no
    // file in two attempts.
    if (auto* set_path = reinterpret_cast<SetCsvPathFn>(
            GetProcAddress(mod, "?SetCsvLogPath@perf@rex@@YAXAEBV?$basic_string@"
                                   "DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"))) {
      std::string runtime_csv = std::string(csv) + ".runtime.csv";
      set_path(runtime_csv);
      REXLOG_INFO("PERFWATCH asked rex::perf for {}", runtime_csv);
    }
  }

  auto* vtable = reinterpret_cast<void**>(
      GetProcAddress(mod, "??_7D3D12Presenter@d3d12@ui@rex@@6B@"));
  auto* present =
      GetProcAddress(mod, "?PaintAndPresentImpl@D3D12Presenter@d3d12@ui@rex@@MEAA?"
                          "AW4PaintResult@Presenter@34@_N@Z");
  auto* refresh =
      GetProcAddress(mod, "?RefreshGuestOutputImpl@D3D12Presenter@d3d12@ui@rex@@MEAA_"
                          "NIIIV?$function@$$A6A_NAEAVGuestOutputRefreshContext@"
                          "Presenter@ui@rex@@@Z@std@@AEA_N@Z");
  if (!vtable || !present) {
    REXLOG_WARN("PERFWATCH missing exports: vtable={} present={}",
                reinterpret_cast<void*>(vtable), reinterpret_cast<void*>(present));
    return;
  }
  g_present_orig = reinterpret_cast<PresentFn>(present);
  size_t hits = HookVtable(vtable, present, reinterpret_cast<void*>(&HookPresent),
                           "PaintAndPresentImpl")
                    ? 1
                    : 0;
  if (refresh) {
    g_refresh_orig = reinterpret_cast<RefreshFn>(refresh);
    hits += HookVtable(vtable, refresh, reinterpret_cast<void*>(&HookRefresh),
                       "RefreshGuestOutputImpl")
                ? 1
                : 0;
  } else {
    REXLOG_WARN("PERFWATCH RefreshGuestOutputImpl export not found");
  }
  REXLOG_INFO("PERFWATCH hooked {}/2 presenter entry points", hits);
}

}  // namespace rex::glue
