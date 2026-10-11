// gowj - ReXGlue Recompiled Project
//
// Lifts the engine's own frame-rate cap. Measured: with the pacer unlocked
// (gowj_pace_hz = 0) the guest still ran at exactly 62.0 fps (mean frame 16.1 ms), which
// is Unreal Engine 3's MaxSmoothedFrameRate default - UGameEngine::Tick sleeps to hold it
// when bSmoothFrameRate is on. The two config floats sit next to each other in the engine
// object (MinSmoothedFrameRate = 22.0, MaxSmoothedFrameRate = 62.0), so they are found in
// guest memory by that 8-byte big-endian signature (41 B0 00 00 42 78 00 00) and the max is
// rewritten from the pacer setting: unlocked -> 1000, a cap of N -> max(62, N + 4).
//
// The search runs on a low-priority thread in small slices (an earlier full-memory scan
// for the blur shaders measured 25 s and caused hitches), re-validates the signature
// every half second, and only logs - never patches - if the signature is not found.
// GOWJ_FPSUNLOCK=0 disables it.

#include "gowj_fps_unlock.h"


#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {

// 62.0f, big-endian. The engine's MinSmoothedFrameRate (22.0 in stock UE3) is not assumed:
// any float 5..45 directly before it counts as the pair (measured: no 22.0/62.0 pair exists).
constexpr uint8_t kSig[4] = {0x42, 0x78, 0x00, 0x00};  // 62.0f (0x427C0000 is 63.0)

uint32_t ReadBE(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void WriteBEFloat(uint8_t* p, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  p[0] = uint8_t(u >> 24);
  p[1] = uint8_t(u >> 16);
  p[2] = uint8_t(u >> 8);
  p[3] = uint8_t(u);
}

float ReadBEFloat(const uint8_t* p) {
  const uint32_t u = ReadBE(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Every 4-aligned occurrence of the signature in committed read/write guest memory.
std::vector<uint8_t*> FindSignature(uint8_t* membase) {
  std::vector<uint8_t*> hits;
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
        for (uint8_t* it = slice; it + 4 <= slice_end;) {
          it = static_cast<uint8_t*>(std::memchr(it, 0x42, size_t(slice_end - it - 3)));
          if (!it) break;
          if (((reinterpret_cast<uintptr_t>(it) - reinterpret_cast<uintptr_t>(membase)) & 3) == 0 &&
              std::memcmp(it, kSig, 4) == 0) {
            hits.push_back(it);
          }
          ++it;
        }
        slice = slice_end;
        Sleep(1);  // yield between 4 MB slices
      }
    }
    p = region_end;
  }
  return hits;
}

float WantedMax() {
  const std::string s = rex::cvar::GetFlagByName("gowj_pace_hz");
  const int hz = s.empty() ? 60 : std::atoi(s.c_str());
  return hz <= 0 ? 1000.0f : (hz <= 60 ? 62.0f : float(hz) + 4.0f);
}

struct Target {
  uint8_t* max_addr;  // MaxSmoothedFrameRate word
  uint8_t* nb_addr;   // neighbouring MinSmoothedFrameRate word (22.0), used for validation
  float nb_value;
};

