// gowj - ReXGlue Recompiled Project
//
// Opt-in whole-process profiler (GOWJ_PROFILE=1) built so a performance question can be
// answered from ONE play session and then analysed offline:
//   * PROF cpu   - per-thread CPU load every 10 s (which thread is the bottleneck).
//   * PROF gpu   - this process's GPU 3D / copy engine utilisation from the Windows GPU
//                  performance counters (works on NVIDIA, AMD and Intel, desktop and handheld).
//   * PROF leaf/incl - a statistical stack profile of the busiest threads: ~100 samples/s,
//                  round robin, each sample suspends the thread only long enough to unwind
//                  (no allocation while suspended). Guest frames are named sub_XXXXXXXX through
//                  codegen's own PPCFuncMappings table; everything else is module+RVA.
// Cumulative tables are re-logged every 20 s because the process hard-exits.

#include "gowj_profiler.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dxgi.h>
#include <intrin.h>

#include <atomic>
#include <mutex>


#include <rex/logging.h>
#include <rex/ppc/func.h>

namespace {

struct Mod {
  uintptr_t base, end;
  std::string name;
};

std::vector<Mod> g_mods;
uintptr_t g_exe_base = 0, g_exe_end = 0;
std::vector<std::pair<uintptr_t, uint32_t>> g_host_to_guest;  // sorted by host address

void RefreshModules() {
  std::vector<Mod> mods;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                         GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return;
  MODULEENTRY32 me{sizeof(me)};
  if (Module32First(snap, &me)) {
    do {
      mods.push_back({reinterpret_cast<uintptr_t>(me.modBaseAddr),
                      reinterpret_cast<uintptr_t>(me.modBaseAddr) + me.modBaseSize, me.szModule});
    } while (Module32Next(snap, &me));
  }
  CloseHandle(snap);
  g_mods.swap(mods);
}

void BuildGuestMap() {
  if (!g_host_to_guest.empty()) return;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) {
    g_host_to_guest.push_back({reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest)});
  }
  std::sort(g_host_to_guest.begin(), g_host_to_guest.end());
}

std::string Label(uintptr_t pc) {
  char buf[96];
  if (pc >= g_exe_base && pc < g_exe_end) {
    auto it = std::upper_bound(g_host_to_guest.begin(), g_host_to_guest.end(),
                               std::make_pair(pc, UINT32_MAX));
    if (it != g_host_to_guest.begin()) {
      --it;
      std::snprintf(buf, sizeof(buf), "sub_%08X", it->second);
      return buf;
    }
  }
  for (const Mod& m : g_mods) {
    if (pc >= m.base && pc < m.end) {
      std::snprintf(buf, sizeof(buf), "%s+0x%llx", m.name.c_str(),
                    (unsigned long long)(pc - m.base));
      return buf;
    }
  }
  std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)pc);
  return buf;
}

std::string ThreadName(DWORD tid) {
  std::string out;
  if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
    PWSTR w = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
      for (PWSTR p = w; *p; ++p) out += char(*p < 128 ? *p : '?');
      LocalFree(w);
    }
    CloseHandle(h);
  }
  return out;
}

uint64_t FileTimeU64(const FILETIME& f) {
  return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

struct ThreadProf {
  std::string name;
  uint64_t samples = 0;
  double cpu_pct = 0;
  std::unordered_map<std::string, uint32_t> leaf, incl;
};

std::unordered_map<DWORD, ThreadProf> g_prof;
std::vector<DWORD> g_hot;  // busiest threads, refreshed every 10 s

struct CpuSnap {
  uint64_t cpu100ns;
};
std::unordered_map<DWORD, CpuSnap> g_prev_cpu;

// Per-thread CPU over the last interval; returns tids sorted busiest first.
void SampleCpu(double wall_s) {
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  struct Row {
    DWORD tid;
    double pct;
  };
  std::vector<Row> rows;
  double total = 0;
  THREADENTRY32 te{sizeof(te)};
  std::unordered_map<DWORD, CpuSnap> now_cpu;
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID != pid) continue;
      HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
      if (!h) continue;
      FILETIME c, e, k, u;
      if (GetThreadTimes(h, &c, &e, &k, &u)) {
        const uint64_t t = FileTimeU64(k) + FileTimeU64(u);
        now_cpu[te.th32ThreadID] = {t};
        auto p = g_prev_cpu.find(te.th32ThreadID);
        if (p != g_prev_cpu.end() && wall_s > 0) {
          const double pct = 100.0 * double(t - p->second.cpu100ns) / 1e7 / wall_s;
          rows.push_back({te.th32ThreadID, pct});
          total += pct;
        }
      }
      CloseHandle(h);
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  g_prev_cpu.swap(now_cpu);
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
  std::string line;
  g_hot.clear();
  for (size_t i = 0; i < rows.size() && i < 8; ++i) {
    char tmp[160];
    const std::string n = ThreadName(rows[i].tid);
    std::snprintf(tmp, sizeof(tmp), "%s t%X %.0f%% | ", n.empty() ? "?" : n.c_str(), rows[i].tid,
                  rows[i].pct);
    line += tmp;
    if (rows[i].pct >= 12.0 && g_hot.size() < 4) {
      g_hot.push_back(rows[i].tid);
      g_prof[rows[i].tid].name = n;
      g_prof[rows[i].tid].cpu_pct = rows[i].pct;
    }
  }
  REXLOG_INFO("PROF cpu total={:.0f}% (100% = one core) top: {}", total, line);
}

