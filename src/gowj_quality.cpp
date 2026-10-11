// gowj - ReXGlue Recompiled Project
//
// Picks graphics settings from what the machine can do.
//
// Rationale: 2x internal resolution (2560x1440) with the ROV render-target path holds 60 fps
// on an RTX 5070 Ti, but the same setting ran at about 20 fps on an RTX 2070 SUPER, so neither
// monitor height nor video memory alone predicts speed. The tier choice is therefore
// conservative, and the tuner below steps DOWN when 60 fps is not held.
//
// Only NVIDIA hardware has been measured. AMD, Intel and handheld GPUs get a tier from
// capability rules only: ROV and the High tier are enabled automatically for NVIDIA only,
// everything else uses the runtime's own render path and tops out at Medium unless
// gowj_quality = "high" is set.

#include "gowj_quality.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(gowj_low_end_tweaks, true, "gfx",
                    "Low level only: switch off the game's lights, shadows, decals and post effects "
                    "(applied to a cached copy of the game's own config; Medium and High never use it)");
REXCVAR_DEFINE_STRING(gowj_quality, "auto", "gfx",
                      "Graphics quality tier: auto (probe the GPU, step down if 60 fps is not "
                      "held), low (handheld / integrated), medium, high.");

namespace rex::glue {
namespace {

std::string Narrow(const wchar_t* w) {
  std::string s;
  for (; *w; ++w) s += char(*w < 128 ? *w : '?');
  return s;
}

// Value of the active `key = value` line of a config file ("" if there is none).
std::string ConfigValue(const std::filesystem::path& path, const char* key) {
  static const char* const kBlank = " \t\r";
  static const char* const kBlankQuote = " \t\r\"";
  std::ifstream in(path);
  std::string line;
  const size_t klen = std::strlen(key);
  while (std::getline(in, line)) {
    const size_t b = line.find_first_not_of(kBlank);
    if (b == std::string::npos || line[b] == '#' || line.compare(b, klen, key) != 0) continue;
    size_t e = b + klen;
    while (e < line.size() && (line[e] == ' ' || line[e] == '\t')) ++e;
    if (e >= line.size() || line[e] != '=') continue;
    std::string v = line.substr(e + 1);
    const size_t hash = v.find('#');
    if (hash != std::string::npos) v.resize(hash);
    const size_t f = v.find_first_not_of(kBlankQuote);
    const size_t l = v.find_last_not_of(kBlankQuote);
    return f == std::string::npos ? "" : v.substr(f, l - f + 1);
  }
  return "";
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct Probe {
  std::string name;
  uint32_t vendor = 0;
  uint64_t vram_mb = 0, budget_mb = 0;
  bool uma = false, rov = false, ok = false;
};

Probe ProbeAdapter(int wanted_index) {
  Probe p;
  HMODULE dxgi = LoadLibraryA("dxgi.dll");
  HMODULE d3d12 = LoadLibraryA("d3d12.dll");
  if (!dxgi || !d3d12) return p;
  auto create_factory = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
      GetProcAddress(dxgi, "CreateDXGIFactory1"));
  auto create_device = reinterpret_cast<HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID,
                                                         void**)>(
      GetProcAddress(d3d12, "D3D12CreateDevice"));
  IDXGIFactory1* factory = nullptr;
  if (!create_factory || !create_device ||
      FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return p;
  }
  IDXGIAdapter1* chosen = nullptr;
  SIZE_T best_vram = 0;
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    const bool usable = SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
    const bool take = usable && (wanted_index >= 0 ? int(i) == wanted_index
                                                   : d.DedicatedVideoMemory >= best_vram);
    if (take) {
      if (chosen) chosen->Release();
      chosen = a;
      best_vram = d.DedicatedVideoMemory;
      p.name = Narrow(d.Description);
      p.vendor = d.VendorId;
      p.vram_mb = d.DedicatedVideoMemory >> 20;
      p.budget_mb = p.vram_mb;
      if (wanted_index >= 0) break;
    } else {
      a->Release();
    }
  }
  factory->Release();
  if (!chosen) return p;
  IDXGIAdapter3* a3 = nullptr;
  if (SUCCEEDED(chosen->QueryInterface(IID_PPV_ARGS(&a3)))) {
    DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
    if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) {
      p.budget_mb = mi.Budget >> 20;
    }
    a3->Release();
  }
  ID3D12Device* dev = nullptr;
  if (SUCCEEDED(create_device(chosen, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))) {
      p.rov = o.ROVsSupported != FALSE;
    }
    D3D12_FEATURE_DATA_ARCHITECTURE1 ar{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &ar, sizeof(ar)))) {
      p.uma = ar.UMA != FALSE;
    }
    dev->Release();
    p.ok = true;
  }
  chosen->Release();
  return p;
}