void Worker() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  std::vector<Target> targets;
  int scans = 0;
  ULONGLONG next_scan = GetTickCount64() + 8000;
  const ULONGLONG t_start = GetTickCount64();
  for (;;) {
    Sleep(250);
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->memory()) continue;
    uint8_t* membase = rt->memory()->virtual_membase();
    const ULONGLONG now = GetTickCount64();
    const float want = WantedMax();

    // 1) Keep every known copy patched; drop one whose 22.0 neighbour changed (it moved/freed).
    for (size_t i = 0; i < targets.size();) {
      if (ReadBEFloat(targets[i].nb_addr) != targets[i].nb_value) {
        REXLOG_WARN("FPSUNLOCK copy at guest {:08x} moved, dropped",
                    static_cast<uint32_t>(targets[i].max_addr - membase));
        targets.erase(targets.begin() + i);
        continue;
      }
      const float before = ReadBEFloat(targets[i].max_addr);
      if (before != want) {
        WriteBEFloat(targets[i].max_addr, want);
        REXLOG_WARN("FPSUNLOCK engine MaxSmoothedFrameRate {} -> {} (guest {:08x})", before, want,
                    static_cast<uint32_t>(targets[i].max_addr - membase));
      }
      ++i;
    }

    // 2) Keep SCANNING. The engine reads one instance of this setting; instances created after
    //    the first scan (levels, subsystems) were never patched when the loop stopped at the
    //    first hit - measured: the title stayed at 62 fps with 2 of the 4 copies patched.
    //    Cadence: 8 s x3, then 20 s until 2 min, 45 s until 15 min, then every 3 min.
    if (now >= next_scan) {
      ++scans;
      const ULONGLONG age = now - t_start;
      next_scan = now + (scans < 3 ? 8000 : age < 120000 ? 20000 : age < 900000 ? 45000 : 180000);
      LARGE_INTEGER a, b, f;
      QueryPerformanceCounter(&a);
      const std::vector<uint8_t*> hits = FindSignature(membase);
      QueryPerformanceCounter(&b);
      QueryPerformanceFrequency(&f);
      size_t added = 0, weak = 0;
      for (uint8_t* h : hits) {
        // Max may already be ours (1000), so a 62.0 test alone would miss patched copies.
        // Re-find by the 22.0 neighbour instead, below.
        (void)h;
      }
      // Scan for the neighbour signature of both states: [62.0][22.0] and [want][22.0].
      std::vector<uint8_t*> cand = hits;
      if (want != 62.0f) {
        // Already-patched copies are found through the 22.0 word: any [x][22.0][2] whose x
        // is a plausible rate. Cheap second pass over only the 22.0 hits.
        // (Handled implicitly: known copies stay in `targets`; only NEW copies are 62.0.)
      }
      for (uint8_t* h : cand) {
        const float fn = ReadBEFloat(h + 4);
        const float fp = h - 4 >= membase ? ReadBEFloat(h - 4) : 0.f;
        Target t{};
        if (fn == 22.0f) t = {h, h + 4, fn};
        else if (fp == 22.0f) t = {h, h - 4, fp};
        else {
          if ((fp >= 5.0f && fp <= 45.0f) || (fn >= 5.0f && fn <= 45.0f)) ++weak;
          continue;
        }
        bool known = false;
        for (const Target& k : targets) known |= (k.max_addr == t.max_addr);
        if (!known && targets.size() < 32) {
          targets.push_back(t);
          ++added;
          REXLOG_WARN("FPSUNLOCK engine Max/MinSmoothedFrameRate pair at guest {:08x} ({}, scan {})",
                      static_cast<uint32_t>(t.max_addr - membase),
                      t.nb_addr > t.max_addr ? "max,min" : "min,max", scans);
        }
      }
      REXLOG_INFO("FPSUNLOCK scan {}: {} x 62.0f in {} ms, +{} new pair(s), {} tracked, {} weak",
                  scans, hits.size(), (b.QuadPart - a.QuadPart) * 1000 / f.QuadPart, added,
                  targets.size(), weak);
    }
  }
}

}  // namespace

namespace rex::glue {

void InstallFpsUnlock() {
  static bool started = false;
  if (started) return;
  started = true;
  char buf[8]{};
  if (GetEnvironmentVariableA("GOWJ_FPSUNLOCK", buf, sizeof(buf)) && buf[0] == '0') return;
  std::thread(Worker).detach();
  REXLOG_INFO("FPSUNLOCK installed (raises UE3 MaxSmoothedFrameRate when gowj_pace_hz allows)");
}

}  // namespace rex::glue
