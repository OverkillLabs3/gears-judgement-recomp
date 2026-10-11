// Gears of War: Judgment (PC) - settings launcher.
//
// A small native window for the settings people change most: quality level, internal
// resolution, texture filtering, anti-aliasing and upscaling, frame limit, mouse sensitivity.
// It edits config\gowj.toml next to this program (the same file the game and its F4 menu use),
// keeps every other line of that file as it is, and can start the game.
//
// Only components that ship with Windows are used (user32, gdi32, GDI+, dwmapi, shell32) and the
// C runtime is linked statically, so there is nothing to install.
//
//   Graphics Settings.exe --apply key=value [key=value ...]    writes settings without a window

#include <windows.h>
#include "../src/gowj_keys.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "default_config.h"

using namespace Gdiplus;

namespace {

// ------------------------------------------------------------------------------------------
// config file model: a list of lines, edited in place
// ------------------------------------------------------------------------------------------
constexpr const char* kBlockBegin = "# >>> gowj auto quality";
constexpr const char* kBlockEnd = "# <<< gowj auto quality";

std::wstring g_dir;          // folder of this program (and of the game exe)
std::wstring g_cfg_path;     // <dir>\config\gowj.toml
std::vector<std::string> g_lines;

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= text.size()) {
    size_t j = text.find('\n', i);
    if (j == std::string::npos) j = text.size();
    std::string l = text.substr(i, j - i);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    out.push_back(l);
    i = j + 1;
  }
  if (!out.empty() && out.back().empty()) out.pop_back();
  return out;
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  GetFileSizeEx(f, &sz);
  out.resize(size_t(sz.QuadPart));
  DWORD got = 0;
  const bool ok = out.empty() || (ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
  CloseHandle(f);
  return ok;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  const bool ok = text.empty() || (WriteFile(f, text.data(), DWORD(text.size()), &put, nullptr) && put == text.size());
  CloseHandle(f);
  return ok;
}

void LoadConfig() {
  std::string text;
  if (!ReadTextFile(g_cfg_path, text)) text = kDefaultConfig;
  g_lines = SplitLines(text);
}

bool SaveConfig() {
  CreateDirectoryW((g_dir + L"\\config").c_str(), nullptr);
  std::string text;
  for (const auto& l : g_lines) {
    text += l;
    text += '\n';
  }
  return WriteTextFile(g_cfg_path, text);
}

// index of the active "key = value" line outside the managed block, or -1
int FindKey(const std::string& key) {
  bool in_block = false;
  for (size_t i = 0; i < g_lines.size(); ++i) {
    const std::string& l = g_lines[i];
    if (l.rfind(kBlockBegin, 0) == 0) { in_block = true; continue; }
    if (in_block) { if (l.rfind(kBlockEnd, 0) == 0) in_block = false; continue; }
    size_t b = l.find_first_not_of(" \t");
    if (b == std::string::npos || l[b] == '#') continue;
    if (l.compare(b, key.size(), key) == 0) {
      size_t e = b + key.size();
      while (e < l.size() && (l[e] == ' ' || l[e] == '\t')) ++e;
      if (e < l.size() && l[e] == '=') return int(i);
    }
  }
  return -1;
}

bool GetValue(const std::string& key, std::string* value) {
  const int i = FindKey(key);
  if (i < 0) return false;
  std::string v = g_lines[i].substr(g_lines[i].find('=') + 1);
  size_t c = v.find('#');
  if (c != std::string::npos) v = v.substr(0, c);
  size_t b = v.find_first_not_of(" \t\"");
  size_t e = v.find_last_not_of(" \t\"");
  *value = b == std::string::npos ? "" : v.substr(b, e - b + 1);
  return true;
}

void SetValue(const std::string& key, const std::string& value) {
  const std::string line = key + " = " + value;
  const int i = FindKey(key);
  if (i >= 0) { g_lines[i] = line; return; }
  for (size_t k = 0; k < g_lines.size(); ++k) {
    if (g_lines[k].rfind(kBlockBegin, 0) == 0) { g_lines.insert(g_lines.begin() + k, line); return; }
  }
  g_lines.push_back(line);
}

void RemoveKey(const std::string& key) {
  const int i = FindKey(key);
  if (i >= 0) g_lines.erase(g_lines.begin() + i);
}

std::string Num(double v, int decimals) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.push_back('0');
  }
  return s;
}

// ------------------------------------------------------------------------------------------
// the settings
// ------------------------------------------------------------------------------------------
enum Id {
  kQuality, kResolution, kFiltering, kFullscreen, kLowEnd,
  kUpscaler, kUpscaleMode,
  kSmaa, kFsr, kSharpness, kGrain, kBlur,
  kLimit, kVrr,
  kSens, kInvert,
  kPrompts, kFov, kShake, kDrawDist, kSkipLogos, kLanguage,
  kCount
};
enum class Kind { Segmented, Toggle, Slider };

struct Ctl {
  Id id;
  Kind kind;
  const wchar_t* label;
  const wchar_t* hint;
  std::vector<std::wstring> items;
  int sel = -1;
  bool on = false;
  double val = 0, vmin = 0, vmax = 1, step = 0.1;
  const wchar_t* unit = L"";
  std::wstring last;  // what the game actually applied on its last start (from cvars.txt)
  bool dirty = false;
  int rows = 1;  // a segmented control with many items wraps onto this many lines
  RectF row{}, ctl{};
};

struct Card {
  const wchar_t* title;
  std::vector<int> ctls;
  int column;
  const wchar_t* note = nullptr;  // short hint drawn at the bottom of the card
  RectF rect{};
};

Ctl g_c[kCount];
std::vector<Card> g_cards;

// ------------------------------------------------------------------------------------------
// key bindings page
// ------------------------------------------------------------------------------------------
struct BindDef {
  const wchar_t* label;
  const char* toml;
  const char* def;  // default key list, engine key names
};
const BindDef kBinds[] = {
    {L"Move forward", "bind_forward", "W"},
    {L"Move backward", "bind_backward", "S"},
    {L"Move left", "bind_left", "A"},
    {L"Move right", "bind_right", "D"},
    {L"Cover, vault, roll, sprint", "bind_action", "SpaceBar"},
    {L"Crouch", "bind_crouch", "LeftControl,C"},
    {L"Walk", "bind_walk", "Z"},
    {L"Fire", "bind_fire", "LeftMouseButton"},
    {L"Aim", "bind_aim", "RightMouseButton"},
    {L"Melee, execute", "bind_melee", "F,V"},
    {L"Reload", "bind_reload", "R,ThumbMouseButton"},
    {L"Zoom", "bind_zoom", "T,MiddleMouseButton"},
    {L"Swap weapons", "bind_swap_weapon", "One,MouseScrollUp,MouseScrollDown"},
    {L"Grenade", "bind_grenade", "Two"},
    {L"Pick up, revive, use", "bind_pickup", "E"},
    {L"Look at point of interest", "bind_poi", "Q"},
    {L"Tactical command", "bind_tactical", "H"},
    {L"Weapon slot 1", "bind_weapon1", "Three"},
    {L"Weapon slot 2", "bind_weapon2", "Four"},
    {L"Scoreboard", "bind_scoreboard", "Slash"},
};
constexpr int kBindCount = int(sizeof(kBinds) / sizeof(kBinds[0]));
std::vector<std::string> g_bind_keys[kBindCount];
bool g_bind_dirty = false;
int g_page = 0;                 // 0 = settings, 1 = key bindings
int g_capture = -1;             // binding being captured
bool g_capture_add = false;     // add a key instead of replacing
bool g_capture_swallow = false; // the click that started the capture is still going
int g_hover_bind = -1;
RectF g_bind_rect[kBindCount];

std::vector<std::string> SplitKeys(const std::string& list) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos <= list.size()) {
    const size_t comma = list.find(',', pos);
    std::string k = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    while (!k.empty() && k.front() == ' ') k.erase(k.begin());
    while (!k.empty() && k.back() == ' ') k.pop_back();
    if (!k.empty()) out.push_back(k);
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

std::string JoinKeys(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& k : v) {
    if (!s.empty()) s += ",";
    s += k;
  }
  return s;
}

