// gowj - ReXGlue Recompiled Project
//
// FPS / frame-time counter. A separate click-through, topmost, no-activate layered
// window pinned to the game window's top-left corner, painted with GDI, so it works
// regardless of the runtime's presenter, FSR/SMAA pass or ImGui (none of which it
// touches). Fed by the PERFWATCH hooks (gowj_perf_watch.cpp OnEvent):
//   kind 0 = presenter Present (paced, can repeat frames), kind 1 = guest output refresh
//   (one per frame the game actually rendered) - FPS is taken from the refresh stream.
// Shown only while the game window is the foreground window. F9 toggles it; F8 toggles the
// frame-rate limit between unlocked and the configured cap (gowj_pace_hz, default 60).
// GOWJ_FPS=0 removes it entirely.

#include "gowj_fps_overlay.h"
#include "gowj_quality.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace {

struct Sample {
  int64_t qpc;
  double ms;
};

std::mutex g_mu;
std::deque<Sample> g_refresh;  // last ~10 s of guest frames
std::deque<Sample> g_present;  // last ~2 s of presents
LARGE_INTEGER g_freq;
bool g_visible = true;
HWND g_overlay = nullptr;
HWND g_game = nullptr;
char g_lines[5][110] = {"FPS --", "", "", "", ""};

double NowQpcMs(int64_t q) { return 1000.0 * double(q) / double(g_freq.QuadPart); }

void Trim(std::deque<Sample>& d, int64_t now, double keep_ms) {
  while (!d.empty() && NowQpcMs(now - d.front().qpc) > keep_ms) d.pop_front();
}