std::wstring UserDir() {
  wchar_t base[MAX_PATH]{};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return L"";
  std::wstring d = std::wstring(base) + L"\\GearsOfWarJudgmentPC";
  CreateDirectoryW(d.c_str(), nullptr);
  d += L"\\user";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

// autotune.ini: "gpu=<name>" and "offset=<-N>" (how many tiers below the probe's pick this
// GPU has been measured to need).
struct TuneState {
  std::string gpu;
  int offset = 0;
  int dd_steps = 0;  // how many times draw distance has been shortened for this GPU
};

TuneState ReadTune() {
  std::ifstream in(UserDir() + L"\\autotune.ini");
  std::string line;
  TuneState t;
  while (std::getline(in, line)) {
    if (line.rfind("gpu=", 0) == 0) t.gpu = line.substr(4);
    if (line.rfind("offset=", 0) == 0) t.offset = std::atoi(line.c_str() + 7);
    if (line.rfind("dd=", 0) == 0) t.dd_steps = std::atoi(line.c_str() + 3);
  }
  return t;
}

int ReadOffset(const std::string& gpu) {
  const TuneState t = ReadTune();
  return (t.gpu == gpu && t.offset < 0) ? t.offset : 0;
}

void WriteTune(const std::string& gpu, int off, int dd) {
  std::ofstream out(UserDir() + L"\\autotune.ini", std::ios::trunc);
  out << "gpu=" << gpu << "\noffset=" << off << "\ndd=" << dd << "\n";
}

struct Tier {
  const char* name;
  int scale;
  bool smaa;
  int aniso;
  bool msaa2x;
  int tex_soft, tex_hard, tex_rt;
};
constexpr Tier kTiers[3] = {
    {"low", 1, false, 3, false, 512, 1024, 64},
    {"medium", 1, true, 4, true, 1024, 2048, 128},
    {"high", 1, true, 5, true, 2048, 4096, 128},
};

std::mutex g_mu;
std::vector<double> g_win;  // frame intervals (ms) in the current 30 s window
std::string g_notice;
QualityPlan g_plan;
int g_offset = 0;
bool g_managed = false;

}  // namespace