std::wstring FriendlyKey(const std::string& k) {
  static const struct { const char* n; const wchar_t* f; } kMap[] = {
      {"SpaceBar", L"Space"}, {"LeftShift", L"Left Shift"}, {"RightShift", L"Right Shift"},
      {"LeftControl", L"Left Ctrl"}, {"RightControl", L"Right Ctrl"}, {"LeftAlt", L"Left Alt"},
      {"RightAlt", L"Right Alt"}, {"LeftMouseButton", L"Left click"},
      {"RightMouseButton", L"Right click"}, {"MiddleMouseButton", L"Middle click"},
      {"ThumbMouseButton", L"Mouse 4"}, {"ThumbMouseButton2", L"Mouse 5"},
      {"MouseScrollUp", L"Wheel up"}, {"MouseScrollDown", L"Wheel down"}, {"One", L"1"}, {"Two", L"2"},
      {"Three", L"3"}, {"Four", L"4"}, {"Five", L"5"}, {"Six", L"6"}, {"Seven", L"7"},
      {"Eight", L"8"}, {"Nine", L"9"}, {"Zero", L"0"}, {"BackSpace", L"Backspace"},
      {"Slash", L"/"}, {"Backslash", L"\\"}, {"Comma", L","}, {"Period", L"."}, {"Semicolon", L";"},
      {"Quote", L"'"}, {"LeftBracket", L"["}, {"RightBracket", L"]"}, {"Equals", L"="},
      {"Underscore", L"-"}, {"PageUp", L"Page Up"}, {"PageDown", L"Page Down"},
  };
  for (const auto& m : kMap)
    if (k == m.n) return m.f;
  return std::wstring(k.begin(), k.end());
}

std::wstring BindText(int i) {
  if (g_capture == i) return g_capture_add ? L"Press a key to add..." : L"Press a key or mouse button...";
  std::wstring s;
  for (const auto& k : g_bind_keys[i]) {
    if (!s.empty()) s += L"   ";
    s += FriendlyKey(k);
  }
  return s.empty() ? L"(none)" : s;
}

void LoadBinds() {
  for (int i = 0; i < kBindCount; ++i) {
    std::string v;
    g_bind_keys[i] = SplitKeys(GetValue(kBinds[i].toml, &v) && !v.empty() ? v : std::string(kBinds[i].def));
  }
  g_bind_dirty = false;
}

void ApplyBinds() {
  if (!g_bind_dirty) return;
  for (int i = 0; i < kBindCount; ++i) {
    const std::string now = JoinKeys(g_bind_keys[i]);
    if (now == kBinds[i].def) RemoveKey(kBinds[i].toml);
    else SetValue(kBinds[i].toml, "\"" + now + "\"");
  }
  g_bind_dirty = false;
}

// Gives `key` to binding `i` (replace or add); a key can only do one thing, so it is taken away
// from any other action that had it.
void AssignKey(int i, const std::string& key, bool add) {
  for (int j = 0; j < kBindCount; ++j) {
    if (j == i) continue;
    auto& v = g_bind_keys[j];
    v.erase(std::remove(v.begin(), v.end(), key), v.end());
  }
  auto& mine = g_bind_keys[i];
  if (!add) mine.clear();
  if (std::find(mine.begin(), mine.end(), key) == mine.end()) mine.push_back(key);
  g_bind_dirty = true;
}

// Windows key message -> engine key name ("" if not offered). Left/right modifiers are told apart
// from the scan code and the extended-key bit.
std::string KeyFromMessage(WPARAM w, LPARAM l) {
  int vk = int(w);
  const int scan = (l >> 16) & 0xFF;
  const bool ext = ((l >> 24) & 1) != 0;
  if (vk == VK_SHIFT) vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
  else if (vk == VK_CONTROL) vk = ext ? VK_RCONTROL : VK_LCONTROL;
  else if (vk == VK_MENU) vk = ext ? VK_RMENU : VK_LMENU;
  for (const rex::glue::KeyName& k : rex::glue::kKeyTable)
    if (k.vk == vk) return k.name;
  return {};
}

// Game language choices, in the order of the Game language row. The console had no language id for
// Czech, Hungarian, Danish, Dutch, Norwegian and Finnish: the game takes those from the country.
// Spanish needs the Mexican country for the Latin American text that ships with the game.
struct LangChoice { const char* lang; const char* country; };
const LangChoice kLangs[] = {{"1", ""},  {"4", ""},  {"5", "71"}, {"7", ""},  {"8", ""},  {"11", ""},
                             {"1", "23"}, {"1", "42"}, {"1", "25"}, {"1", "74"}, {"1", "75"}, {"1", "32"}};
constexpr int kLangCount = int(sizeof(kLangs) / sizeof(kLangs[0]));