// GPU engine utilisation of this process: sums the per-engine counters, and reports the busiest
// single 3D engine because that, not the sum, is what bounds the frame rate.
void SampleGpu() {
  PDH_HQUERY q = nullptr;
  if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
  wchar_t path3d[160], pathcopy[160];
  swprintf(path3d, 160, L"\\GPU Engine(pid_%lu*engtype_3D)\\Utilization Percentage",
           GetCurrentProcessId());
  swprintf(pathcopy, 160, L"\\GPU Engine(pid_%lu*engtype_Copy)\\Utilization Percentage",
           GetCurrentProcessId());
  PDH_HCOUNTER c3d = nullptr, ccopy = nullptr;
  const bool ok3d = PdhAddEnglishCounterW(q, path3d, 0, &c3d) == ERROR_SUCCESS;
  const bool okcp = PdhAddEnglishCounterW(q, pathcopy, 0, &ccopy) == ERROR_SUCCESS;
  if (ok3d || okcp) {
    PdhCollectQueryData(q);
    Sleep(1000);
    PdhCollectQueryData(q);
    auto sum_max = [](PDH_HCOUNTER c, double* sum, double* mx) {
      DWORD bytes = 0, count = 0;
      PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
      if (st != PDH_MORE_DATA || !bytes) return;
      std::vector<char> buf(bytes);
      auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
      if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) {
        return;
      }
      for (DWORD i = 0; i < count; ++i) {
        const double v = items[i].FmtValue.doubleValue;
        *sum += v;
        *mx = std::max(*mx, v);
      }
    };
    double s3 = 0, m3 = 0, sc = 0, mc = 0;
    if (ok3d) sum_max(c3d, &s3, &m3);
    if (okcp) sum_max(ccopy, &sc, &mc);
    REXLOG_INFO("PROF gpu 3D busiest-engine={:.0f}% sum={:.0f}% | copy sum={:.0f}%", m3, s3, sc);
  }
  PdhCloseQuery(q);
}

void Dump(const char* tag) {
  BuildGuestMap();
  RefreshModules();
  for (auto& [tid, tp] : g_prof) {
    if (tp.samples < 20) continue;
    REXLOG_INFO("PROF ==== [{}] {} t{:X} cpu={:.0f}% samples={}", tag, tp.name.empty() ? "?" : tp.name,
                tid, tp.cpu_pct, tp.samples);
    for (int pass = 0; pass < 2; ++pass) {
      auto& m = pass == 0 ? tp.leaf : tp.incl;
      std::vector<std::pair<uint32_t, const std::string*>> v;
      for (auto& [k, n] : m) v.push_back({n, &k});
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
      for (size_t i = 0; i < v.size() && i < (pass == 0 ? 14u : 22u); ++i) {
        REXLOG_INFO("PROF   {} {:5.1f}% {}", pass == 0 ? "leaf" : "incl",
                    100.0 * v[i].first / double(tp.samples), *v[i].second);
      }
    }
  }
}

int UnwindThread(HANDLE h, uint64_t* pcs, int max) {
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(h, &ctx)) return 0;
  int n = 0;
  for (; n < max && ctx.Rip; ++n) {
    pcs[n] = ctx.Rip;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
    if (!rf) {
      if (!ctx.Rsp) break;
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    PVOID hd = nullptr;
    DWORD64 ef = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &ef, nullptr);
  }
  return n;
}