QualityPlan ChooseQualityPlan(uint32_t monitor_height, const std::filesystem::path& config_path,
                              uint32_t monitor_width) {
  QualityPlan q;
  int idx = -1;
  try {
    std::string s = rex::cvar::GetFlagByName("d3d12_adapter");
    if (!config_path.empty()) {
      const std::string c = ConfigValue(config_path, "d3d12_adapter");
      if (!c.empty()) s = c;
    }
    if (!s.empty()) idx = std::stoi(s);
  } catch (...) {
  }
  const Probe p = ProbeAdapter(idx);
  q.gpu_name = p.name;
  q.vendor = p.vendor;
  q.vram_mb = p.vram_mb;
  q.budget_mb = p.budget_mb;
  q.uma = p.uma;
  q.rov_supported = p.rov;
  q.cores = std::max(1u, std::thread::hardware_concurrency());

  std::string requested = Lower(REXCVAR_GET(gowj_quality));
  if (!config_path.empty()) {
    const std::string c = ConfigValue(config_path, "gowj_quality");
    if (!c.empty()) requested = Lower(c);
  }
  char env[32]{};
  if (GetEnvironmentVariableA("GOWJ_QUALITY", env, sizeof(env))) requested = Lower(env);

  const bool nvidia = p.vendor == 0x10DE;
  int base;
  std::string why;
  if (!p.ok) {
    base = 1;
    why = "GPU probe failed -> medium";
  } else if (p.uma || p.vram_mb < 3500) {
    base = 0;
    why = p.uma ? "integrated/UMA GPU (handheld class) -> low"
                : "under 3.5 GB dedicated VRAM -> low";
  } else if (p.vram_mb < 11000) {
    base = 1;
    why = "dedicated VRAM under 11 GB -> medium";
  } else {
    base = 2;
    why = "12 GB+ discrete -> high";
  }
  if (base == 2 && !(nvidia && p.rov)) {
    base = 1;
    why += "; high needs ROV on NVIDIA (the only measured path) -> medium";
  }
  if (base == 2 && monitor_height < 1080) {
    base = 1;
    why += "; panel under 1080p -> medium";
  }

  int tier = base;
  q.source = "auto";
  if (requested == "low" || requested == "medium" || requested == "high") {
    tier = requested == "low" ? 0 : requested == "medium" ? 1 : 2;
    q.source = "override:" + requested;
    why = "gowj_quality=" + requested;
  } else {
    g_offset = ReadOffset(p.name);
    if (g_offset < 0) {
      tier = std::max(0, base + g_offset);
      q.source = "tuned(" + std::to_string(g_offset) + ")";
      why += "; lowered " + std::to_string(-g_offset) + " tier(s): 60 fps was not held earlier";
    }
  }

  const Tier& t = kTiers[tier];
  // Lighter engine settings: Low tier only (explicit, automatic or tuned), never Medium or High.
  q.low_end = tier == 0;
  if (q.low_end) {
    std::string opt = config_path.empty() ? std::string() : Lower(ConfigValue(config_path, "gowj_low_end_tweaks"));
    char e[8]{};
    if (GetEnvironmentVariableA("GOWJ_LOW_END", e, sizeof(e))) opt = e[0] == '0' ? "false" : "true";
    if (opt == "false") q.low_end = false;
  }
  q.tier = tier;
  q.tier_name = t.name;
  q.scale = t.scale;
  q.smaa = t.smaa;
  {
    // A temporal upscaler renders the scene below the output size and reconstructs the rest, so
    // the target (internal resolution scale) follows the display: 1 -> 720p, 2 -> 1440p, 3 -> 4K.
    std::string up = config_path.empty() ? std::string() : Lower(ConfigValue(config_path, "gowj_upscaler"));
    if (kTemporalUpscalers && !up.empty() && up != "off") {
      q.upscaler = true;
      q.smaa = false;  // the upscaler already resolves aliasing
      // The whole game frame is drawn at the render size and the upscaler scales the finished frame
      // to the screen, so a smaller render size is what makes it faster. Only sizes the game draws
      // cleanly are used: 1440p is its 2x resolution scale, 1080p and 720p are native guest sizes
      // (3x was measured to be slow and to put blue speckle over the picture).
      int mode = -1;
      if (!config_path.empty()) {
        const std::string m = ConfigValue(config_path, "gowj_upscale_quality");
        if (!m.empty()) mode = std::atoi(m.c_str());
      }
      if (mode < 0) mode = tier == 0 ? 3 : tier == 1 ? 0 : 2;  // Auto: Low / Medium / High
      if (mode != 0 && mode != 3) mode = 2;                     // anything else is Quality
      q.upscale_mode = mode;
      // The render size follows the screen, so every screen gets the same quality / performance /
      // ultra performance ratios: two thirds, one half and one third of the output height. On a 4K
      // panel that is exactly 1440p (the game's 2x scale), 1080p and 720p; on a 1440p panel it is
      // 960p, 720p and 480p; on a 1080p panel 720p, 540p and 360p.
      const double ratio = mode == 2 ? 2.0 / 3.0 : mode == 0 ? 0.5 : 1.0 / 3.0;
      const uint32_t out_h = monitor_height ? monitor_height : 1080;
      const uint32_t out_w = monitor_width ? monitor_width : (out_h * 16 + 8) / 9;
      const double aspect = double(out_w) / double(out_h);
      uint32_t rh = uint32_t(out_h * ratio + 0.5);
      // The game cannot draw below its native 720p (below that its own surface-size check stops it:
      // measured, a 960x540 target aborts), and above ~2.1 million pixels the EDRAM tiling breaks, so
      // the height is clamped to what is proven: 720 .. 1080 for 16:9, less for very wide screens.
      // Measured on screenshots: guest render targets over about 1.3 million pixels (1712x960, 1920x1080,
      // 2560x720, 2736x768) come out corrupted (garbage bands, black lower half). The sizes that render
      // correctly are the native 1280x720, wider 720-high targets up to 1792 wide, and the 2x scale
      // (1440p). So the render height is either 720 or, on a 16:9-ish screen asking for 1440p, 1440.
      if (!(rh >= 1400 && rh <= 1460 && aspect < 1.9)) rh = 720;
      rh = std::min<uint32_t>(rh, 1460);
      // An upscaler needs a real size difference to be worth it: on a screen barely larger than 720p
      // the game is drawn natively (SMAA and the final-image pass) instead.
      if (out_h < uint32_t(rh * 1.2)) {
        q.upscaler = false;
        q.smaa = t.smaa;
        q.reason += "; screen too small for upscaling (output " + std::to_string(out_h) + "p vs render " +
                    std::to_string(rh) + "p) -> native";
      } else {
        // Sizes the game already draws are used as they are: 720p is its own native size and 1440p
        // is its proven 2x scale. Anything else is a guest render target override.
        q.scale = 1;
        q.render_w = q.render_h = 0;
        if (rh >= 1400 && rh <= 1460 && aspect < 1.9) {
          q.scale = 2;
        }
        // No other guest render size is used: every override measured so far (1712x720 included) shows
        // corrupted bands. Wider screens get the native 16:9 picture with side bars.
      }
      q.render_final_h = rh;
      // Measured on screen captures while walking: DLSS flickers the textures when it has to upscale by
      // more than about 2.5x (720p to 4K: sharpness swings in 6% of frames at 8% or more, against 0.2% at
      // 2x and nothing at 1.5x), while FSR 3.1 at the same ratio is steady and a little faster. Those
      // sizes use FSR 3.1 for the session instead; DLSS stays for every other ratio.
      if ((up == "dlss" || up == "auto") && rh && double(out_h) / double(rh) > 2.5) {
        q.upscaler_override = "fsr3";
        q.reason += "; DLSS would upscale " + std::to_string(rh) + "p to " + std::to_string(out_h) +
                    "p (more than 2.5x) and flickers, so FSR 3.1 is used";
      }
    }
  }
  {
    // Never rasterize more than 1440p worth of pixels: above that the EDRAM scaling breaks and puts blue
    // speckle over the picture (measured at a 3840x2160 render target).
    const double gw = q.render_w ? q.render_w : 1280.0, gh = q.render_h ? q.render_h : 720.0;
    if (q.scale > 1 && gw * gh * q.scale * q.scale > 3.8e6) q.scale = 1;
  }
  q.fsr = true;
  q.aniso = t.aniso;
  q.msaa2x = t.msaa2x;
  q.tex_rt_mb = t.tex_rt;
  q.tex_hard_mb = t.tex_hard;
  const int cap = int(std::max<uint64_t>(256, p.budget_mb * 45 / 100));
  q.tex_hard_mb = std::min(t.tex_hard, cap);
  q.tex_soft_mb = std::min(t.tex_soft, q.tex_hard_mb / 2);
  const int cores_q = int(q.cores / 4);
  q.pso_threads = tier == 0 ? std::clamp(cores_q, 1, 2) : std::clamp(cores_q, 2, 3);
  q.rov = nvidia && p.rov;
  q.reason = why;
  g_plan = q;
  q.summary = fmt::format(
      "QUALITY gpu='{}' vendor={:04X} vram={}MB budget={}MB uma={} rov={} cores={} monitor_h={} "
      "-> tier={} ({}) scale={} smaa={} aniso={} msaa2x={} tex={}/{}/{}MB pso_threads={} rov_path={} "
      "upscaler={} mode={} guest_render={}x{} [{}: {}]",
      q.gpu_name, q.vendor, q.vram_mb, q.budget_mb, q.uma, q.rov_supported, q.cores,
      monitor_height, q.tier, q.tier_name, q.scale, q.smaa, q.aniso, q.msaa2x, q.tex_soft_mb,
      q.tex_hard_mb, q.tex_rt_mb, q.pso_threads, q.rov, q.upscaler, q.upscale_mode, q.render_w,
      q.render_h, q.source, q.reason);
  return q;
}