void InitControls() {
  g_c[kQuality] = {kQuality, Kind::Segmented, L"Quality level",
                   L"Picking a level sets resolution, texture filtering and SMAA below to match it. Auto picks from your graphics card and lowers itself if 60 fps is not held. Low: no SMAA, 4x, and lighter game settings (see below). Medium: SMAA, 8x. High: SMAA, 16x. The game renders at 720p on every level."};
  g_c[kLowEnd] = {kLowEnd, Kind::Toggle, L"Low level: lighter game settings",
                  L"Only used on the Low level. Switches off the game's dynamic lights, shadows, decals, bloom, depth of field, ambient occlusion and other post effects to run on weak graphics cards and handhelds. It is applied to a copy of your own game file in the cache; your game folder is never changed. Medium and High are not affected."};
  g_c[kQuality].items = {L"Auto", L"Low", L"Medium", L"High"};
  g_c[kResolution] = {kResolution, Kind::Segmented, L"Internal resolution (experimental)",
                      L"The resolution the game renders at before it is scaled to your screen. 720p is the default. 1440p is experimental and very demanding: it needs a fast graphics card, can drop below 60 fps in heavy scenes, and may show small visual glitches."};
  g_c[kResolution].items = {L"Auto", L"720p", L"1440p (demanding)"};
  g_c[kFiltering] = {kFiltering, Kind::Segmented, L"Texture filtering",
                     L"Keeps ground and walls sharp at a slant. Auto follows the quality level."};
  g_c[kFiltering].items = {L"Auto", L"4x", L"8x", L"16x"};
  g_c[kUpscaler] = {kUpscaler, Kind::Segmented, L"Upscaler",
                    L"Renders the scene below your screen's resolution and reconstructs a sharp picture from several frames. DLSS needs an NVIDIA RTX card. FSR 3.1 works on any graphics card. FSR 4 is for Radeon RX 9000 cards; other cards use FSR 3.1. The frame size, anti-aliasing and scaling are then done by the upscaler, so those options disappear."};
  g_c[kUpscaler].items = {L"Off", L"DLSS", L"FSR 3.1", L"FSR 4"};
  g_c[kUpscaleMode] = {kUpscaleMode, Kind::Segmented, L"Upscale mode",
                       L"How big the game's picture is before the upscaler enlarges it to your screen. Quality draws it at 1440p, Performance at 1080p, Ultra Performance at 720p; smaller is faster. Auto follows the quality level: Low uses Ultra Performance, Medium Performance, High Quality."};
  g_c[kUpscaleMode].items = {L"Auto", L"Quality", L"Performance", L"Ultra Perf."};
  g_c[kFullscreen] = {kFullscreen, Kind::Toggle, L"Fullscreen", L"Borderless fullscreen. Turn off to play in a window."};
  g_c[kSmaa] = {kSmaa, Kind::Segmented, L"Anti-aliasing (SMAA)",
                L"Smooths jagged edges in the final image. Auto turns it on except on the Low level. Not used while DLSS or FSR 3.1 is on, because the upscaler already does this."};
  g_c[kSmaa].items = {L"Auto", L"On", L"Off"};
  g_c[kFsr] = {kFsr, Kind::Toggle, L"Spatial upscaling (FSR 1)", L"For when the Upscaler is Off: AMD FidelityFX Super Resolution 1.0 scales the 720p or 1440p image to your screen. Off uses a plain stretch. Not used with DLSS or FSR 3.1, which do their own upscaling."};
  g_c[kSharpness] = {kSharpness, Kind::Slider, L"Sharpness", L"How much the final image is sharpened, in every mode. Lower is sharper, higher is softer."};
  g_c[kSharpness].vmin = 0; g_c[kSharpness].vmax = 2; g_c[kSharpness].step = 0.1;
  g_c[kGrain] = {kGrain, Kind::Toggle, L"Film grain", L"The game's animated film grain. Off gives a calmer, sharper image."};
  g_c[kBlur] = {kBlur, Kind::Toggle, L"Motion blur", L"The game's motion blur. Off is the default."};
  g_c[kLimit] = {kLimit, Kind::Segmented, L"Frame limit",
                 L"A steady limit looks smoother than a fast but uneven frame rate. Unlocked also turns vsync off, so frames are not held to the display refresh. In the game, F8 switches between this limit and unlocked."};
  g_c[kLimit].items = {L"30", L"60", L"90", L"120", L"Unlocked"};
  g_c[kVrr] = {kVrr, Kind::Toggle, L"Variable refresh rate", L"For G-Sync and FreeSync monitors. Turn on when the frame limit is Unlocked."};
  g_c[kSens] = {kSens, Kind::Slider, L"Mouse sensitivity", L"How far the camera turns per mouse movement. In the game, F11 and F12 lower and raise it, and Insert releases the mouse."};
  g_c[kSens].vmin = 25; g_c[kSens].vmax = 300; g_c[kSens].step = 5; g_c[kSens].unit = L"%";
  g_c[kInvert] = {kInvert, Kind::Toggle, L"Invert vertical look", L"Moving the mouse up looks down."};
  g_c[kPrompts] = {kPrompts, Kind::Segmented, L"Menu prompts",
                   L"What the buttons in menus are called. Auto shows keyboard names like [Enter] unless a controller is connected when the game starts. The small button pictures drawn by some in-game hints stay as they are."};
  g_c[kPrompts].items = {L"Auto", L"Keyboard", L"Controller"};
  g_c[kDrawDist] = {kDrawDist, Kind::Segmented, L"Draw distance",
                    L"How far away the game still draws objects. Auto follows the quality level: Low draws half as far and Medium three quarters, which is a large speed-up on slow PCs (Low gained about 50% more frames in the test scene); High uses the game's own distance. Short is the Low setting; Far draws 50% farther than the game normally does."};
  g_c[kDrawDist].items = {L"Auto", L"Short", L"Normal", L"Far"};
  g_c[kFov] = {kFov, Kind::Slider, L"Field of view", L"How wide the everyday camera sees, in degrees. The game's own value is 80. Aiming keeps its own zoom."};
  g_c[kFov].vmin = 60; g_c[kFov].vmax = 120; g_c[kFov].step = 5; g_c[kFov].unit = L"deg";
  g_c[kLanguage] = {kLanguage, Kind::Segmented, L"Game language",
                    L"The language of the game's text and menus. Every language that came with your game files is listed (German, Italian, Portuguese, Russian and Japanese were not on the disc this was made from). Takes effect the next time the game starts."};
  g_c[kLanguage].items = {L"English", L"French", L"Spanish", L"Korean", L"Chinese", L"Polish"};
  g_c[kLanguage].rows = 2;
  g_c[kLanguage].items.insert(g_c[kLanguage].items.end(), {L"Czech", L"Hungarian", L"Danish", L"Dutch", L"Norwegian", L"Finnish"});
  g_c[kSkipLogos] = {kSkipLogos, Kind::Toggle, L"Skip startup logo movie",
                     L"Skips the logo movie that plays when the game starts, which saves about 15 seconds per launch."};
  g_c[kShake] = {kShake, Kind::Toggle, L"Roadie-run camera shake", L"The shaking of the camera while sprinting. Off holds it steady."};

  {
    auto has = [](const wchar_t* f) { return GetFileAttributesW((g_dir + L"\\" + f).c_str()) != INVALID_FILE_ATTRIBUTES; };
    static std::wstring note;
    note = std::wstring(L"DLSS: ") + (has(L"nvngx_dlss.dll") ? L"nvngx_dlss.dll found" : L"nvngx_dlss.dll not found") +
           L".  FSR: " +
           (has(L"amd_fidelityfx_loader_dx12.dll") && has(L"amd_fidelityfx_upscaler_dx12.dll") ? L"AMD DLLs found" : L"AMD DLLs not found") +
           L". The overlay (F9) shows what is running.";
    // Two columns of about the same height, so no card has to stretch: picture settings on the left,
    // frame rate, input and comfort on the right.
    g_cards = {
        {L"QUALITY", {kQuality, kResolution, kFiltering, kDrawDist, kLowEnd, kFullscreen, kSkipLogos}, 0},
        {L"IMAGE", {kSmaa, kFsr, kSharpness, kGrain, kBlur}, 0},
        {L"FRAME RATE", {kLimit, kVrr}, 1, L"In the game, F8 lifts and restores this limit, and F9 shows an FPS counter. Choose Unlocked here to also turn vsync off."},
        {L"MOUSE", {kSens, kInvert}, 1},
        {L"COMFORT AND PROMPTS", {kLanguage, kPrompts, kFov, kShake}, 1},
    };
  }
}

// config -> controls
void LoadControls() {
  struct ForceUpscalerOff { ~ForceUpscalerOff() { g_c[kUpscaler].sel = 0; g_c[kUpscaleMode].sel = 0; } } force_off;  // upscalers are not part of this release
  std::string v;
  auto& q = g_c[kQuality];
  q.sel = 0;
  if (GetValue("gowj_quality", &v)) q.sel = v == "low" ? 1 : v == "medium" ? 2 : v == "high" ? 3 : 0;

  auto& r = g_c[kResolution];
  r.sel = 0;
  if (GetValue("draw_resolution_scale_x", &v)) r.sel = v == "1" ? 1 : v == "2" ? 2 : -1;

  auto& f = g_c[kFiltering];
  f.sel = 0;
  if (GetValue("anisotropic_override", &v)) f.sel = v == "3" ? 1 : v == "4" ? 2 : v == "5" ? 3 : -1;

  g_c[kFullscreen].on = !GetValue("fullscreen", &v) || v != "false";
  g_c[kLowEnd].on = !GetValue("gowj_low_end_tweaks", &v) || v != "false";

  auto& up = g_c[kUpscaler];
  up.sel = 0;
  if (GetValue("gowj_upscaler", &v)) up.sel = v == "dlss" ? 1 : v == "fsr3" ? 2 : v == "fsr4" ? 3 : 0;
  auto& um = g_c[kUpscaleMode];
  um.sel = 0;  // Auto when nothing is written
  if (GetValue("gowj_upscale_quality", &v)) {
    const int q = atoi(v.c_str());
    um.sel = q < 0 ? 0 : q == 0 ? 2 : q == 3 ? 3 : 1;  // anything else (older settings) is Quality
  }

  auto& s = g_c[kSmaa];
  s.sel = 0;
  if (GetValue("gowj_smaa", &v)) s.sel = v == "true" ? 1 : v == "false" ? 2 : 0;

  g_c[kFsr].on = !GetValue("gowj_fsr", &v) || v != "false";
  g_c[kSharpness].val = GetValue("gowj_fsr_sharpness", &v) ? atof(v.c_str()) : 0.5;
  g_c[kGrain].on = GetValue("gowj_film_grain", &v) && v == "true";
  g_c[kBlur].on = GetValue("gowj_motion_blur", &v) && v == "true";

  auto& l = g_c[kLimit];
  int hz = 60;
  if (GetValue("gowj_pace_hz", &v)) hz = atoi(v.c_str());
  l.sel = hz == 30 ? 0 : hz == 60 ? 1 : hz == 90 ? 2 : hz == 120 ? 3 : hz == 0 ? 4 : -1;
  g_c[kVrr].on = GetValue("d3d12_allow_variable_refresh_rate_and_tearing", &v) && v == "true";

  const double sens = GetValue("mnk_raw_look_scale", &v) ? atof(v.c_str()) : 0.003;
  g_c[kSens].val = std::clamp(std::round(sens / 0.003 * 100.0 / 5.0) * 5.0, 25.0, 300.0);
  g_c[kInvert].on = GetValue("mnk_invert_y", &v) && v == "true";
  {
    auto& dd = g_c[kDrawDist];
    dd.sel = 0;
    if (GetValue("gowj_draw_distance", &v) && v != "auto") {
      const double f = atof(v.c_str());
      dd.sel = f < 0.8 ? 1 : f <= 1.2 ? 2 : 3;
    }
  }
  g_c[kPrompts].sel = GetValue("gowj_prompts", &v) ? (v == "keyboard" ? 1 : v == "controller" ? 2 : 0) : 0;
  g_c[kFov].val = GetValue("gowj_fov", &v) && atoi(v.c_str()) > 0 ? atoi(v.c_str()) : 80;
  g_c[kShake].on = !(GetValue("gowj_no_camera_shake", &v) && v == "true");
  g_c[kSkipLogos].on = GetValue("gowj_skip_logos", &v) && v == "true";
  {
    // The game picks its text from the console language id (user_language) and, for the languages the
    // console had no id for, from the country (user_country): see kLangs.
    g_c[kLanguage].sel = 0;
    std::string lang = "1", country;
    GetValue("user_language", &lang);
    GetValue("user_country", &country);
    if (lang == "10") lang = "8";  // the other Chinese id
    for (int k = 1; k < kLangCount; ++k) {
      if (lang == kLangs[k].lang && (!kLangs[k].country[0] || country == kLangs[k].country)) g_c[kLanguage].sel = k;
    }
  }

  LoadBinds();
  for (auto& c : g_c) c.dirty = false;
}

