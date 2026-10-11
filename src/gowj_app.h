// gowj - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <array>
#include <vector>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <sstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#include <shobjidl.h>
#endif

#include "gowj_kbm_input.h"
#include "gowj_content_probe.h"
#include "gowj_save_io_probe.h"
#include "gowj_blur_kill.h"
#include "gowj_view_accel.h"
#include "gowj_utf8_guard.h"
#include "gowj_xam_input_guard.h"
#include "gowj_perf_watch.h"
#include "gowj_coalesced.h"
#include "gowj_quality.h"
#include "gowj_perf_watch.h"
#include "gowj_crash_watch.h"

REXCVAR_DECLARE(std::string, input_backend);
REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, mnk_mouse);
REXCVAR_DECLARE(double, mnk_sensitivity);
REXCVAR_DECLARE(std::string, keybind_a);
REXCVAR_DECLARE(std::string, keybind_b);
REXCVAR_DECLARE(std::string, keybind_x);
REXCVAR_DECLARE(std::string, keybind_y);
REXCVAR_DECLARE(std::string, keybind_start);
REXCVAR_DECLARE(std::string, keybind_back);
REXCVAR_DECLARE(std::string, keybind_dpad_up);
REXCVAR_DECLARE(std::string, keybind_dpad_down);
REXCVAR_DECLARE(std::string, keybind_dpad_left);
REXCVAR_DECLARE(std::string, keybind_dpad_right);
REXCVAR_DECLARE(std::string, keybind_lstick_up);
REXCVAR_DECLARE(std::string, keybind_lstick_down);
REXCVAR_DECLARE(std::string, keybind_lstick_left);
REXCVAR_DECLARE(std::string, keybind_lstick_right);

class GowjApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GowjApp>(new GowjApp(ctx, "gowj",
        PPCImageConfig));
  }
  // Returns `dir` if it holds default.xex, else the first immediate
  // subdirectory that does (the disc ships as <root>/default.xex plus
  // <root>/GearGame, but users also drop the exe inside the game folder).
  static std::filesystem::path WithXex(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty()) return {};
    if (std::filesystem::exists(dir / "default.xex", ec)) return dir;
    for (std::filesystem::directory_iterator it(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator(); ++it) {
      std::error_code e2;
      if (!it->is_directory(e2)) continue;
      if (std::filesystem::exists(it->path() / "default.xex", e2)) return it->path();
    }
    return {};
  }

  // Content search for a double-clicked exe. Walks the exe's own directory and
  // up to four ancestors (build dir -> out -> gowj -> project root), which
  // covers both a self-contained folder and this repository's layout without
  // ever leaving the project tree to pick up an unrelated copy of the game.
  // GOWJ_HOME (set by the single-file launcher) is the folder the player runs the program from:
  // settings, saves and the remembered game folder live there. Without it, the exe's own folder.
  static std::filesystem::path AppHome() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"GOWJ_HOME", buf, MAX_PATH * 2);
    if (n && n < MAX_PATH * 2) return std::filesystem::path(buf);
    return rex::filesystem::GetExecutablePath().parent_path();
  }

  static std::filesystem::path ResolveContentRoot(const std::filesystem::path& given) {
    std::error_code ec;
    if (!given.empty() && std::filesystem::exists(given, ec)) {
      if (auto hit = WithXex(given); !hit.empty()) return hit;
    }
    std::filesystem::path dir = AppHome();
    for (int depth = 0; !dir.empty(); ++depth) {
      if (auto hit = WithXex(dir); !hit.empty()) {
        if (hit != given) REXLOG_INFO("gowj found content root {}", hit.string());
        return hit;
      }
      if (depth >= 4) break;
      auto parent = dir.parent_path();
      if (parent == dir) break;
      dir = parent;
    }
    // First run of a downloaded release: remembered choice, else ask once.
    const auto remembered = AppHome() / "gowj_game_path.txt";
    {
      std::wifstream in(remembered);
      std::wstring line;
      if (in && std::getline(in, line) && !line.empty()) {
        if (auto hit = WithXex(std::filesystem::path(line)); !hit.empty()) {
          REXLOG_INFO("gowj content root from {}: {}", remembered.string(), hit.string());
          return hit;
        }
      }
    }
    for (;;) {
      const std::filesystem::path picked = PickGameFolder();
      if (picked.empty()) {
        MessageBoxW(nullptr,
                    L"Gears of War: Judgment needs your own game files to run.\n\n"
                    L"Choose the folder that contains default.xex (your disc dump), or "
                    L"put this program's folder inside it.",
                    L"Gears of War: Judgment", MB_OK | MB_ICONINFORMATION);
        ExitProcess(1);
      }
      if (auto hit = WithXex(picked); !hit.empty()) {
        std::wofstream(remembered) << hit.wstring() << L'\n';
        REXLOG_INFO("gowj content root chosen by the player: {}", hit.string());
        return hit;
      }
      MessageBoxW(nullptr,
                  L"That folder does not contain default.xex. Pick the folder of your "
                  L"Gears of War: Judgment game files (the one with default.xex and "
                  L"GearGame).",
                  L"Gears of War: Judgment", MB_OK | MB_ICONWARNING);
    }
  }

  // draw_resolution_scale is registered by the GPU plugin DLL, which loads after
  // ApplyDisplayDefaults runs - so setting it there fails (measured on the first
  // release test: scale_proposed=2x2 but IN EFFECT scale=1x1 [default]). The
  // config file is applied after the plugin registers its cvars, so the
  // automatic value is written into the user's config instead, once, only when
  // the user has not set it: 2x on monitors 1080p or taller, else 1x.
  // The scale line the tier chose lives in a marked block that is rewritten every boot, so
  // a GPU change or a tuner step takes effect. A draw_resolution_scale_ line written by the
  // user OUTSIDE the block always wins and is never touched (returns false = unmanaged).
  // force_auto: an upscaler is on, so the frame buffers follow the screen. A resolution the player
  // picked is set aside (not deleted) and comes back when the upscaler is turned off.
  // The player's own draw_resolution_scale_x line (outside the managed block), or 1.
  static int ReadUserScale(const std::filesystem::path& cfg) {
    std::ifstream in(cfg, std::ios::binary);
    std::string line;
    bool in_block = false;
    while (std::getline(in, line)) {
      if (line.rfind("# >>> gowj auto quality", 0) == 0) { in_block = true; continue; }
      if (in_block) { if (line.rfind("# <<< gowj auto quality", 0) == 0) in_block = false; continue; }
      const size_t b = line.find_first_not_of(" 	");
      if (b != std::string::npos && line.compare(b, 24, "draw_resolution_scale_x ") == 0) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) return std::atoi(line.c_str() + eq + 1);
      }
    }
    return 1;
  }
  int user_scale_ = 1;

  static bool EnsureAutoResolutionScale(const std::filesystem::path& cfg, int scale, bool force_auto = false) {
    static constexpr const char* kSuspended = "#gowj-suspended# ";
    static constexpr const char* kBegin = "# >>> gowj auto quality (managed block";
    static constexpr const char* kEnd = "# <<< gowj auto quality";
    std::string text;
    {
      std::ifstream in(cfg, std::ios::binary);
      if (!in) return false;
      text.assign(std::istreambuf_iterator<char>(in), {});
    }
    std::string kept, line;
    bool in_block = false, user_set = false;
    std::istringstream lines(text);
    int legacy = 0;
    while (std::getline(lines, line)) {
      // Older builds appended the automatic resolution as plain lines (a header comment, one more
      // comment and the two scale lines) with no marker. Left alone they count as a choice made by
      // the player and pin the resolution for every quality level, so they are dropped here.
      if (line.rfind("# Internal resolution, chosen automatically on first run", 0) == 0) {
        legacy = 3;
        continue;
      }
      if (legacy > 0) {
        const size_t lb = line.find_first_not_of(" \t");
        const bool is_comment = lb != std::string::npos && line[lb] == '#';
        const bool is_scale = lb != std::string::npos && line.compare(lb, 22, "draw_resolution_scale_") == 0;
        if (is_comment || is_scale) {
          --legacy;
          continue;
        }
        legacy = 0;
      }
      if (line.rfind(kBegin, 0) == 0) { in_block = true; continue; }
      if (in_block) {
        if (line.rfind(kEnd, 0) == 0) in_block = false;
        continue;
      }
      if (line.rfind(kSuspended, 0) == 0) {
        if (force_auto) {
          kept += line + "\n";
          continue;
        }
        line = line.substr(std::strlen(kSuspended));  // upscaler is off again: the player's choice is back
      }
      const size_t b = line.find_first_not_of(" \t");
      if (b != std::string::npos && line.compare(b, 22, "draw_resolution_scale_") == 0) {
        if (force_auto) {
          kept += std::string(kSuspended) + line + "\n";
          continue;
        }
        user_set = true;
      }
      kept += line + "\n";
    }
    std::string out = kept;
    if (!user_set) {
      out += std::string(kBegin) +
             "; rewritten each launch from the GPU probe - set draw_resolution_scale_x/y\n"
             "# ABOVE this block to take control, or gowj_quality = low|medium|high)\n"
             "draw_resolution_scale_x = " + std::to_string(scale) +
             "\ndraw_resolution_scale_y = " + std::to_string(scale) + "\n" + kEnd + "\n";
    }
    if (out != text) {
      std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
      f << out;
    }
    return !user_set;
  }

  // Standard Windows folder picker. Returns empty if cancelled.
  static std::filesystem::path PickGameFolder() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::filesystem::path result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
      DWORD opts = 0;
      dlg->GetOptions(&opts);
      dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
      dlg->SetTitle(L"Select your Gears of War: Judgment game folder (contains default.xex)");
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->Show(nullptr)) && SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          result = path;
          CoTaskMemFree(path);
        }
        item->Release();
      }
      dlg->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return result;
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // Fill display cvars the user has NOT chosen, from the real monitor. Runs in
  // OnPreSetup, i.e. BEFORE Runtime::Setup() initializes the GPU: several of
  // these are read during graphics init, so setting them later is a no-op that
  // still reports success. Measured: the previous placement
  // (OnPostSetup, 344 ms after "GPU system initialized") AND the unconditional
  // SetFlagByName both made the code claim a mode it never applied - it stomped
  // --video_mode_width=1920 back to 1280 and logged "3840x2160" while the guest
  // allocated a 1280x720 framebuffer. Every value is now written once, early,
  // and only when nothing higher-priority has already set it.
  static bool SetIfUnset(const char* name, const std::string& value) {
    if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) {
      return false;  // config file / REX_* / --flag already won
    }
    return rex::cvar::SetFlagByName(name, value);
  }
  // On a machine with two GPUs (for example an RTX 5070 Ti and an RTX 2070 SUPER), with d3d12_adapter=-1
  // ("any physical") the runtime picked the 2070 SUPER in 41 of 65 logged boots
  // - roughly half the GPU the player bought, on a coin flip per launch. Pick the
  // adapter with the most dedicated VRAM (the fastest card on any sane
  // multi-GPU box) unless the user chose one. DXGI's EnumAdapters1 order is the
  // index d3d12_adapter takes.
  static void PickFastestAdapter() {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(
                             GetProcAddress(dxgi, "CreateDXGIFactory1"))
                       : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1),
                                 reinterpret_cast<void**>(&factory)))) {
      return;
    }
    int best = -1;
    SIZE_T best_vram = 0;
    std::string best_name;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
          !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          desc.DedicatedVideoMemory > best_vram) {
        best = static_cast<int>(i);
        best_vram = desc.DedicatedVideoMemory;
        char name[128]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1,
                            nullptr, nullptr);
        best_name = name;
      }
      adapter->Release();
    }
    factory->Release();
    if (best >= 0 && SetIfUnset("d3d12_adapter", std::to_string(best))) {
      REXLOG_INFO("gowj d3d12_adapter -> {} ({}, {} MB VRAM)", best, best_name,
                  best_vram >> 20);
    }
  }
  rex::glue::QualityPlan quality_plan_;
  std::string low_end_summary_;
  bool quality_plan_ready_ = false;
  // The plan has to exist before OnConfigurePaths writes the resolution into the config, which
  // is earlier than anything else in start-up, so it is computed on first use from there.
  void EnsureQualityPlan(const std::filesystem::path& config_path) {
    if (quality_plan_ready_) return;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    const bool have_mode = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsHeight;
#ifdef GOWJ_DEV_TOOLS
    {
      // Test rigs: GOWJ_FORCE_MONITOR=3840x2160 pins the monitor size the plan works from.
      char fm[32]{};
      unsigned fw = 0, fh = 0;
      if (GetEnvironmentVariableA("GOWJ_FORCE_MONITOR", fm, sizeof fm) && std::sscanf(fm, "%ux%u", &fw, &fh) == 2) {
        dm.dmPelsWidth = fw;
        dm.dmPelsHeight = fh;
      }
    }
#endif
    const uint32_t height = (have_mode || dm.dmPelsHeight) && dm.dmPelsHeight ? dm.dmPelsHeight : 1080;
    quality_plan_ = rex::glue::ChooseQualityPlan(height, config_path, dm.dmPelsWidth);
    quality_plan_ready_ = true;
    // The upscaler (GPU plugin) scales the finished game frame to the screen size.
    if (!rex::glue::kTemporalUpscalers) {
      _putenv_s("REX_GOWJ_UPSCALER", "off");  // wins over a leftover gowj_upscaler line in the config
    } else if (!quality_plan_.upscaler_override.empty() && !std::getenv("REX_GOWJ_UPSCALER")) {
      _putenv_s("REX_GOWJ_UPSCALER", quality_plan_.upscaler_override.c_str());
      REXLOG_INFO("QUALITY upscaler for this session: {} ({})", quality_plan_.upscaler_override, quality_plan_.reason);
    }
    _putenv_s("GOWJ_TARGET_W", std::to_string(dm.dmPelsWidth ? dm.dmPelsWidth : 1920).c_str());
    _putenv_s("GOWJ_TARGET_H", std::to_string(height).c_str());
  }
  void ApplyDisplayDefaults() {
#ifdef _WIN32
    PickFastestAdapter();
    auto env_or = [](const char* k, const char* dflt) -> std::string {
      char buf[512]{};
      DWORD n = GetEnvironmentVariableA(k, buf, sizeof(buf));
      return (n && n < sizeof(buf)) ? std::string(buf, n) : std::string(dflt);
    };
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ||
        !dm.dmPelsWidth || !dm.dmPelsHeight) {
      REXLOG_WARN("gowj EnumDisplaySettings failed, keeping config video mode");
      return;
    }
    EnsureQualityPlan({});
    // GOWJ_RES_SCALE forces the scale (test override); without it the config /
    // command line wins and we only fill the default.
    //
    // The default is 1 - the guest's own 720p raster, unscaled. scale=3 was
    // measured to produce a genuinely sharp 3840x2160 title frame, but it also
    // put a blue cluster over the frame corner that the scale=1 control at the
    // same crop does not have,
    // so it is opt-in rather than shipped. Sharpness effort now goes into the
    // quality cvars at the bottom of this function instead.
    const bool force_scale = GetEnvironmentVariableA("GOWJ_RES_SCALE", nullptr, 0) > 0;
    // Auto default: 2x (2560x1440 internal) on any monitor 1080p or taller - the
    // measured sweet spot (60 fps on an RTX 5070 Ti in the heaviest Act 1 scene
    // with the ROV path); 1x below that. The config/command line still overrides.
    const std::string scale_s =
        env_or("GOWJ_RES_SCALE", std::to_string(quality_plan_.scale).c_str());
    int32_t scale = 1;
    try {
      // 720p is the default. 1440p (2x) is an experimental, demanding choice the player makes in the
      // launcher; it is the only larger size that renders correctly.
      scale = force_scale ? std::clamp(std::stoi(scale_s), 1, 4) : std::clamp(user_scale_, 1, 2);
    } catch (...) {
      scale = 1;
    }
    const std::string eff = std::to_string(scale);
    bool scale_ok = rex::cvar::SetFlagByName("draw_resolution_scale_x", eff) &&
                    rex::cvar::SetFlagByName("draw_resolution_scale_y", eff);
    // The guest's OWN render target. sub_82990A28 stores a literal 720 and a
    // width clamped to [904,1280] into the two globals every buffer size in the
    // engine derives from, and the post-codegen patch reroutes those three writes
    // through gowj_render_width/height (src/gowj_render_scale.cpp,
    // tools/patch_render_resolution.py) so the host can raise them.
    //
    // Measured: raising them is NOT the way to 4K. The Xenon EDRAM is
    // 10 MB, so the guest bands its render passes against a ~2.62 Mpixel budget at
    // 32bpp, and once the composed target cannot fit in one band the final resolve
    // draws N shrunken diagonal copies of the whole frame instead of one image
    // (3840x2160 -> 3 copies; 2560x1440 shatters the
    // same way at 3.69 Mpixel). 1920x1080 (2.07 Mpixel) is clean, which is the
    // ceiling this route can ever reach. So the default leaves the guest on its
    // own 720p path - which bands correctly by design - and the panel is filled by
    // draw_resolution_scale, which scales the emulator's EDRAM and raster instead
    // of the guest's bookkeeping (one correctly composed
    // 3840x2160 frame). Setting gowj_render_* in the config still overrides, for
    // anyone who wants the 1080p guest target.
    // With an upscaler the plan picks the guest frame size from the upscale mode (1080p for
    // Performance, otherwise the guest's own 720p; 1440p comes from the resolution scale).
    bool render_ok =
        quality_plan_.upscaler
            ? (rex::cvar::SetFlagByName("gowj_render_width", std::to_string(quality_plan_.render_w)) &&
               rex::cvar::SetFlagByName("gowj_render_height", std::to_string(quality_plan_.render_h)))
            : (SetIfUnset("gowj_render_width", "0") && SetIfUnset("gowj_render_height", "0"));
    // The mode the guest is TOLD about (XGetVideoMode / VdQueryVideoMode). It
    // feeds only the aspect ratio the engine multiplies its hardcoded 720 by -
    // it does NOT set the render-target size, which is why raising it alone was
    // measured to leave a 1280x720 back buffer.
    bool mode_ok = SetIfUnset("video_mode_width", "1280") &&
                   SetIfUnset("video_mode_height", "720");
    // EDRAM-resolve flags that keep a scaled raster correct.
    static constexpr const char* kResDefaults[][2] = {
        {"clear_memory_page_state", "true"},
        {"direct_host_resolve", "true"},
        {"resolve_resolution_scale_fill_half_pixel_offset", "true"},
        {"mrt_edram_used_range_clamp_to_min", "true"},
        {"d3d12_tiled_shared_memory", "false"},
    };
    std::string applied;
    for (const auto& kv : kResDefaults) {
      bool ok = SetIfUnset(kv[0], kv[1]);
      fmt::format_to(std::back_inserter(applied), "{}={}({}) ", kv[0], kv[1], ok);
    }
    // Everything the emulator can raise about image quality that is not pixel
    // count. Names and ranges are from the live registry dump (config/cvars.txt,
    // written by DumpCvarRegistry below), not from DLL string guesses. The guest
    // engine's own quality is set by its coalesced ini and does not respond to
    // these; what responds is the Xenos->D3D12 translation layer.
    const rex::glue::QualityPlan& qp = quality_plan_;
    const std::string s_aniso = std::to_string(qp.aniso);
    const std::string s_soft = std::to_string(qp.tex_soft_mb);
    const std::string s_hard = std::to_string(qp.tex_hard_mb);
    const std::string s_rt = std::to_string(qp.tex_rt_mb);
    const std::string s_thr = std::to_string(qp.pso_threads);
    std::vector<std::array<const char*, 2>> kQualityDefaults = {
        // Anti-aliasing is SMAA in gowj's own final-image pass (gowj_fsr.cpp); the
        // runtime's FXAA would run before it and only smear the edges.
        {"swap_post_effect", "none"},
        {"native_2x_msaa", qp.msaa2x ? "true" : "false"},
        {"anisotropic_override", s_aniso.c_str()},
        {"use_fuzzy_alpha_epsilon", "true"},
        {"texture_cache_memory_limit_soft", s_soft.c_str()},
        {"texture_cache_memory_limit_hard", s_hard.c_str()},
        {"texture_cache_memory_limit_render_to_texture", s_rt.c_str()},
        {"texture_cache_memory_limit_soft_lifetime", "120"},
        {"d3d12_pipeline_creation_threads", s_thr.c_str()},
        {"gowj_smaa", qp.smaa ? "true" : "false"},
        {"gowj_fsr", qp.fsr ? "true" : "false"},
    };
    // ROV render-target path: measured only on NVIDIA (PIX: ~15x fewer pixel-shader
    // invocations, 52 -> 60 fps). Everywhere else the runtime's own auto path is kept.
    if (qp.rov) kQualityDefaults.push_back({"render_target_path_d3d12", "rov"});
    for (const auto& kv : kQualityDefaults) {
      bool ok = SetIfUnset(kv[0], kv[1]);
      fmt::format_to(std::back_inserter(applied), "{}={}({}) ", kv[0], kv[1], ok);
    }
    // GOWJ_RES_CVARS='name=value;...' forces extras (wins over everything).
    const std::string extra = env_or("GOWJ_RES_CVARS", "");
    size_t pos = 0;
    while (pos < extra.size()) {
      size_t semi = extra.find(';', pos);
      std::string tok = extra.substr(
          pos, semi == std::string::npos ? std::string::npos : semi - pos);
      pos = (semi == std::string::npos) ? extra.size() : semi + 1;
      size_t eq = tok.find('=');
      if (eq == std::string::npos || tok.empty()) continue;
      std::string name = tok.substr(0, eq);
      bool ok = rex::cvar::SetFlagByName(name.c_str(), tok.substr(eq + 1).c_str());
      fmt::format_to(std::back_inserter(applied), "{}={}({}) ", name,
                     tok.substr(eq + 1), ok);
    }
    const std::string hz = std::to_string(dm.dmDisplayFrequency);
    bool hz_ok = dm.dmDisplayFrequency > 0 &&
                 SetIfUnset("video_mode_refresh_rate", hz);
    bool fs_ok = SetIfUnset("fullscreen", "true") &&
                 SetIfUnset("present_letterbox", "false");
    REXLOG_INFO(
        "gowj display PRE-INIT: monitor {}x{} @ {} Hz; guest render target "
        "={}x{} [native={} (0=guest's own 720p)] mode={}x{} "
        "scale_proposed={}x{} (mode={} render={} scale={} refresh={} "
        "fullscreen={}) ; {} - the applied value is the OnPostSetup "
        "'IN EFFECT' read-back, not this line",
        dm.dmPelsWidth, dm.dmPelsHeight, hz,
        rex::cvar::GetFlagByName("gowj_render_width"),
        rex::cvar::GetFlagByName("gowj_render_height"), render_ok,
        rex::cvar::GetFlagByName("video_mode_width"),
        rex::cvar::GetFlagByName("video_mode_height"), eff, eff, mode_ok,
        render_ok, scale_ok, hz_ok, fs_ok, applied);