// ---- tuner ---------------------------------------------------------------------------
// Steps DOWN only. A window is 30 s of guest frames, collected only while the game is the
// foreground window and 75 s after boot (load screens). The median frame interval is the
// test: the pacer holds 16.67 ms when 60 fps is met, so a median over 18.5 ms means the
// game cannot hold 60, and a p95 over 20 ms means more than 5% of frames miss it. Three consecutive failing windows (90 s) lower the tier for the next
// launch - one window would trip on a long cutscene/movie.
void QualityNoteFrame(uint64_t interval_us) {
  if (interval_us < 4000 || interval_us > 2000000) return;
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_win.size() < 20000) g_win.push_back(interval_us / 1000.0);
}

int QualityDrawDistanceSteps(const std::string& gpu) {
  const TuneState t = ReadTune();
  return (t.gpu == gpu && t.dd_steps > 0) ? std::min(t.dd_steps, 2) : 0;
}

std::string QualityOverlayLine() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_notice.empty()) return g_notice;
  if (g_plan.gpu_name.empty()) return "";
  return std::string("quality: ") + g_plan.tier_name + " (" + g_plan.source + ")";
}

void QualityStartTuner(const QualityPlan& plan, bool managed) {
  static bool started = false;
  if (started) return;
  started = true;
  g_managed = managed;
  if (!managed || plan.source.rfind("override", 0) == 0) {
    REXLOG_INFO("QUALITY tuner off (managed={} tier={} source={})", managed, plan.tier,
                plan.source);
    return;
  }
  std::thread([plan] {
    const DWORD pid = GetCurrentProcessId();
    ULONGLONG t0 = GetTickCount64() + 75000;  // skip boot / first load
    int fails = 0;
    bool fg_ok = true;
    ULONGLONG win_start = t0;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_win.clear();
    }
    for (;;) {
      Sleep(1000);
      const ULONGLONG now = GetTickCount64();
      DWORD fpid = 0;
      GetWindowThreadProcessId(GetForegroundWindow(), &fpid);
      if (fpid != pid) fg_ok = false;
      if (now < t0) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_win.clear();
        win_start = now;
        continue;
      }
      if (now - win_start < 30000) continue;
      std::vector<double> w;
      {
        std::lock_guard<std::mutex> lk(g_mu);
        w.swap(g_win);
      }
      const bool valid = fg_ok && w.size() > 600;
      win_start = now;
      fg_ok = true;
      if (!valid) {
        fails = 0;
        continue;
      }
      std::sort(w.begin(), w.end());
      const double p50 = w[w.size() / 2];
      const double p99 = w[std::min(w.size() - 1, size_t(w.size() * 0.99))];
      const double p95 = w[std::min(w.size() - 1, size_t(w.size() * 0.95))];
      // p50: cannot hold 60 at all. p95: holds it on average but more than 1 frame in 20 is
      // late (a 33 ms miss under the 60 Hz pacer) - the "drops in heavy scenes" case that a
      // median never sees (measured High on the 5070 Ti stuttered, Low did not).
      const bool miss = p50 > 18.5 || p95 > 20.0;
      fails = miss ? fails + 1 : 0;
      REXLOG_INFO("QUALITY window: {} frames p50={:.1f}ms p95={:.1f}ms p99={:.1f}ms -> {} (fails={})",
                  w.size(), p50, p95, p99, miss ? "below 60fps" : "ok", fails);
      if (fails >= 3) {
        const TuneState cur = ReadTune();
        const int dd = cur.gpu == plan.gpu_name ? cur.dd_steps : 0;
        std::string msg;
        int new_off = std::min(g_offset, 0);
        int new_dd = dd;
        if (dd < 2) {
          // First step: draw less far. It costs the least picture and often fixes a CPU-bound scene.
          new_dd = dd + 1;
          msg = "60 fps not held - draw distance will be shortened on next launch";
        } else if (plan.tier > 0) {
          new_off = new_off - 1;
          new_dd = 0;  // the lower tier starts again from its own draw distance
          msg = std::string("60 fps not held at ") + plan.tier_name + " - quality will be lowered on next launch";
        } else {
          REXLOG_INFO("QUALITY lowest tier and shortest draw distance already in use");
          return;
        }
        WriteTune(plan.gpu_name, new_off, new_dd);
        {
          std::lock_guard<std::mutex> lk(g_mu);
          g_notice = msg;
        }
        REXLOG_WARN("QUALITY {} (offset {} dd {} saved)", msg, new_off, new_dd);
        return;  // one step per session; the next launch re-measures
      }
    }
  }).detach();
}

}  // namespace rex::glue