// controls -> config (only what was changed)
void ApplyControls() {
  auto b = [](bool on) { return std::string(on ? "true" : "false"); };
  for (auto& c : g_c) {
    if (!c.dirty) continue;
    switch (c.id) {
      case kQuality: {
        static const char* n[] = {"auto", "low", "medium", "high"};
        if (c.sel >= 0) SetValue("gowj_quality", std::string("\"") + n[c.sel] + "\"");
      } break;
      case kResolution:
        if (c.sel == 0) { RemoveKey("draw_resolution_scale_x"); RemoveKey("draw_resolution_scale_y"); }
        else if (c.sel > 0) {
          SetValue("draw_resolution_scale_x", std::to_string(c.sel));
          SetValue("draw_resolution_scale_y", std::to_string(c.sel));
        }
        break;
      case kFiltering:
        if (c.sel == 0) RemoveKey("anisotropic_override");
        else if (c.sel > 0) SetValue("anisotropic_override", std::to_string(c.sel + 2));
        break;
      case kFullscreen: SetValue("fullscreen", b(c.on)); break;
      case kLowEnd: SetValue("gowj_low_end_tweaks", b(c.on)); break;
      case kUpscaler: {
        static const char* n[] = {"off", "dlss", "fsr3", "fsr4"};
        if (c.sel >= 0) SetValue("gowj_upscaler", std::string("\"") + n[c.sel] + "\"");
      } break;
      case kUpscaleMode: {
        static const int q[] = {-1, 2, 0, 3};
        if (c.sel >= 0) SetValue("gowj_upscale_quality", std::to_string(q[c.sel]));
      } break;
      case kSmaa:
        if (c.sel == 0) RemoveKey("gowj_smaa");
        else if (c.sel > 0) SetValue("gowj_smaa", b(c.sel == 1));
        break;
      case kFsr: SetValue("gowj_fsr", b(c.on)); break;
      case kSharpness: SetValue("gowj_fsr_sharpness", Num(c.val, 1)); break;
      case kGrain: SetValue("gowj_film_grain", b(c.on)); break;
      case kBlur: SetValue("gowj_motion_blur", b(c.on)); break;
      case kLimit: {
        static const int hz[] = {30, 60, 90, 120, 0};
        if (c.sel >= 0) SetValue("gowj_pace_hz", std::to_string(hz[c.sel]));
        // Unlimited also has to leave vsync, which otherwise gates presents at half the refresh rate.
        if (c.sel >= 0) SetValue("vsync", hz[c.sel] == 0 ? "false" : "true");
      } break;
      case kVrr: SetValue("d3d12_allow_variable_refresh_rate_and_tearing", b(c.on)); break;
      case kSens: SetValue("mnk_raw_look_scale", Num(0.003 * c.val / 100.0, 5)); break;
      case kInvert: SetValue("mnk_invert_y", b(c.on)); break;
      case kPrompts: {
        static const char* n[] = {"auto", "keyboard", "controller"};
        if (c.sel >= 0) SetValue("gowj_prompts", std::string("\"") + n[c.sel] + "\"");
      } break;
      case kFov:
        if (int(c.val) == 80) RemoveKey("gowj_fov");
        else SetValue("gowj_fov", std::to_string(int(c.val)));
        break;
      case kShake: SetValue("gowj_no_camera_shake", b(!c.on)); break;
      case kSkipLogos: SetValue("gowj_skip_logos", b(c.on)); break;
      case kLanguage: {
        if (c.sel <= 0 || c.sel >= kLangCount) { RemoveKey("user_language"); RemoveKey("user_country"); }
        else {
          SetValue("user_language", kLangs[c.sel].lang);
          if (kLangs[c.sel].country[0]) SetValue("user_country", kLangs[c.sel].country);
          else RemoveKey("user_country");
        }
      } break;
      case kDrawDist: {
        static const char* n[] = {"auto", "0.5", "1.0", "1.5"};
        if (c.sel == 0) RemoveKey("gowj_draw_distance");
        else if (c.sel > 0) SetValue("gowj_draw_distance", std::string("\"") + n[c.sel] + "\"");
      } break;
      default: break;
    }
    c.dirty = false;
  }
  ApplyBinds();
}

// A quality level is a bundle: picking one also sets resolution, texture filtering and SMAA so the
// window shows exactly what will be applied (Auto clears them and lets the game choose).
void ApplyPreset(int level) {
  struct Preset { int res, filt, smaa; };
  static const Preset kPresets[4] = {{0, 0, 0}, {1, 1, 2}, {1, 2, 1}, {1, 3, 1}};
  if (level < 0 || level > 3) return;
  g_c[kQuality].sel = level;
  g_c[kQuality].dirty = true;
  const Preset& p = kPresets[level];
  g_c[kResolution].sel = p.res;
  g_c[kResolution].dirty = true;
  g_c[kFiltering].sel = p.filt;
  g_c[kFiltering].dirty = true;
  g_c[kSmaa].sel = p.smaa;
  g_c[kSmaa].dirty = true;
}

// config\cvars.txt is rewritten by the game on every start with the values it really applied
// (columns: category, name, type, source, default, current, ...). It is what the "last start"
// labels are read from, so the window never has to guess what Auto turned into.
void LoadLastStart() {
  std::string text;
  if (!ReadTextFile(g_dir + L"\\config\\cvars.txt", text)) return;
  auto field = [&](const std::string& line, int idx) {
    size_t start = 0;
    for (int i = 0; i < idx; ++i) {
      start = line.find(char(9), start);
      if (start == std::string::npos) return std::string();
      ++start;
    }
    size_t end = line.find(char(9), start);
    return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
  };
  std::string scale, aniso, smaa, hz;
  for (const auto& l : SplitLines(text)) {
    const std::string name = field(l, 1);
    if (name == "draw_resolution_scale_x") scale = field(l, 5);
    else if (name == "anisotropic_override") aniso = field(l, 5);
    else if (name == "gowj_smaa") smaa = field(l, 5);
    else if (name == "gowj_pace_hz") hz = field(l, 5);
  }
  if (scale == "1") g_c[kResolution].last = L"last start: 1x 720p";
  else if (scale == "2") g_c[kResolution].last = L"last start: 2x 1440p";
  else if (!scale.empty()) g_c[kResolution].last = L"last start: " + Widen(scale) + L"x";
  if (aniso == "3") g_c[kFiltering].last = L"last start: 4x";
  else if (aniso == "4") g_c[kFiltering].last = L"last start: 8x";
  else if (aniso == "5") g_c[kFiltering].last = L"last start: 16x";
  else if (aniso == "-1" || aniso == "0") g_c[kFiltering].last = L"last start: game default";
  if (smaa == "true") g_c[kSmaa].last = L"last start: on";
  else if (smaa == "false") g_c[kSmaa].last = L"last start: off";
  if (hz == "0") g_c[kLimit].last = L"last start: unlocked";
  else if (!hz.empty()) g_c[kLimit].last = L"last start: " + Widen(hz) + L" fps";
}