#endif
  }
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Before any backend init so the graphics system reads these, not its
    // compiled-in defaults.
    ApplyDisplayDefaults();
    config.gpu_plugin = "xenos";
    // Guest content names are raw bytes; rexruntime's UTF-8-validating map keys
    // would abort the process on a bad lead byte (see gowj_utf8_guard.h).
    rex::glue::InstallUtf8Guard();
    rex::glue::InstallContentProbe();
    rex::glue::InstallSaveIoProbe();
    rex::glue::InstallBlurKill();
    rex::glue::InstallViewAccelHook();
    // InputSystem::RefreshDevices rebuilds a std::vector the guest polls from
    // several XThreads at once, with no lock in the runtime - the measured
    // source of the silent 0xC0000374 / 0xC000001D deaths. Serializes it and
    // counts the overlaps it removes; see gowj_xam_input_guard.cpp.
    rex::glue::InstallXamInputGuard();
    rex::glue::InstallDiscErrorSilencer();
    // The frame-pacing instrument: timestamps the presenter's two virtual entry
    // points so "fps" and "pacing" become measured series instead of impressions
    // (see gowj_perf_watch.cpp). Read-only - it hooks, logs, and calls through.
    rex::glue::InstallPerfWatch();
    // Attributes an unhandled host crash to the guest function it happened in.
    // Must be last: it installs an unhandled-exception filter, and the other
    // watchers only observe first-chance events.
    rex::glue::InstallCrashWatch();
    // Own the whole InputSystem so drivers are added before Setup().
    // CreateDefaultInputSystem() already ran Setup(), and AddDriver() after it
    // leaves RefreshDevices reading uninitialised per-driver heap - see
    // gowj_kbm_input.h for the measured fault registers.
    // Drivers: real gamepads first (SDL, the backend this runtime selects by
    // default - measured "input_backend=sdl"), then our keyboard+mouse pad.
    // Replacing the default system drops the gamepad driver with it, which is
    // why a plugged-in Xbox controller was inert; SharedAssignment feeds every
    // device to guest user 0 and InputSystem merges them (buttons OR, triggers
    // max, sticks by larger magnitude), so pad and KBM both work at once.
    config.input_factory = [](bool tool_mode)
        -> std::unique_ptr<rex::system::IInputSystem> {
      REXLOG_INFO("gowj input_factory invoked (tool_mode={})", tool_mode);
      auto sys = std::make_unique<rex::input::GowjInputSystem>(nullptr);
      sys->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
      sys->AddDriver(std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 100));
      sys->AddDriver(std::make_unique<rex::input::GowjKbmInputDriver>());
      sys->Setup();
      REXLOG_INFO("gowj input system built (SDL gamepad + KBM, pre-Setup)");
      return sys;
    };
  }
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Double-click support. ReXGlue has no content search: with no
    // --game_data_root it logs "Runtime::SetupVfs: No game_data_root specified,
    // skipping VFS setup" and boots nothing. Resolve the disc folder here so
    // the exe finds its own content, and keep the flag as an override.
    paths.game_data_root = ResolveContentRoot(paths.game_data_root);
    paths.update_data_root = paths.game_data_root / "$SystemUpdate";
    // Saves live next to the exe, so a copied-out folder keeps its checkpoints
    // and the install stays portable. Deliberately not paths.user_data_root:
    // on some machines that resolves inside a OneDrive-synced Documents tree, where
    // placeholder files and roaming make guest save behaviour unpredictable.
    std::error_code ec;
    saves_root_ = AppHome() / "saves";
    std::filesystem::create_directories(saves_root_, ec);
    // Shader/pipeline storage and runtime user data default to Documents\gowj,
    // which on many PCs (this one included) is a OneDrive-synced folder: the
    // pipeline store is appended during play, and every append makes OneDrive
    // re-sync the file - an I/O stall source in the middle of gameplay. Keep them
    // on local, unsynced storage, carrying an existing cache over once.
    if (wchar_t local[MAX_PATH]{}; GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
      const std::filesystem::path base = std::filesystem::path(local) / L"GearsOfWarJudgmentPC";
      const std::filesystem::path old_cache = paths.cache_root;
      const std::filesystem::path new_cache = base / "cache";
      if (!old_cache.empty() && std::filesystem::exists(old_cache, ec) &&
          !std::filesystem::exists(new_cache, ec)) {
        std::filesystem::create_directories(new_cache, ec);
        std::filesystem::copy(old_cache, new_cache,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::skip_existing,
                              ec);
      }
      // Runtime user data (profile settings etc.) is migrated the same way.
      const std::filesystem::path old_user = paths.user_data_root;
      const std::filesystem::path new_user = base / "user";
      if (!old_user.empty() && std::filesystem::exists(old_user, ec) &&
          !std::filesystem::exists(new_user, ec)) {
        std::filesystem::create_directories(new_user, ec);
        for (std::filesystem::directory_iterator it(old_user, ec), end; !ec && it != end;
             it.increment(ec)) {
          if (it->path() == old_cache) continue;  // cache handled above
          std::error_code cec;
          std::filesystem::copy(it->path(), new_user / it->path().filename(),
                                std::filesystem::copy_options::recursive |
                                    std::filesystem::copy_options::skip_existing,
                                cec);
        }
        ec.clear();
      }
      paths.cache_root = new_cache;
      paths.user_data_root = new_user;
      std::filesystem::create_directories(paths.cache_root, ec);
      std::filesystem::create_directories(paths.user_data_root, ec);
    }
    // Ship the tunables as a file rather than a command line: the settings
    // overlay writes back to paths.config_path, so pointing it at the config
    // directory next to the exe makes menu changes persist and keeps every
    // value editable without a rebuild. Source::kConfig still loses to REX_*
    // and --flag= overrides, so the file can never fight a deliberate flag.
    std::filesystem::path cfg_dir = AppHome() / "config";
    paths.config_path = cfg_dir / "gowj.toml";
    if (!std::filesystem::exists(paths.config_path, ec)) {
      // First run: promote the shipped defaults to the user file, which is the
      // one the settings overlay writes back to.
      std::filesystem::create_directories(cfg_dir, ec);
      auto shipped = rex::filesystem::GetExecutablePath().parent_path() / "config" / "gowj.default.toml";
      if (!std::filesystem::exists(shipped, ec)) shipped = cfg_dir / "gowj.default.toml";
      std::filesystem::copy_file(shipped, paths.config_path,
                                 std::filesystem::copy_options::skip_existing, ec);
    }
    EnsureQualityPlan(paths.config_path);
    // Low preset only: lighter engine settings, applied to a cached copy of the player's own
    // Coalesced files (the game folder is not modified). Cleared otherwise.
    _putenv_s("GOWJ_VFS_OVERRIDES", "");
    std::string vfs_overrides;
    {
      const auto opts = rex::glue::OptionsFromConfig(paths.config_path, quality_plan_.low_end, quality_plan_.tier >= 1, quality_plan_.tier,
                                                  rex::glue::QualityDrawDistanceSteps(quality_plan_.gpu_name));
      _putenv_s("GOWJ_NATIVE_KEYS", rex::glue::ForwardedKeys(opts).c_str());
      const auto co = rex::glue::PrepareCoalescedOverrides(paths.game_data_root, paths.cache_root, opts);
      vfs_overrides = co.overrides;
      low_end_summary_ = co.summary;
    }