void Worker() {
  RefreshModules();
  BuildGuestMap();
  uint64_t tick = 0;
  size_t rr = 0;
  LARGE_INTEGER f, last;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&last);
  ULONGLONG next_dump = GetTickCount64() + 20000;
  // The first CPU sample just establishes the baseline.
  SampleCpu(0);
  for (;;) {
    Sleep(10);
    ++tick;
    if (tick % 1000 == 0) {  // ~10 s
      LARGE_INTEGER n;
      QueryPerformanceCounter(&n);
      const double wall = double(n.QuadPart - last.QuadPart) / double(f.QuadPart);
      last = n;
      SampleCpu(wall);
      SampleGpu();  // takes ~1 s
    }
    if (g_hot.empty()) continue;
    const DWORD tid = g_hot[rr++ % g_hot.size()];
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, tid);
    if (!h) continue;
    uint64_t pcs[24];
    int n = 0;
    if (SuspendThread(h) != (DWORD)-1) {
      n = UnwindThread(h, pcs, 24);
      ResumeThread(h);
    }
    CloseHandle(h);
    if (n <= 0) continue;
    ThreadProf& tp = g_prof[tid];
    ++tp.samples;
    ++tp.leaf[Label(pcs[0])];
    std::unordered_set<std::string> seen;
    for (int i = 0; i < n; ++i) {
      std::string l = Label(pcs[i]);
      if (seen.insert(l).second) ++tp.incl[l];
    }
    if (GetTickCount64() >= next_dump) {
      next_dump = GetTickCount64() + 20000;
      Dump("cumulative");
    }
  }
}


// ---- frame log ---------------------------------------------------------------------------
// GOWJ_FRAMELOG=<path>: EVERY frame of the session, flushed every 3 s, so one play session is a
// complete dataset (the runtime's own GOWJ_PERF_CSV keeps only a short rolling window).
//   # header line with wall-clock start, then  t_ms,kind,interval_us,draws,disp,pso,barriers,copies
// kind r = guest frame (the thing to optimise), p = present. Command counts are deltas since the
// previous r row.
struct FrameRow {
  float t_ms;
  char kind;
  uint32_t iv;
  uint32_t draws, disp, pso, barriers, copies;
};
std::mutex g_frame_mu;
std::vector<FrameRow> g_frame_rows;
std::string g_frame_path;
LARGE_INTEGER g_frame_qpc0, g_frame_freq;
uint64_t g_prev_cmd[7];
std::atomic<bool> g_frame_on{false};

void FrameLogFlusher() {
  bool header_done = false;
  for (;;) {
    Sleep(3000);
    std::vector<FrameRow> rows;
    {
      std::lock_guard<std::mutex> lk(g_frame_mu);
      rows.swap(g_frame_rows);
    }
    FILE* f = std::fopen(g_frame_path.c_str(), header_done ? "ab" : "wb");
    if (!f) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        REXLOG_WARN("FRAMELOG cannot open {}", g_frame_path);
      }
      continue;
    }
    if (!header_done) {
      SYSTEMTIME st;
      GetLocalTime(&st);
      std::fprintf(f, "# gowj frametimes start_local=%04d-%02d-%02dT%02d:%02d:%02d.%03d columns=t_ms,kind,interval_us,draws,disp,pso,barriers,copies\n",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
      header_done = true;
    }
    for (const FrameRow& r : rows) {
      std::fprintf(f, "%.3f,%c,%u,%u,%u,%u,%u,%u\n", r.t_ms, r.kind, r.iv, r.draws, r.disp, r.pso,
                   r.barriers, r.copies);
    }
    std::fclose(f);
    static uint64_t total = 0;
    total += rows.size();
    static int logged = 0;
    if (logged++ < 3 || logged % 10 == 0) {
      REXLOG_INFO("FRAMELOG wrote {} rows (total {}) to {}", rows.size(), total, g_frame_path);
    }
  }
}