// ------------------------------------------------------------------------------------------
// look
// ------------------------------------------------------------------------------------------
const Color kBg(255, 16, 18, 22), kCard(255, 24, 27, 33), kCardLine(255, 38, 42, 51),
    kText(255, 232, 234, 237), kMuted(255, 139, 147, 161), kTrack(255, 33, 37, 45),
    kTrackHover(255, 45, 50, 60), kAccent(255, 224, 64, 38), kAccentHover(255, 240, 84, 58),
    kOff(255, 63, 69, 82), kWhite(255, 245, 246, 248), kOk(255, 90, 200, 120), kWarn(255, 240, 170, 60);

constexpr float kMinW = 900.f, kMaxW = 6000.f, kMargin = 24.f, kGap = 18.f, kHeaderH = 92.f, kFooterH = 96.f;
constexpr float kPad = 22.f, kTitleH = 44.f;

HWND g_hwnd = nullptr;
float g_scale = 1.f;
float g_win_w = 1000.f;  // logical width; the window can be widened (never made taller)
int g_hover_ctl = -1, g_hover_seg = -1, g_hover_btn = -1, g_drag = -1, g_press_btn = -1;
bool g_changed = false, g_game_found = false;
std::wstring g_status;
Color g_status_color = kMuted;
RectF g_btn[4];  // key bindings, reset, save, play
const wchar_t* kBtnText[4] = {L"Key bindings", L"Reset to defaults", L"Save", L"Play"};
float g_content_h = 0;
float g_min_h = 0;   // height that fits the content
float g_user_h = 0;  // height the player dragged the window to (0 = natural)

// With DLSS or FSR on, the upscaler sets the frame size and does the anti-aliasing and scaling, so
// these three controls would only confuse: they are removed (and the window shrinks) instead.
// The lighter-settings switch only applies to the Low level, so it is shown only while Low is selected.
bool Hidden(int id) {
  if (id == kLowEnd && g_c[kQuality].sel != 1) return true;
  return g_c[kUpscaler].sel > 0 && (id == kSmaa || id == kFsr);
}

float RowHeight(const Ctl& c) { return c.kind == Kind::Toggle ? 46.f : 70.f + 36.f * float(c.rows - 1); }

void Layout() {
  const float colw = (g_win_w - 2 * kMargin - kGap) / 2.f;
  float y[2] = {kHeaderH, kHeaderH};
  for (auto& card : g_cards) {
    float h = kTitleH + kPad * 0.4f;
    for (int i : card.ctls) h += Hidden(i) ? 0.f : RowHeight(g_c[i]);
    h += kPad * 0.6f;
    if (card.note) h += 46.f;
    card.rect = RectF(kMargin + card.column * (colw + kGap), y[card.column], colw, h);
    float cy = y[card.column] + kTitleH + kPad * 0.4f;
    for (int i : card.ctls) {
      Ctl& c = g_c[i];
      if (Hidden(i)) { c.row = c.ctl = RectF(-100, -100, 0, 0); continue; }
      const float rh = RowHeight(c);
      c.row = RectF(card.rect.X + kPad, cy, colw - 2 * kPad, rh);
      if (c.kind == Kind::Toggle) {
        c.ctl = RectF(c.row.X + c.row.Width - 46.f, cy + 10.f, 46.f, 26.f);
      } else if (c.kind == Kind::Segmented) {
        c.ctl = RectF(c.row.X, cy + 26.f, c.row.Width, 34.f + 36.f * float(c.rows - 1));
      } else {
        c.ctl = RectF(c.row.X, cy + 26.f, c.row.Width - 64.f, 34.f);
      }
      cy += rh;
    }
    y[card.column] += h + kGap;
  }
  // Both columns end at the same height: the last card of the shorter column takes the difference.
  float bottom = (std::max)(y[0], y[1]) - kGap;
  g_min_h = bottom + kGap + kFooterH;
  if (g_user_h > g_min_h) bottom += g_user_h - g_min_h;
  for (int col = 0; col < 2; ++col) {
    for (int k = int(g_cards.size()) - 1; k >= 0; --k) {
      if (g_cards[k].column != col) continue;
      g_cards[k].rect.Height += bottom - (y[col] - kGap);
      break;
    }
  }
  const float content = bottom + kGap;
  g_content_h = content;
  const float bar_y = content + 2.f;
  const float bw[4] = {150.f, 160.f, 110.f, 130.f};
  float x = g_win_w - kMargin;
  for (int i = 3; i >= 0; --i) {
    x -= bw[i];
    g_btn[i] = RectF(x, bar_y + 14.f, bw[i], 44.f);
    x -= 12.f;
  }
  // key bindings page: two columns of rows inside the same content area
  {
    const float colw2 = (g_win_w - 2 * kMargin - kGap) / 2.f;
    const int half = (kBindCount + 1) / 2;
    const float top = kHeaderH + 46.f;
    const float rh = (content - top - kGap - 26.f) / float(half);
    for (int i = 0; i < kBindCount; ++i) {
      const int col = i / half, row = i % half;
      g_bind_rect[i] = RectF(kMargin + col * (colw2 + kGap), top + row * rh, colw2, rh - 6.f);
    }
  }
}

float TotalHeight() { return g_content_h + kFooterH; }

GraphicsPath* Round(const RectF& r, float rad) {
  auto* p = new GraphicsPath();
  const float d = rad * 2;
  p->AddArc(r.X, r.Y, d, d, 180, 90);
  p->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  p->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  p->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  p->CloseFigure();
  return p;
}

void FillRound(Graphics& g, const RectF& r, float rad, const Color& c) {
  SolidBrush br(c);
  GraphicsPath* p = Round(r, rad);
  g.FillPath(&br, p);
  delete p;
}

void StrokeRound(Graphics& g, const RectF& r, float rad, const Color& c, float w = 1.f) {
  Pen pen(c, w);
  GraphicsPath* p = Round(r, rad);
  g.DrawPath(&pen, p);
  delete p;
}

void Text(Graphics& g, const std::wstring& s, const Font& f, const RectF& r, const Color& c,
          StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter,
          bool wrap = false, bool ellipsis = true) {
  StringFormat fmt;
  fmt.SetAlignment(h);
  fmt.SetLineAlignment(v);
  if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
  fmt.SetTrimming(ellipsis ? StringTrimmingEllipsisCharacter : StringTrimmingNone);
  SolidBrush br(c);
  g.DrawString(s.c_str(), -1, &f, r, &fmt, &br);
}

RectF Scaled(const RectF& r) { return RectF(r.X * g_scale, r.Y * g_scale, r.Width * g_scale, r.Height * g_scale); }

std::wstring SliderText(const Ctl& c) {
  wchar_t b[32];
  if (c.id == kSens) swprintf(b, 32, L"%d%%", int(c.val));
  else if (c.id == kFov) swprintf(b, 32, L"%d deg", int(c.val));
  else swprintf(b, 32, L"%.1f", c.val);
  return b;
}

float SegWidth(const Ctl& c) { return c.ctl.Width / float(c.items.size()); }