#ifdef GOWJ_DEV_TOOLS
    {
      // Test rigs: GOWJ_VFS_TEST="guest/path=host file;..." adds file overrides for experiments.
      char t[2048]{};
      if (GetEnvironmentVariableA("GOWJ_VFS_TEST", t, sizeof t) && t[0]) {
        if (!vfs_overrides.empty()) vfs_overrides += ";";
        vfs_overrides += t;
      }
    }
#endif
    if (!vfs_overrides.empty()) _putenv_s("GOWJ_VFS_OVERRIDES", vfs_overrides.c_str());
    rex::glue::QualityStartTuner(
        quality_plan_, EnsureAutoResolutionScale(paths.config_path, quality_plan_.scale, false));
    user_scale_ = ReadUserScale(paths.config_path);
    // Nothing logged here: SetupEnvironment runs paths *before* logging, so
    // REXLOG_* calls in this hook are dropped. Paths and the config load are
    // reported from OnPostInitLogging instead.
    paths_ = paths;
  }
  void OnPostInitLogging() override {
    if (!quality_plan_.summary.empty()) REXLOG_INFO("{}", quality_plan_.summary);
    if (!low_end_summary_.empty()) REXLOG_INFO("{}", low_end_summary_);
    REXLOG_INFO("gowj paths: game='{}' user='{}' update='{}' cache='{}' "
                "metadata='{}' config='{}' saves='{}'",
                paths_.game_data_root.string(), paths_.user_data_root.string(),
                paths_.update_data_root.string(), paths_.cache_root.string(),
                paths_.metadata_root.string(), paths_.config_path.string(),
                saves_root_.string());
    std::error_code ec;
    if (std::filesystem::exists(paths_.config_path, ec)) {
      rex::cvar::LoadConfig(paths_.config_path);
      REXLOG_INFO("gowj config loaded from {}", paths_.config_path.string());
    } else {
      REXLOG_WARN("gowj no config file at {} (defaults and flags only)",
                  paths_.config_path.string());
    }
    // After LoadConfig, so a value in the file (or REX_* / --flag=) always wins.
    rex::input::ApplyPreferredKeybindings();
  }
  // One-shot inventory of every tunable the runtime exposes, written beside the
  // config file. The reason it exists: nothing in the SDK ships a sample config
  // and a key that is not a real cvar is skipped silently, so the only way to
  // write a correct file (or document one) is to read the registry back out.
  // The `source` column is what proves whether a config key actually landed.
  // Run from OnPreLaunchModule rather than earlier: the overlay binds
  // (bind_console, bind_settings, ...) are registered by RegisterBind() when the
  // ImGui dialogs are built, which is after logging and path setup.
  void OnPreLaunchModule() override { DumpCvarRegistry(); }
  void DumpCvarRegistry() {
    std::error_code ec;
    std::filesystem::path out_path =
        paths_.config_path.parent_path() / "cvars.txt";
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      REXLOG_WARN("gowj could not write cvar dump to {}", out_path.string());
      return;
    }
    static constexpr const char* kTypes[] = {
        "bool", "int32", "int64", "uint32", "uint64", "double", "string",
        "command"};
    static constexpr const char* kSources[] = {
        "default", "config", "environment", "commandline", "runtime"};
    int n = 0;
    for (const auto& entry : rex::cvar::GetRegistry()) {
      out << entry.category << '\t' << entry.name << '\t'
          << kTypes[std::min<size_t>(static_cast<size_t>(entry.type), 7)] << '\t'
          << kSources[std::min<size_t>(static_cast<size_t>(entry.source), 4)]
          << '\t' << entry.default_value << '\t'
          << (entry.getter ? entry.getter() : std::string()) << "\t\t"
          << entry.description << '\n';
      ++n;
    }
    out.close();
    REXLOG_INFO("gowj wrote {} cvars to {}", n, out_path.string());
  }
  // One-time copy of checkpoints written under the old one-dir-per-alias layout
  // into the shared directory: the newest non-empty copy of each file wins. The
  // old SGn_0 directories are left untouched as a backup.
  static void MigratePerAliasCheckpoints(const std::filesystem::path& saves,
                                         const std::filesystem::path& shared) {
    std::error_code ec;
    std::filesystem::create_directories(shared, ec);
    for (int i = 0; i < 8; ++i) {
      std::filesystem::path legacy = saves / ("SG" + std::to_string(i) + "_0");
      for (std::filesystem::directory_iterator it(legacy, ec), end; !ec && it != end;
           it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || it->file_size(fec) == 0 || fec) continue;
        std::filesystem::path dst = shared / it->path().filename();
        const bool have = std::filesystem::exists(dst, fec) &&
                          std::filesystem::file_size(dst, fec) > 0;
        if (have && std::filesystem::last_write_time(dst, fec) >=
                        std::filesystem::last_write_time(it->path(), fec)) {
          continue;
        }
        std::filesystem::copy_file(it->path(), dst,
                                   std::filesystem::copy_options::overwrite_existing,
                                   fec);
        REXLOG_INFO("gowj checkpoint {} -> {} ({})", it->path().string(),
                    dst.string(), fec ? fec.message() : "ok");
      }
      ec.clear();
    }
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  void OnPostLoadXexImage() override {
    // XamContentCreateEx forwards pszRootName into the path verbatim, so every
    // root name this title uses must be its own registered VFS device or
    // NtCreateFile fails and the save-check flow dies. Measured root names, in
    // the order they first appear in the boot log:
    //   savedrive0:  PlayerStorage.dat (Xbox user storage)
    //   SG1_0:       GearsCheckpoint, first player slot   
    //   SG2_0:       GearsCheckpoint, second player slot  
    //   SG0_0:       gamer-data drive the guest probes at boot
    // Measured: the slot scan walks SG0_0..SG5_0 one
    // slot every 30-90 s, so mounting only four left SG4_0/SG5_0 unregistered
    // and their XamContentCreateEx failed - the guest then treats the storage
    // as broken and offers to overwrite it. Mount eight: the observed range
    // plus headroom for a split-screen party. Each root gets its own host
    // directory: the guest pairs a name with a file, never with a directory
    // layout, so separate trees are safe and keep per-user checkpoints from
    // overwriting each other.
    std::filesystem::path saves =
        saves_root_.empty() ? std::filesystem::path("saves") : saves_root_;
    // Only SG0_0 is static (the boot probe opens it before any hook could see a
    // fresh alias); SG1_0 and up are mounted on demand by the content shim.
    static constexpr const char* kRoots[] = {"SG0_0:", "savedrive0:", "savedrive1:",
                                             "savedrive2:", "savedrive3:"};
    // SGn_0 is NOT a slot: it is a symbolic root the guest re-rolls every time it
    // opens the GearsCheckpoint package, and on hardware every alias resolves to
    // the same content package. Measured: with one host dir per alias,
    // each session's checkpoint landed in a different SGn_0 (SG1..SG7 each held
    // one), so the next boot opened another alias, found nothing or a 0-byte probe,
    // and reported the save corrupted. All checkpoint aliases share one directory.
    // Absolute: the runtime changes the working directory after this point, and
    // the save shims resolve host files from this path at call time.
    const std::filesystem::path checkpoints =
        std::filesystem::absolute(saves / "GearsCheckpoint");
    MigratePerAliasCheckpoints(saves, checkpoints);
    rex::glue::SetCheckpointDir(checkpoints);
    for (const char* root : kRoots) {
      std::error_code ec;
      std::string_view r(root);  // "SG1_0:" -> dir "SG1_0"
      const bool checkpoint_alias = r.compare(0, 2, "SG") == 0;
      std::filesystem::path dir =
          checkpoint_alias ? checkpoints
                           : saves / std::string(r.substr(0, r.size() - 1));
      std::filesystem::create_directories(dir, ec);
      // The guest's boot-time slot probe opens each checkpoint with
      // create-then-close and writes nothing, leaving a 0-byte file behind. The
      // NEXT boot's save-check runs early (at the title, well before the late
      // probe-create) and reads that empty file as a corrupted save, offering to
      // overwrite it - the "saves corrupted every launch" report. A real
      // checkpoint is ~164 KB, so a zero-length file is never a valid save:
      // drop it before the guest probes so the slot reads as empty, not corrupt.
      //
      // Scoped to checkpoint probes ONLY. Measured: the unscoped sweep
      // deleted a PlayerStorage.dat and the guest's very next open of it failed
      // with STATUS_NO_SUCH_FILE - xam creates that file empty on real hardware
      // before the game writes it, so an empty PlayerStorage.dat is legitimate.
      bool user_storage = r.size() >= 9 && r.compare(0, 9, "savedrive") == 0;
      for (std::filesystem::directory_iterator it(
               dir, std::filesystem::directory_options::skip_permission_denied,
               ec);
           !ec && it != std::filesystem::directory_iterator(); ++it) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        auto sz = it->file_size(fec);
        if (fec || sz != 0) continue;
        if (it->path().filename().string().find("PlayerStorage") !=
            std::string::npos) {
          continue;
        }
        if (std::filesystem::remove(it->path(), fec) && !fec) {
          REXLOG_INFO("gowj removed empty save probe {}", it->path().string());
        }
      }
      // XamContentCreateEx for PlayerStorage.dat returns 0x3E5 (IO_PENDING) and
      // the runtime's deferred worker never puts the file on disk, so the guest's
      // follow-up NtCreateFile('savedriveN:\PlayerStorage.dat') fails and the
      // boot-time storage check reports the user's storage as broken. Provide the
      // file xam would have made so that open succeeds.
      if (user_storage) {
        std::error_code eec;
        std::filesystem::path ps = dir / "PlayerStorage.dat";
        if (!std::filesystem::exists(ps, eec)) {
          std::ofstream create(ps, std::ios::binary);
          if (create.is_open()) {
            create.close();
            REXLOG_INFO("gowj created empty {} (xam would; runtime's pending "
                        "content-create never lands)",
                        ps.string());
          } else {
            REXLOG_WARN("gowj could not create {}", ps.string());
          }
        }
      }
      auto device = std::make_unique<rex::filesystem::HostPathDevice>(
          root, dir, /*read_only=*/false, /*allow_share_delete=*/true);
      bool init = device->Initialize();
      bool ok = runtime()->file_system()->RegisterDevice(std::move(device));
      if (checkpoint_alias) rex::glue::NoteMountedRoot(std::string(r));
      REXLOG_INFO("gowj {} -> {} init={} mount={} (ec={})", root,
                  dir.string(), init, ok, ec.message());
    }

    // The disc itself. Judgment addresses its own content as D:\GearGame\... and
    // without that device every movie and every streaming texture cache fails
    // with 0xc000000f: measured in probe_gp.game.log, Attract_Opening_Cinematic.bik
    // is requested twice during a single 5-minute PRESS START wait (23:49:58 and
    // 23:53:17), i.e. the title is stuck cycling the intro it cannot play, and
    // Lighting.xxx / Textures.xxx / CharTextures.xxx - the streamed high-resolution
    // character and level texture caches - never open at all. The packages that
    // boot needs are reached another way, which is why the title renders; nothing
    // that lives only on the disc can.
    //
    // game_data_root is the directory the loader found default.xex in, so this
    // mounts the player's own dump and ships no game content of its own.
    //
    // GATED OFF by default. Measured, both sides: with D: mounted
    // the run died at mission start AND at the PRESS START title (0xC0000374 /
    // 0xC000001D / a second-chance AV, no [FATAL] line); with it unmounted the
    // player gets into and plays the mission - but a title-idle death still
    // reproduced at ~12 s (0xC0000005), so THE MOUNT
    // IS NOT THE ROOT CAUSE. It is a frequency knob on an unnamed memory-corruption
    // bug that is still open: newly-reachable disc content drives guest code that
    // had never executed, and that makes the existing fault land far more often.
    // Unmounted is the playable configuration, which is why it is the default; the
    // movies it costs are cosmetic. Set GOWJ_DISC_MOUNT=1 to restore disc access
    // when chasing the corruption itself.
    char mount_env[8]{};
    const bool disc_mounted =
        GetEnvironmentVariableA("GOWJ_DISC_MOUNT", mount_env, sizeof(mount_env)) &&
        mount_env[0] == '1';
    if (!disc_mounted) {
      REXLOG_WARN(
          "gowj D: NOT mounted (GOWJ_DISC_MOUNT=1 to enable). Disc movies fail "
          "to open and are skipped; enabling it makes the guest's Bink decoder "
          "run, which currently crashes at mission start.");
    } else {
      std::error_code dec;
      auto disc = std::make_unique<rex::filesystem::HostPathDevice>(
          "D:", paths_.game_data_root, /*read_only=*/true,
          /*allow_share_delete=*/false);
      bool dinit = disc->Initialize();
      bool dok = runtime()->file_system()->RegisterDevice(std::move(disc));
      REXLOG_INFO("gowj D: -> {} (disc) init={} mount={} (ec={})",
                  paths_.game_data_root.string(), dinit, dok, dec.message());
    }
  }
  void OnPostSetup() override {
    // READ-BACK ONLY. The display values are written in ApplyDisplayDefaults()
    // before the GPU initializes; nothing set here would be honoured, and the
    // unconditional writes that used to live here are what let the code log
    // "3840x2160" while the guest allocated a 1280x720 framebuffer. This now
    // reports the state the guest actually got, with each value's winning source
    // so a stale config line can never again be mistaken for an applied setting.
    // It deliberately does NOT report a pixel count: the only proof of that is the
    // guest's own "Created tiled WxH 2D k_8_8_8_8" line in the GPU log.
    static auto src = [](const char* n) {
      switch (rex::cvar::GetFlagSource(n)) {
        case rex::cvar::Source::kDefault: return "default";
        case rex::cvar::Source::kConfig: return "config";
        case rex::cvar::Source::kEnvironment: return "env";
        case rex::cvar::Source::kCommandLine: return "cli";
        case rex::cvar::Source::kRuntime: return "runtime";
      }
      return "?";
    };
    REXLOG_INFO(
        "gowj display IN EFFECT: guest video_mode={}x{} @ {} [{}x{}] scale={}x{} "
        "[{}x{}] fullscreen={} letterbox={} vsync={} tearing={} "
        "present_effect={} d3d12_tiled={} clear_pages={}",
        rex::cvar::GetFlagByName("video_mode_width"),
        rex::cvar::GetFlagByName("video_mode_height"),
        rex::cvar::GetFlagByName("video_mode_refresh_rate"), src("video_mode_width"),
        src("video_mode_height"),
        rex::cvar::GetFlagByName("draw_resolution_scale_x"),
        rex::cvar::GetFlagByName("draw_resolution_scale_y"),
        src("draw_resolution_scale_x"), src("draw_resolution_scale_y"),
        rex::cvar::GetFlagByName("fullscreen"),
        rex::cvar::GetFlagByName("present_letterbox"),
        rex::cvar::GetFlagByName("vsync"),
        rex::cvar::GetFlagByName(
            "d3d12_allow_variable_refresh_rate_and_tearing"),
        rex::cvar::GetFlagByName("present_effect"),
        rex::cvar::GetFlagByName("d3d12_tiled_shared_memory"),
        rex::cvar::GetFlagByName("clear_memory_page_state"));
    REXLOG_INFO("INPUTDUMP backend={} mnk_mode={} mnk_mouse={} sens={}",
                REXCVAR_GET(input_backend), REXCVAR_GET(mnk_mode),
                REXCVAR_GET(mnk_mouse), REXCVAR_GET(mnk_sensitivity));
    REXLOG_INFO("INPUTDUMP a='{}' b='{}' x='{}' y='{}' start='{}' back='{}'",
                REXCVAR_GET(keybind_a), REXCVAR_GET(keybind_b),
                REXCVAR_GET(keybind_x), REXCVAR_GET(keybind_y),
                REXCVAR_GET(keybind_start), REXCVAR_GET(keybind_back));
    REXLOG_INFO("INPUTDUMP dup='{}' ddown='{}' dleft='{}' dright='{}'",
                REXCVAR_GET(keybind_dpad_up), REXCVAR_GET(keybind_dpad_down),
                REXCVAR_GET(keybind_dpad_left), REXCVAR_GET(keybind_dpad_right));
    REXLOG_INFO("INPUTDUMP lup='{}' ldown='{}' lleft='{}' lright='{}'",
                REXCVAR_GET(keybind_lstick_up), REXCVAR_GET(keybind_lstick_down),
                REXCVAR_GET(keybind_lstick_left), REXCVAR_GET(keybind_lstick_right));
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}

 private:
  rex::PathConfig paths_;
  std::filesystem::path saves_root_;
};