void SysInfo() {
  char brand[64]{};
  int r[4];
  for (unsigned i = 0; i < 3; ++i) {
    __cpuid(r, 0x80000002 + i);
    std::memcpy(brand + 16 * i, r, 16);
  }
  SYSTEM_INFO si;
  GetNativeSystemInfo(&si);
  MEMORYSTATUSEX ms{sizeof(ms)};
  GlobalMemoryStatusEx(&ms);
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm);
  std::string gpu = "?";
  std::string driver = "?";
  if (HMODULE dxgi = LoadLibraryA("dxgi.dll")) {
    auto create = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* fac = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac)))) {
      IDXGIAdapter1* a = nullptr;
      SIZE_T best = 0;
      for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory >= best) {
          best = d.DedicatedVideoMemory;
          gpu.clear();
          for (const wchar_t* p = d.Description; *p; ++p) gpu += char(*p < 128 ? *p : '?');
          LARGE_INTEGER umd{};
          if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            char tmp[64];
            std::snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", unsigned(umd.HighPart >> 16),
                          unsigned(umd.HighPart & 0xFFFF), unsigned(umd.LowPart >> 16),
                          unsigned(umd.LowPart & 0xFFFF));
            driver = tmp;
          }
        }
        a->Release();
      }
      fac->Release();
    }
  }
  RTL_OSVERSIONINFOW os{sizeof(os)};
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"))) fn(&os);
  REXLOG_INFO("SYSINFO cpu='{}' logical_cpus={} ram={}MB os={}.{}.{} display={}x{}@{}Hz gpu='{}' driver={}",
              brand, si.dwNumberOfProcessors, ms.ullTotalPhys >> 20, os.dwMajorVersion, os.dwMinorVersion,
              os.dwBuildNumber, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, gpu, driver);
}

}  // namespace

namespace rex::glue {


void FrameLogNote(size_t kind, uint64_t interval_us) {
  if (!g_frame_on.load(std::memory_order_relaxed) || kind > 1) return;
  LARGE_INTEGER n;
  QueryPerformanceCounter(&n);
  FrameRow row{float(1000.0 * double(n.QuadPart - g_frame_qpc0.QuadPart) / double(g_frame_freq.QuadPart)),
               kind == 1 ? 'r' : 'p', uint32_t(interval_us), 0, 0, 0, 0, 0};
  std::lock_guard<std::mutex> lk(g_frame_mu);
  if (kind == 1) {
    uint64_t cur[7];
    for (int i = 0; i < 7; ++i) cur[i] = 0;  // command-list counters need the private D3D timing hook
    row.draws = uint32_t(cur[0] + cur[1] - g_prev_cmd[0] - g_prev_cmd[1]);
    row.disp = uint32_t(cur[2] - g_prev_cmd[2]);
    row.pso = uint32_t(cur[3] - g_prev_cmd[3]);
    row.barriers = uint32_t(cur[4] - g_prev_cmd[4]);
    row.copies = uint32_t(cur[5] - g_prev_cmd[5]);
    for (int i = 0; i < 7; ++i) g_prev_cmd[i] = cur[i];
  }
  if (g_frame_rows.size() < 2000000) g_frame_rows.push_back(row);
}

void InstallProfiler() {
  static bool started = false;
  if (started) return;
  started = true;
  char buf[8]{};
  char path[MAX_PATH]{};
  if (GetEnvironmentVariableA("GOWJ_FRAMELOG", path, sizeof(path)) && path[0]) {
    g_frame_path = path;
    QueryPerformanceFrequency(&g_frame_freq);
    QueryPerformanceCounter(&g_frame_qpc0);
    g_frame_on = true;
    std::thread(FrameLogFlusher).detach();
    REXLOG_INFO("FRAMELOG on -> {}", g_frame_path);
  }
  const bool prof_on = GetEnvironmentVariableA("GOWJ_PROFILE", buf, sizeof(buf)) && buf[0] == '1';
  if (prof_on || g_frame_on) SysInfo();
  if (!prof_on) return;
  HMODULE self = GetModuleHandleA(nullptr);
  g_exe_base = reinterpret_cast<uintptr_t>(self);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(self) +
                                                 reinterpret_cast<IMAGE_DOS_HEADER*>(self)->e_lfanew);
  g_exe_end = g_exe_base + nt->OptionalHeader.SizeOfImage;
  std::thread(Worker).detach();
  REXLOG_WARN("PROFILER on (GOWJ_PROFILE=1): thread CPU, GPU engine load and a stack profile are "
              "logged as PROF lines; expect ~1-2% overhead");
}

}  // namespace rex::glue