void Paint(HDC hdc, int w, int h) {
  Graphics g(hdc);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
  g.Clear(kBg);
  FontFamily fam(L"Segoe UI");
  const float S = g_scale;
  Font fTitle(&fam, 22 * S, FontStyleBold, UnitPixel), fSub(&fam, 13 * S, FontStyleRegular, UnitPixel),
      fCard(&fam, 12 * S, FontStyleBold, UnitPixel), fLabel(&fam, 14 * S, FontStyleRegular, UnitPixel),
      fSeg(&fam, 13 * S, FontStyleRegular, UnitPixel), fSegBold(&fam, 13 * S, FontStyleBold, UnitPixel),
      fHint(&fam, 12.5f * S, FontStyleRegular, UnitPixel), fBtn(&fam, 14 * S, FontStyleBold, UnitPixel);

  // header
  FillRound(g, Scaled(RectF(kMargin, 26, 8, 40)), 3 * S, kAccent);
  Text(g, L"GEARS OF WAR: JUDGMENT", fTitle, Scaled(RectF(kMargin + 22, 20, 520, 32)), kText);
  Text(g, L"PC  -  graphics and controls", fSub, Scaled(RectF(kMargin + 22, 50, 520, 22)), kMuted);
  {
    const wchar_t* chip = g_game_found ? L"Game found" : L"GearsOfWarJudgment.exe not found next to this program";
    // Sized from the measured text, so it fits at any display scale.
    StringFormat measure(StringFormat::GenericTypographic());
    measure.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF box;
    g.MeasureString(chip, -1, &fSeg, PointF(0, 0), &measure, &box);
    const float cw = box.Width / S + 52.f;  // dot + padding on both sides
    RectF cr = Scaled(RectF(g_win_w - kMargin - cw, 32, cw, 30));
    FillRound(g, cr, 15 * S, g_game_found ? Color(255, 28, 52, 38) : Color(255, 60, 40, 28));
    SolidBrush dot(g_game_found ? kOk : kWarn);
    g.FillEllipse(&dot, cr.X + 13 * S, cr.Y + 11 * S, 8 * S, 8 * S);
    Text(g, chip, fSeg, RectF(cr.X + 30 * S, cr.Y, cr.Width - 30 * S, cr.Height), g_game_found ? kOk : kWarn,
         StringAlignmentNear, StringAlignmentCenter, false, false);
  }

  if (g_page == 1) {
    Text(g, L"KEY BINDINGS", fCard, Scaled(RectF(kMargin, kHeaderH + 8, 400, 26)), kMuted);
    Text(g, L"Click an action, then press the key or mouse button. Right-click an action to add a second key. Esc cancels, Delete clears.",
         fHint, Scaled(RectF(kMargin + 150, kHeaderH + 8, g_win_w - 2 * kMargin - 150, 26)), kMuted);
    for (int i = 0; i < kBindCount; ++i) {
      RectF r = Scaled(g_bind_rect[i]);
      const bool hot = g_hover_bind == i, cap = g_capture == i;
      FillRound(g, r, 10 * S, cap ? Color(255, 58, 38, 34) : (hot ? kTrackHover : kCard));
      StrokeRound(g, r, 10 * S, cap ? kAccent : kCardLine);
      Text(g, kBinds[i].label, fLabel, RectF(r.X + 14 * S, r.Y, r.Width * 0.5f - 14 * S, r.Height), kText);
      Text(g, BindText(i), fSegBold, RectF(r.X + r.Width * 0.5f, r.Y, r.Width * 0.5f - 14 * S, r.Height),
           cap ? kAccentHover : kWhite, StringAlignmentFar);
    }
  }
  // cards
  for (auto& card : g_cards) {
    if (g_page == 1) break;
    RectF cr = Scaled(card.rect);
    FillRound(g, cr, 12 * S, kCard);
    StrokeRound(g, cr, 12 * S, kCardLine);
    Text(g, card.title, fCard, RectF(cr.X + kPad * S, cr.Y + 6 * S, cr.Width - 2 * kPad * S, (kTitleH - 6) * S), kMuted);
    if (card.note) {
      Text(g, card.note, fHint, RectF(cr.X + kPad * S, cr.Y + cr.Height - 54 * S, cr.Width - 2 * kPad * S, 44 * S),
           kMuted, StringAlignmentNear, StringAlignmentFar, true);
    }
    for (int i : card.ctls) {
      if (Hidden(i)) continue;
      Ctl& c = g_c[i];
      const RectF row = Scaled(c.row), ct = Scaled(c.ctl);
      const bool hot = g_hover_ctl == i;
      if (c.kind == Kind::Toggle) {
        Text(g, c.label, fLabel, RectF(row.X, row.Y + 2 * S, row.Width - 60 * S, 40 * S), kText);
        FillRound(g, ct, ct.Height / 2, c.on ? (hot ? kAccentHover : kAccent) : (hot ? Color(255, 80, 87, 102) : kOff));
        SolidBrush knob(kWhite);
        const float d = ct.Height - 6 * S;
        g.FillEllipse(&knob, c.on ? ct.X + ct.Width - d - 3 * S : ct.X + 3 * S, ct.Y + 3 * S, d, d);
      } else {
        Text(g, c.label, fLabel, RectF(row.X, row.Y + 2 * S, row.Width, 22 * S), kText);
        if (!c.last.empty()) {
          Text(g, c.last, fHint, RectF(row.X, row.Y + 2 * S, row.Width, 22 * S), kMuted, StringAlignmentFar);
        }
        if (c.kind == Kind::Segmented) {
          FillRound(g, ct, 9 * S, kTrack);
          const int per = (int(c.items.size()) + c.rows - 1) / c.rows;
          const float sw = ct.Width / float(per), sh = ct.Height / float(c.rows);
          for (size_t k = 0; k < c.items.size(); ++k) {
            RectF seg(ct.X + sw * float(int(k) % per) + 3 * S, ct.Y + sh * float(int(k) / per) + 3 * S, sw - 6 * S, sh - 6 * S);
            const bool sel = int(k) == c.sel;
            const bool over = hot && g_hover_seg == int(k);
            if (sel) FillRound(g, seg, 7 * S, over ? kAccentHover : kAccent);
            else if (over) FillRound(g, seg, 7 * S, kTrackHover);
            // A label that does not fit its segment gets a smaller font instead of being cut off.
            float px = 13.f;
            StringFormat mf;
            mf.SetFormatFlags(StringFormatFlagsNoWrap);
            for (; px > 9.f; px -= 0.5f) {
              Font ft(&fam, px * S, sel ? FontStyleBold : FontStyleRegular, UnitPixel);
              RectF bx;
              g.MeasureString(c.items[k].c_str(), -1, &ft, PointF(0, 0), &mf, &bx);
              if (bx.Width <= seg.Width - 8 * S) break;
            }
            Font fit(&fam, px * S, sel ? FontStyleBold : FontStyleRegular, UnitPixel);
            Text(g, c.items[k], fit, seg, sel ? kWhite : kMuted, StringAlignmentCenter);
          }
        } else {
          const float t = float((c.val - c.vmin) / (c.vmax - c.vmin));
          const float ty = ct.Y + ct.Height / 2;
          RectF track(ct.X, ty - 3 * S, ct.Width, 6 * S);
          FillRound(g, track, 3 * S, kTrack);
          FillRound(g, RectF(track.X, track.Y, track.Width * t, track.Height), 3 * S, hot || g_drag == i ? kAccentHover : kAccent);
          SolidBrush knob(kWhite);
          const float d = 18 * S;
          g.FillEllipse(&knob, ct.X + ct.Width * t - d / 2, ty - d / 2, d, d);
          Text(g, SliderText(c), fSegBold, RectF(ct.X + ct.Width + 8 * S, ct.Y, 60 * S, ct.Height), kText, StringAlignmentFar);
        }
      }
    }
  }

  // footer: hint / status, buttons
  const float fy = g_content_h * S;
  SolidBrush footer_brush(Color(255, 12, 13, 16));
  g.FillRectangle(&footer_brush, 0.f, fy, float(w), float(h) - fy);
  std::wstring msg = g_status;
  Color mc = g_status_color;
  if (g_hover_ctl >= 0) { msg = g_c[g_hover_ctl].hint; mc = kMuted; }
  else if (g_hover_btn == 0) { msg = g_page == 1 ? L"Back to the settings." : L"Choose the keys and mouse buttons for every action."; mc = kMuted; }
  else if (g_hover_btn == 1) { msg = L"Put every setting back to its default."; mc = kMuted; }
  else if (g_hover_btn == 3) { msg = L"Save and start the game."; mc = kMuted; }
  else if (g_page == 1 && g_hover_bind >= 0) { msg = L"Click, then press the new key. Right-click adds a second key. Keys can only do one thing, so a key used elsewhere moves here."; mc = kMuted; }
  {
    // The window cannot be resized, so a long hint shrinks until it fits instead of being cut off.
    const RectF fr(kMargin * S, fy + 8 * S, (g_btn[0].X - kMargin - 12) * S, (kFooterH - 16) * S);
    StringFormat mfmt;
    mfmt.SetTrimming(StringTrimmingNone);
    float px = 12.5f;
    for (; px > 9.f; px -= 0.5f) {
      Font ft(&fam, px * S, FontStyleRegular, UnitPixel);
      RectF bounds;
      g.MeasureString(msg.c_str(), -1, &ft, RectF(0, 0, fr.Width, 0), &mfmt, &bounds);
      if (bounds.Height <= fr.Height) break;
    }
    Font fit(&fam, px * S, FontStyleRegular, UnitPixel);
    Text(g, msg, fit, fr, mc, StringAlignmentNear, StringAlignmentCenter, true, false);
  }
  for (int i = 0; i < 4; ++i) {
    RectF br = Scaled(g_btn[i]);
    const bool over = g_hover_btn == i, down = g_press_btn == i && over;
    const bool disabled = i == 3 && !g_game_found;
    Color fill = i == 3 ? (disabled ? kOff : (down ? Color(255, 200, 52, 30) : over ? kAccentHover : kAccent))
                        : (i == 2 ? (down ? Color(255, 56, 62, 74) : over ? Color(255, 66, 73, 88) : Color(255, 50, 56, 68))
                                  : (over ? kTrackHover : kTrack));
    FillRound(g, br, 10 * S, fill);
    Text(g, i == 0 && g_page == 1 ? L"Back" : kBtnText[i], fBtn, br, disabled ? kMuted : kWhite, StringAlignmentCenter);
  }
}