HWND FindGameWindow() {
  struct Ctx {
    DWORD pid;
    HWND best;
    long best_area;
  } ctx{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != c->pid || !IsWindowVisible(h) || h == g_overlay) return TRUE;
        if (GetWindow(h, GW_OWNER)) return TRUE;
        RECT r{};
        GetClientRect(h, &r);
        const long area = (r.right - r.left) * (r.bottom - r.top);
        if (area > c->best_area) {
          c->best = h;
          c->best_area = area;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.best;
}

void Recompute() {
  LARGE_INTEGER n;
  QueryPerformanceCounter(&n);
  std::lock_guard<std::mutex> lk(g_mu);
  Trim(g_refresh, n.QuadPart, 10000.0);
  Trim(g_present, n.QuadPart, 2000.0);
  // Last 1 s of guest frames.
  int cnt = 0;
  double sum = 0, worst = 0;
  for (auto it = g_refresh.rbegin(); it != g_refresh.rend(); ++it) {
    if (NowQpcMs(n.QuadPart - it->qpc) > 1000.0) break;
    ++cnt;
    sum += it->ms;
    worst = std::max(worst, it->ms);
  }
  if (cnt == 0) {
    snprintf(g_lines[0], sizeof(g_lines[0]), "FPS --");
    g_lines[1][0] = g_lines[2][0] = g_lines[4][0] = 0;
    snprintf(g_lines[3], sizeof(g_lines[3]), "%s", rex::glue::QualityOverlayLine().c_str());
    return;
  }
  const double avg = sum / cnt;
  // 1% low over the last 10 s: fps equivalent of the 99th-percentile frame time.
  std::vector<double> all;
  all.reserve(g_refresh.size());
  for (const auto& s : g_refresh) all.push_back(s.ms);
  std::sort(all.begin(), all.end());
  const double p99 = all.empty() ? avg : all[std::min(all.size() - 1, size_t(all.size() * 0.99))];
  const double pres_fps = g_present.size() > 1 ? 1000.0 * (g_present.size() - 1) /
                                                     std::max(1.0, NowQpcMs(g_present.back().qpc - g_present.front().qpc))
                                               : 0.0;
  snprintf(g_lines[0], sizeof(g_lines[0]), "%.0f FPS  %.1f ms", 1000.0 / avg, avg);
  snprintf(g_lines[1], sizeof(g_lines[1]), "1%% low %.0f   worst %.0f ms", 1000.0 / p99, worst);
  const std::string lim = rex::cvar::GetFlagByName("gowj_pace_hz");
  snprintf(g_lines[2], sizeof(g_lines[2]), "present %.0f/s   limit %s [F8]", pres_fps,
           (lim.empty() || lim == "0") ? "UNLOCKED" : lim.c_str());
  snprintf(g_lines[3], sizeof(g_lines[3]), "%s", rex::glue::QualityOverlayLine().c_str());
  // Temporal upscaler status (set by the GPU plugin; empty when the upscaler is off).
  const std::string up = rex::cvar::GetFlagByName("gowj_upscale_status");
  snprintf(g_lines[4], sizeof(g_lines[4]), "%s", up.c_str());
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH key = CreateSolidBrush(RGB(0, 0, 0));  // color key = transparent
    FillRect(dc, &rc, key);
    DeleteObject(key);
    static HFONT font = CreateFontA(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                    NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    int y = 2;
    for (int i = 0; i < 5; ++i) {
      if (!g_lines[i][0]) continue;
      const int len = int(strlen(g_lines[i]));
      SetTextColor(dc, RGB(24, 24, 24));  // shadow (not the key colour)
      TextOutA(dc, 3, y + 1, g_lines[i], len);
      SetTextColor(dc, i == 0   ? RGB(80, 255, 80)
                       : i == 3 ? RGB(255, 200, 80)
                       : i == 4 ? RGB(120, 200, 255)
                                : RGB(230, 230, 230));
      TextOutA(dc, 2, y, g_lines[i], len);
      y += 22;
    }
    SelectObject(dc, old);
    EndPaint(h, &ps);
    return 0;
  }
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  return DefWindowProcA(h, m, w, l);
}

void OverlayThread() {
  WNDCLASSA wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "gowj_fps_overlay";
  RegisterClassA(&wc);
  g_overlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT |
                                  WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              wc.lpszClassName, "gowj_fps", WS_POPUP, 0, 0, 720, 118, nullptr,
                              nullptr, wc.hInstance, nullptr);
  if (!g_overlay) {
    REXLOG_WARN("FPS overlay: window creation failed ({})", GetLastError());
    return;
  }
  SetLayeredWindowAttributes(g_overlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
  REXLOG_INFO("FPS overlay ready (F9 toggles, GOWJ_FPS=0 disables)");
  bool f9_prev = false, f8_prev = false;
  int tick = 0;
  std::string saved_limit = "60";
  for (;;) {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    if (!g_game || !IsWindow(g_game)) g_game = FindGameWindow();
#ifdef GOWJ_DEV_TOOLS
    static const bool always = [] {
      const char* e = std::getenv("GOWJ_FPS_ALWAYS");
      return e && *e == '1';
    }();  // test rigs: show the overlay without window focus (hidden desktop)
    const bool fg = g_game && (always || GetForegroundWindow() == g_game) && !IsIconic(g_game);
#else
    const bool fg = g_game && GetForegroundWindow() == g_game && !IsIconic(g_game);
#endif
    const bool f9 = fg && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !f9_prev) g_visible = !g_visible;
    f9_prev = f9;
    const bool f8 = fg && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !f8_prev) {
      // gowj_pace_hz is read by the pacer on every frame: 0 = free-run (unlocked), N = cap.
      const std::string cur = rex::cvar::GetFlagByName("gowj_pace_hz");
      const bool unlocked = cur.empty() || cur == "0";
      if (!unlocked) saved_limit = cur;
      const std::string next = unlocked ? saved_limit : "0";
      const bool ok = rex::cvar::SetFlagByName("gowj_pace_hz", next);
      REXLOG_WARN("FPS limit -> {} (was {}, applied={})", next == "0" ? "UNLOCKED" : next, cur, ok);
    }
    f8_prev = f8;
    if (++tick % 5 != 0) {  // keys at 50 ms, window/UI work at 250 ms
      Sleep(50);
      continue;
    }
    if (fg && g_visible) {
      POINT p{0, 0};
      ClientToScreen(g_game, &p);
      Recompute();
      SetWindowPos(g_overlay, HWND_TOPMOST, p.x + 12, p.y + 10, 720, 118,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
      InvalidateRect(g_overlay, nullptr, TRUE);
    } else {
      ShowWindow(g_overlay, SW_HIDE);
    }
    Sleep(50);
  }
}

}  // namespace

namespace rex::glue {

void FpsOverlayNote(size_t kind, uint64_t interval_us) {
  if (!interval_us || kind > 1 || !g_freq.QuadPart) return;
  LARGE_INTEGER n;
  QueryPerformanceCounter(&n);
  std::lock_guard<std::mutex> lk(g_mu);
  (kind == 1 ? g_refresh : g_present).push_back({n.QuadPart, interval_us / 1000.0});
}

void InstallFpsOverlay() {
  static bool done = false;
  if (done) return;
  done = true;
  char buf[8]{};
  if (GetEnvironmentVariableA("GOWJ_FPS", buf, sizeof(buf)) && buf[0] == '0') return;
  QueryPerformanceFrequency(&g_freq);
  std::thread(OverlayThread).detach();
}

}  // namespace rex::glue