// ------------------------------------------------------------------------------------------
// behaviour
// ------------------------------------------------------------------------------------------
void SetStatus(const wchar_t* s, const Color& c) { g_status = s; g_status_color = c; }

void MarkChanged() {
  g_changed = true;
  SetStatus(L"You have unsaved changes.", kWarn);
}

int HitControl(float x, float y, int* seg) {
  *seg = -1;
  if (g_page == 1) return -1;
  for (int i = 0; i < kCount; ++i) {
    if (Hidden(i)) continue;
    Ctl& c = g_c[i];
    RectF r = Scaled(c.row);
    if (x < r.X || x > r.X + r.Width || y < r.Y || y > r.Y + r.Height) continue;
    if (c.kind == Kind::Segmented) {
      RectF ct = Scaled(c.ctl);
      if (y >= ct.Y && y <= ct.Y + ct.Height && x >= ct.X && x <= ct.X + ct.Width) {
        const int per = (int(c.items.size()) + c.rows - 1) / c.rows;
        const int col = (std::min)(int((x - ct.X) / (ct.Width / per)), per - 1);
        const int rw = (std::min)(int((y - ct.Y) / (ct.Height / c.rows)), c.rows - 1);
        *seg = (std::min)(rw * per + col, int(c.items.size()) - 1);
      }
    }
    return i;
  }
  return -1;
}

int HitButton(float x, float y) {
  for (int i = 0; i < 4; ++i) {
    RectF r = Scaled(g_btn[i]);
    if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return i;
  }
  return -1;
}

int HitBind(float x, float y) {
  if (g_page != 1) return -1;
  for (int i = 0; i < kBindCount; ++i) {
    RectF r = Scaled(g_bind_rect[i]);
    if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return i;
  }
  return -1;
}

void SetSliderFromX(Ctl& c, float x) {
  RectF ct = Scaled(c.ctl);
  double t = std::clamp(double((x - ct.X) / ct.Width), 0.0, 1.0);
  double v = c.vmin + t * (c.vmax - c.vmin);
  v = std::round(v / c.step) * c.step;
  v = std::clamp(v, c.vmin, c.vmax);
  if (v != c.val) {
    c.val = v;
    c.dirty = true;
    MarkChanged();
  }
}

bool DoSave() {
  ApplyControls();
  if (!SaveConfig()) {
    SetStatus(L"Could not write config\\gowj.toml (is the folder writable?).", kWarn);
    return false;
  }
  g_changed = false;
  SetStatus(L"Saved. The game reads these settings the next time it starts.", kOk);
  return true;
}

void DoPlay() {
  if (!g_game_found) return;
  if (g_changed && !DoSave()) return;
  const std::wstring exe = g_dir + L"\\GearsOfWarJudgment.exe";
  ShellExecuteW(g_hwnd, L"open", exe.c_str(), nullptr, g_dir.c_str(), SW_SHOWNORMAL);
  PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

void DoReset() {
  if (MessageBoxW(g_hwnd, L"Put every setting back to its default? Your current settings file is kept as gowj.toml.bak.",
                  L"Reset to defaults", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  std::string old;
  if (ReadTextFile(g_cfg_path, old)) WriteTextFile(g_cfg_path + L".bak", old);
  g_lines = SplitLines(kDefaultConfig);
  SaveConfig();
  LoadControls();
  g_changed = false;
  SetStatus(L"Defaults restored.", kOk);
}

void Redraw() {
  if (!g_hwnd) return;
  Layout();  // controls may have appeared or disappeared; the window height follows the content
  RECT wr, cr;
  GetWindowRect(g_hwnd, &wr);
  GetClientRect(g_hwnd, &cr);
  const LONG want = LONG(TotalHeight() * g_scale);
  if (cr.bottom != want) {
    SetWindowPos(g_hwnd, nullptr, 0, 0, wr.right - wr.left, (wr.bottom - wr.top) - cr.bottom + want,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      const int cw = rc.right, ch = rc.bottom;
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
      HGDIOBJ old = SelectObject(mem, bmp);
      Paint(mem, cw, ch);
      BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      if (g_drag >= 0) { SetSliderFromX(g_c[g_drag], x); Redraw(); return 0; }
      int seg;
      const int c = HitControl(x, y, &seg);
      const int b = HitButton(x, y);
      const int hb = HitBind(x, y);
      if (c != g_hover_ctl || seg != g_hover_seg || b != g_hover_btn || hb != g_hover_bind) {
        g_hover_ctl = c; g_hover_seg = seg; g_hover_btn = b; g_hover_bind = hb;
        Redraw();
      }
      TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      g_hover_ctl = g_hover_seg = g_hover_btn = g_hover_bind = -1;
      Redraw();
      return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      if (g_page == 1 && g_capture >= 0) {
        const char* name = m == WM_RBUTTONDOWN ? "RightMouseButton" : m == WM_MBUTTONDOWN ? "MiddleMouseButton"
                           : (GET_XBUTTON_WPARAM(w) == XBUTTON1 ? "ThumbMouseButton" : "ThumbMouseButton2");
        AssignKey(g_capture, name, g_capture_add);
        g_capture = -1;
        MarkChanged();
        Redraw();
        return m == WM_XBUTTONDOWN ? TRUE : 0;
      }
      if (m == WM_RBUTTONDOWN) {
        const int hb = HitBind(x, y);
        if (hb >= 0) { g_capture = hb; g_capture_add = true; g_capture_swallow = false; Redraw(); }
      }
      return m == WM_XBUTTONDOWN ? TRUE : 0;
    }
    case WM_MOUSEWHEEL:
      if (g_page == 1 && g_capture >= 0) {
        AssignKey(g_capture, GET_WHEEL_DELTA_WPARAM(w) > 0 ? "MouseScrollUp" : "MouseScrollDown", g_capture_add);
        g_capture = -1;
        MarkChanged();
        Redraw();
      }
      return 0;
    case WM_LBUTTONDOWN: {
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      SetCapture(h);
      if (g_page == 1 && g_capture >= 0 && !g_capture_swallow) {
        AssignKey(g_capture, "LeftMouseButton", g_capture_add);
        g_capture = -1;
        MarkChanged();
        Redraw();
        return 0;
      }
      const int b = HitButton(x, y);
      if (b >= 0) { g_press_btn = b; Redraw(); return 0; }
      if (g_page == 1) {
        const int hb = HitBind(x, y);
        if (hb >= 0) { g_capture = hb; g_capture_add = false; g_capture_swallow = true; Redraw(); }
        return 0;
      }
      int seg;
      const int i = HitControl(x, y, &seg);
      if (i < 0) return 0;
      Ctl& c = g_c[i];
      if (c.kind == Kind::Toggle) {
        RectF r = Scaled(c.row);
        if (y >= r.Y && y <= r.Y + r.Height) { c.on = !c.on; c.dirty = true; MarkChanged(); }
      } else if (c.kind == Kind::Segmented) {
        if (seg >= 0 && seg != c.sel) {
          if (c.id == kQuality) ApplyPreset(seg);
          else { c.sel = seg; c.dirty = true; }
          MarkChanged();
        }
      } else {
        RectF ct = Scaled(c.ctl);
        if (y >= ct.Y - 6 * g_scale && y <= ct.Y + ct.Height + 6 * g_scale) { g_drag = i; SetSliderFromX(c, x); }
      }
      Redraw();
      return 0;
    }
    case WM_LBUTTONUP: {
      g_capture_swallow = false;
      ReleaseCapture();
      const float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
      const int b = HitButton(x, y);
      const int pressed = g_press_btn;
      g_press_btn = -1;
      g_drag = -1;
      Redraw();
      if (pressed >= 0 && pressed == b) {
        if (b == 0) { g_page = 1 - g_page; g_capture = -1; }
        else if (b == 1) DoReset();
        else if (b == 2) DoSave();
        else DoPlay();
        Redraw();
      }
      return 0;
    }
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
      if (g_page == 1 && g_capture >= 0) {
        if (w == VK_ESCAPE) { g_capture = -1; Redraw(); return 0; }
        if (w == VK_DELETE) {
          g_bind_keys[g_capture].clear();
          g_bind_dirty = true;
          g_capture = -1;
          MarkChanged();
          Redraw();
          return 0;
        }
        const std::string name = KeyFromMessage(w, l);
        if (!name.empty()) {
          AssignKey(g_capture, name, g_capture_add);
          g_capture = -1;
          MarkChanged();
          Redraw();
        }
        return 0;
      }
      if (m == WM_SYSKEYDOWN) return DefWindowProcW(h, m, w, l);
      if (w == VK_ESCAPE && g_page == 1) { g_page = 0; Redraw(); }
      else if (w == VK_ESCAPE) PostMessageW(h, WM_CLOSE, 0, 0);
      else if (w == VK_RETURN) { if (GetKeyState(VK_CONTROL) < 0) DoPlay(); else { DoSave(); Redraw(); } }
      return 0;
    case WM_GETMINMAXINFO: {
      // Wider is allowed (labels get more room); the height follows the content and is fixed.
      RECT wr, cr;
      GetWindowRect(h, &wr);
      GetClientRect(h, &cr);
      const int ncw = (wr.right - wr.left) - cr.right, nch = (wr.bottom - wr.top) - cr.bottom;
      auto* mm = reinterpret_cast<MINMAXINFO*>(l);
      mm->ptMinTrackSize.x = LONG(kMinW * g_scale) + ncw;
      mm->ptMaxTrackSize.x = LONG(kMaxW * g_scale) + ncw;
      mm->ptMinTrackSize.y = LONG(g_min_h * g_scale) + nch;
      mm->ptMaxTrackSize.y = 6000;
      return 0;
    }
    case WM_SIZE:
      if (w != SIZE_MINIMIZED && g_scale > 0) {
        g_win_w = (std::max)(kMinW, (std::min)(kMaxW, float(LOWORD(l)) / g_scale));
        {
          const float hh = float(HIWORD(l)) / g_scale;
          g_user_h = hh > g_min_h + 1.f ? hh : 0.f;
        }
        Layout();
        Redraw();
      }
      return 0;
    case WM_DPICHANGED: {
      g_scale = float(HIWORD(w)) / 96.f;
      const RECT* r = reinterpret_cast<RECT*>(l);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      Redraw();
      return 0;
    }
    case WM_CLOSE:
      if (g_changed) {
        const int r = MessageBoxW(h, L"Save your changes before closing?", L"Gears of War: Judgment",
                                  MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES && !DoSave()) return 0;
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

std::wstring SelfDir() {
  wchar_t buf[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  std::wstring p = buf;
  return p.substr(0, p.find_last_of(L"\\/"));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  g_dir = SelfDir();
  g_cfg_path = g_dir + L"\\config\\gowj.toml";
  InitControls();
  LoadConfig();
  LoadControls();
  LoadLastStart();

  // Command-line mode: --apply key=value ... (used by scripts and tests; no window).
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && argc >= 2 && std::wstring(argv[1]) == L"--apply") {
    for (int i = 2; i < argc; ++i) {
      std::string kv = Narrow(argv[i]);
      const size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      std::string value = kv.substr(eq + 1);
      if (kv.substr(0, eq) == "preset") {
        ApplyPreset(value == "low" ? 1 : value == "medium" ? 2 : value == "high" ? 3 : 0);
        continue;
      }
      if (kv.substr(0, eq) == "limit") {
        const int hz = atoi(value.c_str());
        g_c[kLimit].sel = hz == 30 ? 0 : hz == 60 ? 1 : hz == 90 ? 2 : hz == 120 ? 3 : 4;
        g_c[kLimit].dirty = true;
        continue;
      }
      if (value == "-") { RemoveKey(kv.substr(0, eq)); continue; }
      // Text values must be quoted in the config file (a bare word makes the whole file invalid).
      const bool bare = value == "true" || value == "false" ||
                        value.find_first_not_of("0123456789.-+") == std::string::npos;
      if (!bare && value.front() != '"') value = "\"" + value + "\"";
      SetValue(kv.substr(0, eq), value);
    }
    ApplyControls();
    return SaveConfig() ? 0 : 1;
  }

  {
    DWORD a = GetFileAttributesW((g_dir + L"\\GearsOfWarJudgment.exe").c_str());
    g_game_found = a != INVALID_FILE_ATTRIBUTES;
  }
  SetStatus(g_game_found ? L"Hover a setting for details. Changes apply the next time the game starts."
                         : L"Put this program in the same folder as GearsOfWarJudgment.exe to use the Play button.",
            kMuted);

  using SetCtx = BOOL(WINAPI*)(HANDLE);
  if (auto fn = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
    fn(reinterpret_cast<HANDLE>(-4));  // per-monitor v2
  } else {
    SetProcessDPIAware();
  }

  GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  GdiplusStartup(&token, &gsi, nullptr);

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"gowj_settings";
  RegisterClassW(&wc);

  Layout();
  // Size from the DPI of the monitor the window will open on.
  HDC sdc = GetDC(nullptr);
  g_scale = float(GetDeviceCaps(sdc, LOGPIXELSX)) / 96.f;
  ReleaseDC(nullptr, sdc);
  RECT rc{0, 0, LONG(g_win_w * g_scale), LONG(TotalHeight() * g_scale)};
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
  AdjustWindowRect(&rc, style, FALSE);
  const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Gears of War: Judgment - Settings", style,
                           (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                           ww, wh, nullptr, nullptr, inst, nullptr);
  const UINT dpi = GetDpiForWindow(g_hwnd);
  if (dpi) {
    // Always size from the real frame metrics at this DPI (the resize frame is thicker than the plain one).
    g_scale = float(dpi) / 96.f;
    RECT r2{0, 0, LONG(g_win_w * g_scale), LONG(TotalHeight() * g_scale)};
    AdjustWindowRectExForDpi(&r2, style, FALSE, 0, dpi);
    SetWindowPos(g_hwnd, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top, SWP_NOMOVE | SWP_NOZORDER);
  }
  BOOL dark = TRUE;
  DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));  // dark title bar
  DwmSetWindowAttribute(g_hwnd, 19, &dark, sizeof(dark));
  ShowWindow(g_hwnd, show);
  UpdateWindow(g_hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  GdiplusShutdown(token);
  return 0;
}
