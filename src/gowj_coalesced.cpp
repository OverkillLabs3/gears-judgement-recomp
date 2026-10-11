#include "gowj_coalesced.h"

#include <cmath>
#include <windows.h>

#include <rex/cvar.h>

#include "gowj_keys.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

REXCVAR_DEFINE_STRING(gowj_prompts, "auto", "input",
                      "Prompt glyphs: auto (keyboard labels unless a gamepad is connected at launch), "
                      "keyboard or controller.");
REXCVAR_DEFINE_STRING(gowj_draw_distance, "auto", "gfx",
                      "How far the game draws objects: auto (the quality level chooses: Low 50%, Medium 75%, "
                      "High the game's own distance; shorter is a large speed-up on slow PCs), or a factor "
                      "of the game's own distance (0.4 short ... 1.0 stock ... 1.5 far).");
REXCVAR_DEFINE_BOOL(gowj_mouse_camera, true, "input", "Mouse-friendly camera: no automatic pitch re-centring, no engine mouse smoothing, and the engine's on-screen debug messages off.");
REXCVAR_DEFINE_BOOL(gowj_skip_logos, false, "gfx", "Skip the startup logo movie (saves about 15 seconds per launch).");
REXCVAR_DEFINE_INT32(gowj_fov, 0, "gfx",
                     "Field of view in degrees for the everyday camera modes (stock 80). 0 = the "
                     "game's own value. Aiming keeps its own zoom.");
REXCVAR_DEFINE_BOOL(gowj_no_camera_shake, false, "gfx",
                    "Turns off the roadie-run camera shake.");

namespace rex::glue {

namespace {

constexpr int kVersion = 13;  // bump when the edit definitions change (invalidates cached copies)

// ----------------------------------------------------------------------------------------------
// Coalesced bundle: BE u32 file count; per file: name, section count; per section: name, key count;
// per key: key, value. Strings are a BE int32 length (0 empty, negative = UTF-16LE + terminator).
// ----------------------------------------------------------------------------------------------
struct Entry {
  std::u16string key, value;
};
struct Section {
  std::u16string name;
  std::vector<Entry> entries;
};
struct IniFile {
  std::u16string name;
  std::vector<Section> sections;
};

struct Reader {
  const std::vector<uint8_t>& b;
  size_t pos = 0;
  bool ok = true;
  uint32_t U32() {
    if (pos + 4 > b.size()) {
      ok = false;
      return 0;
    }
    uint32_t v = (uint32_t(b[pos]) << 24) | (uint32_t(b[pos + 1]) << 16) | (uint32_t(b[pos + 2]) << 8) |
                 b[pos + 3];
    pos += 4;
    return v;
  }
  std::u16string Str() {
    int32_t len = int32_t(U32());
    if (!ok || len == 0) return {};
    if (len > 0 || len < -(1 << 20)) {
      ok = false;  // narrow strings are not expected in this file
      return {};
    }
    const size_t chars = size_t(-len);
    if (pos + chars * 2 > b.size()) {
      ok = false;
      return {};
    }
    std::u16string s(chars, u'\0');
    for (size_t i = 0; i < chars; ++i) s[i] = char16_t(b[pos + 2 * i] | (b[pos + 2 * i + 1] << 8));
    pos += chars * 2;
    if (!s.empty() && s.back() == u'\0') s.pop_back();
    return s;
  }
};

bool Parse(const std::vector<uint8_t>& bytes, std::vector<IniFile>* out) {
  Reader r{bytes};
  const uint32_t nfiles = r.U32();
  if (!r.ok || nfiles == 0 || nfiles > 4096) return false;
  for (uint32_t f = 0; f < nfiles && r.ok; ++f) {
    IniFile file;
    file.name = r.Str();
    const uint32_t nsec = r.U32();
    if (!r.ok || nsec > (1u << 20)) return false;
    for (uint32_t s = 0; s < nsec && r.ok; ++s) {
      Section sec;
      sec.name = r.Str();
      const uint32_t nk = r.U32();
      if (!r.ok || nk > (1u << 24)) return false;
      sec.entries.reserve(nk);
      for (uint32_t k = 0; k < nk && r.ok; ++k) {
        Entry e;
        e.key = r.Str();
        e.value = r.Str();
        sec.entries.push_back(std::move(e));
      }
      file.sections.push_back(std::move(sec));
    }
    out->push_back(std::move(file));
  }
  return r.ok && r.pos == bytes.size();  // the whole file must be understood
}

void PutU32(std::vector<uint8_t>* o, uint32_t v) {
  o->push_back(uint8_t(v >> 24));
  o->push_back(uint8_t(v >> 16));
  o->push_back(uint8_t(v >> 8));
  o->push_back(uint8_t(v));
}
void PutStr(std::vector<uint8_t>* o, const std::u16string& s) {
  if (s.empty()) {
    PutU32(o, 0);
    return;
  }
  PutU32(o, uint32_t(-int32_t(s.size() + 1)));
  for (char16_t c : s) {
    o->push_back(uint8_t(c & 0xFF));
    o->push_back(uint8_t(c >> 8));
  }
  o->push_back(0);
  o->push_back(0);
}
std::vector<uint8_t> Serialize(const std::vector<IniFile>& files) {
  std::vector<uint8_t> o;
  PutU32(&o, uint32_t(files.size()));
  for (const IniFile& f : files) {
    PutStr(&o, f.name);
    PutU32(&o, uint32_t(f.sections.size()));
    for (const Section& s : f.sections) {
      PutStr(&o, s.name);
      PutU32(&o, uint32_t(s.entries.size()));
      for (const Entry& e : s.entries) {
        PutStr(&o, e.key);
        PutStr(&o, e.value);
      }
    }
  }
  // Trailing zero padding: the game's own parser has been seen to read a byte past the end of the file
  // image it loaded, and for some sizes that lands on a no-access guard page and kills the process
  // at boot (guest 0xBF5D0000). The padding is never read as data (the reader is count driven).
  {
    char pad[16];
    const size_t n = GetEnvironmentVariableA("GOWJ_COALESCED_PAD", pad, sizeof pad) ? size_t(std::atoi(pad)) : 0;
    o.resize(o.size() + n, 0);
  }
  return o;
}

std::u16string W(const std::string& s) {
  std::u16string r;
  for (unsigned char c : s) r.push_back(char16_t(c));
  return r;
}
bool EndsWith(const std::u16string& s, const std::u16string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::u16string LowerW(std::u16string s) {
  for (char16_t& c : s)
    if (c >= u'A' && c <= u'Z') c = char16_t(c - u'A' + u'a');
  return s;
}

// The English bundle names its text files "GearGameUI.int"; every other language bundle names the same
// file "geargameui.FRA", "geargameui.KOR" and so on (and the case differs). An edit written for the
// ".int" name therefore also applies to the bundle's own language file.
bool FileMatches(const std::u16string& name, const std::string& edit_file, const std::u16string& locale) {
  const std::u16string n = LowerW(name), e = LowerW(W(edit_file));
  if (EndsWith(n, e)) return true;
  if (!locale.empty() && EndsWith(e, u".int")) {
    const std::u16string localized = e.substr(0, e.size() - 3) + LowerW(locale);
    return EndsWith(n, localized);
  }
  return false;
}

bool ReadAll(const std::filesystem::path& p, std::vector<uint8_t>* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// ----------------------------------------------------------------------------------------------
// Edits
// ----------------------------------------------------------------------------------------------
struct Edit {
  enum Kind { kSet, kAppend, kAddToNumber, kRemoveCommand, kRemoveKey, kReplaceSub, kRemoveValue } kind;
  std::string file;     // file name suffix inside the bundle
  std::string section;  // section name
  std::string key;
  std::string value;    // kAddToNumber: the delta, as text
};

const char* const kEngine = "Xbox360-GearEngine.ini";
const char* const kInput = "Xbox360-GearInput.ini";
const char* const kUI = "GearGameUI.int";
const char* const kCamera = "GearCamera.ini";
const char* const kPawn = "GearPawn.ini";

void AddLowEnd(std::vector<Edit>* e) {
  static const char* const kOff[] = {
      "StaticDecals", "DynamicDecals", "UnbatchedDecals", "DynamicLights", "DynamicShadows",
      "LightEnvironmentShadows", "CompositeDynamicLights", "SHSecondaryLighting", "MotionBlur",
      "MotionBlurPause", "DepthOfField", "AmbientOcclusion", "Bloom", "bAllowLightShafts",
      "Distortion", "FilteredDistortion", "SpeedTreeLeaves", "SpeedTreeFronds", "LensFlares",
      "FogVolumes", "FloatingPointRenderTargets", "AllowRadialBlur", "AllowSubsurfaceScattering",
      "AllowImageReflections", "AllowImageReflectionShadowing", "bAllowSeparateTranslucency",
      "bAllowDynamicIris", "bAllowTexturedFog"};
  for (const char* k : kOff) e->push_back({Edit::kSet, kEngine, "SystemSettings", k, "False"});
  e->push_back({Edit::kSet, kEngine, "SystemSettings", "DetailMode", "1"});
  e->push_back({Edit::kSet, kEngine, "SystemSettings", "MaxAnisotropy", "0"});
}

void AddSkipLogos(std::vector<Edit>* e) {
  // The startup logo movie (Open_Logos) is a Bink video; skipping it saves about 15 seconds per
  // launch and the boot crash seen in this build happens while it plays.
  e->push_back({Edit::kRemoveValue, kEngine, "FullScreenMovie", "StartupMovies", "Open_Logos"});
  e->push_back({Edit::kRemoveValue, kEngine, "FullScreenMovie", "SkippableMovies", "Open_Logos"});
}

void AddMouseQol(std::vector<Edit>* e) {
  // Console camera habits that fight a mouse: the camera re-centres its pitch by itself while you
  // move, and the engine smooths mouse input (the mouse path here is already direct).
  e->push_back({Edit::kSet, kInput, "GearGame.GearPlayerInput", "bAutoCenterPitch", "FALSE"});
  e->push_back({Edit::kSet, kInput, "Engine.PlayerInput", "bEnableMouseSmoothing", "false"});
  // The engine's on-screen developer messages (kismet and script warnings) are on in this build.
  e->push_back({Edit::kSet, kEngine, "Engine.Engine", "bEnableOnScreenDebugMessages", "false"});
}

void AddPcGraphics(std::vector<Edit>* e) {
  // The console's texture sampler uses point mip selection (hard bands where one mip level hands
  // over to the next on a slanted surface); trilinear blends them. No measurable frame-time cost.
  e->push_back({Edit::kReplaceSub, kEngine, "SystemSettings", "TEXTUREGROUP_", "MipFilter=point|MipFilter=linear"});
  e->push_back({Edit::kReplaceSub, kEngine, "SystemSettings", "TEXTUREGROUP_", "MipFilter=Point|MipFilter=linear"});
  // The streaming pool was sized for the console's 512 MB; a larger one keeps more detail resident
  // (frame rate was unchanged in the benchmark scene).
  e->push_back({Edit::kSet, kEngine, "TextureStreaming", "PoolSize", "256"});
}

// Loading-screen tips name pad buttons in parentheses: "press (X) to revive a downed squadmate".
// With keyboard prompts on they read the player's own keys instead, taken from the bindings.
std::string FriendlyKeyName(const std::string& k) {
  static const struct { const char* n; const char* f; } kMap[] = {
      {"SpaceBar", "Space"}, {"LeftShift", "Shift"}, {"RightShift", "Shift"}, {"LeftControl", "Ctrl"},
      {"RightControl", "Ctrl"}, {"LeftAlt", "Alt"}, {"RightAlt", "Alt"}, {"LeftMouseButton", "LMB"},
      {"RightMouseButton", "RMB"}, {"MiddleMouseButton", "MMB"}, {"ThumbMouseButton", "Mouse 4"},
      {"ThumbMouseButton2", "Mouse 5"}, {"MouseScrollUp", "Wheel"}, {"MouseScrollDown", "Wheel"},
      {"One", "1"}, {"Two", "2"}, {"Three", "3"}, {"Four", "4"}, {"Five", "5"}, {"Six", "6"},
      {"Seven", "7"}, {"Eight", "8"}, {"Nine", "9"}, {"Zero", "0"}, {"Slash", "/"},
      {"BackSpace", "Backspace"}, {"Underscore", "-"}, {"Equals", "="}, {"Comma", ","}, {"Period", "."},
  };
  for (const auto& m : kMap)
    if (k == m.n) return m.f;
  return k;
}

void AddTips(std::vector<Edit>* e, const CoalescedOptions& o) {
  auto first = [&](const char* command, const char* fallback) {
    for (const BindingChoice& bc : o.bindings)
      if (bc.command == command && !bc.keys.empty()) return "[" + FriendlyKeyName(bc.keys.front()) + "]";
    return std::string("[") + fallback + "]";
  };
  const std::pair<const char*, std::string> map[] = {
      {"(A)", first("GBA_MoveAction", "Space")}, {"(B)", first("GBA_Melee", "F")},
      {"(X)", first("GBA_Pickup", "E")},         {"(Y)", first("GBA_SwitchWeapon", "1")},
      {"(LB)", first("GBA_Grenades", "2")},      {"(RB)", first("GBA_Reload", "R")},
      {"(LT)", first("GBA_Target", "RMB")},      {"(RT)", first("GBA_Shoot", "LMB")},
      {"(BACK)", first("GBA_Scoreboard", "/")},  {"(START)", "[Esc]"},
  };
  for (const auto& m : map)
    e->push_back({Edit::kReplaceSub, "Subtitles.int", "Subtitles", "LoadingMovie_", std::string(m.first) + "|" + m.second});
}

void AddBindings(std::vector<Edit>* e, const CoalescedOptions& o) {
  // Every action the player (or the defaults) binds: drop the game's own keyboard/mouse entries
  // for that action, then add the chosen keys. Pad entries (Name="Xbox...") are never touched.
  std::vector<std::string> used;
  for (const BindingChoice& bc : o.bindings) {
    e->push_back({Edit::kRemoveCommand, kInput, "Engine.PlayerInput", "Bindings", bc.command});
    for (const std::string& k : bc.keys) {
      e->push_back({Edit::kAppend, kInput, "Engine.PlayerInput", "Bindings",
                    "(Name=\"" + k + "\",Command=\"" + bc.command + bc.suffix + "\")"});
      used.push_back(k);
    }
  }
  // Debug and cheat commands share the table (letters = god, kill, slomo, fly...). A key the
  // player bound must do only what it was bound to.
  for (const std::string& k : used)
    e->push_back({Edit::kRemoveKey, kInput, "Engine.PlayerInput", "Bindings", k});
}

void AddPrompts(std::vector<Edit>* e, const CoalescedOptions& o) {
  for (const auto& kv : o.prompt_labels)
    e->push_back({Edit::kSet, kUI, "ControllerFont", kv.first,
                  "<Color:R=1.0,G=1.0,B=1.0>" + kv.second + "</Color>"});
}

void AddComfort(std::vector<Edit>* e, const CoalescedOptions& o) {
  if (o.no_camera_shake) {
    e->push_back({Edit::kSet, kPawn, "GearGame.GearPawn", "RoadieRunShakyCamDampeningFactor", "0.0f"});
    e->push_back({Edit::kSet, kPawn, "GearGameContent.GearPawn_LocustKantusArmor",
                  "RoadieRunShakyCamDampeningFactor", "0.0f"});
  }
  if (o.draw_distance > 0) {
    // MaxDrawDistanceScale multiplies every object's own cull distance. Measured on the Low level
    // (Ryzen 7 5800X3D, CPU limited, vsync off): 1.0 = 104 fps, 0.8 = 113, 0.6 = 123, 0.4 = 159,
    // 0.3 = 198. 0.6 looks the same as 1.0 in the test street; at 0.4 distant props thin out and at
    // 0.3 effects such as the burning tree start to disappear.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", o.draw_distance);
    e->push_back({Edit::kSet, kEngine, "SystemSettings", "MaxDrawDistanceScale", buf});
  }
  if (o.fov > 0) {
    // Stock gameplay FOV is 80. The setting is a wider (or narrower) view for the everyday camera
    // modes; aiming and scoped modes keep their own zoom so aiming still zooms.
    const int delta = std::clamp(o.fov, 60, 120) - 80;
    if (delta != 0) {
      static const char* const kModes[] = {
          "GearGame.GearGameplayCameraMode", "GearGame.GearCameraMode", "GearGame.GameplayCam_Default",
          "GearGame.GameplayCam_Cover", "GearGame.GameplayCam_RoadieRun",
          "GearGame.GameplayCam_RoadieRunStab", "GearGame.GameplayCam_DeployedShield",
          "GearGame.GameplayCam_Execution", "GearGame.GameplayCam_Reaver", "GearGame.GameplayCam_Death"};
      for (const char* m : kModes)
        e->push_back({Edit::kAddToNumber, kCamera, m, "FOVAngle", std::to_string(delta)});
    }
  }
}

uint64_t Fnv(const std::string& s, uint64_t h = 1469598103934665603ull) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

// Many scenes embed the controller glyph directly, e.g. "<Font:$ControllerFont>A</Font> ACCEPT" or
// "<FONT FACE='$ControllerFont' COLOR='#C38139'>H</FONT>". A single glyph character that is one of the
// pad buttons is replaced by its key label; multi-character runs (award stars) and glyphs that are
// not buttons are left alone. `labels` maps the glyph character to the label text.
int ReplaceInlineGlyphs(std::u16string* v, const std::vector<std::pair<char16_t, std::u16string>>& labels,
                        const std::u16string& tag, bool only_abxy) {
  int n = 0;
  size_t pos = 0;
  while ((pos = v->find(tag, pos)) != std::u16string::npos) {
    const size_t open = v->rfind(u'<', pos);
    const size_t close = v->find(u'>', pos);
    if (open == std::u16string::npos || close == std::u16string::npos || close < pos) { pos += tag.size(); continue; }
    // exactly one character, then the closing </font> (any case)
    if (close + 3 >= v->size() || (*v)[close + 2] != u'<' || (*v)[close + 3] != u'/') {
      pos = close; continue;
    }
    const size_t end = v->find(u'>', close + 2);
    if (end == std::u16string::npos) { pos = close; continue; }
    std::u16string closer = v->substr(close + 2, end - (close + 2) + 1);
    for (auto& ch : closer) ch = char16_t(ch >= u'A' && ch <= u'Z' ? ch + 32 : ch);
    if (closer != u"</font>") { pos = close; continue; }
    const char16_t glyph = (*v)[close + 1];
    if (only_abxy && glyph != u'A' && glyph != u'B' && glyph != u'X' && glyph != u'Y') { pos = end; continue; }
    const std::u16string* label = nullptr;
    for (const auto& kv : labels) if (kv.first == glyph) { label = &kv.second; break; }
    if (!label) { pos = end; continue; }
    v->replace(open, end - open + 1, *label);
    pos = open + label->size();
    ++n;
  }
  return n;
}

int Apply(std::vector<IniFile>* files, const std::vector<Edit>& edits, const std::u16string& locale) {
  int changed = 0;
  // glyph character -> label, from the ControllerFont table (before it is edited) and the new labels
  std::vector<std::pair<char16_t, std::u16string>> glyph_labels;
  {
    std::vector<std::pair<std::u16string, std::u16string>> wanted;  // key -> label (plain text)
    for (const Edit& ed : edits)
      if (ed.section == "ControllerFont" && ed.kind == Edit::kSet) {
        std::u16string label = W(ed.value);
        const size_t a = label.find(u'>');
        const size_t b = label.rfind(u"</Color>");
        if (a != std::u16string::npos && b != std::u16string::npos && b > a) label = label.substr(a + 1, b - a - 1);
        wanted.push_back({W(ed.key), label});
      }
    if (!wanted.empty()) {
      for (IniFile& f : *files) {
        if (!FileMatches(f.name, kUI, locale)) continue;
        for (Section& sec : f.sections) {
          if (sec.name != u"ControllerFont") continue;
          for (Entry& en : sec.entries) {
            const size_t a = en.value.find(u"$ControllerFont>");
            if (a == std::u16string::npos) continue;
            const size_t g = a + 16;
            if (g + 1 >= en.value.size() || en.value[g + 1] != u'<') continue;
            for (const auto& w : wanted)
              if (w.first == en.key) { glyph_labels.push_back({en.value[g], w.second}); break; }
          }
        }
      }
    }
  }
  for (const Edit& ed : edits) {
    for (IniFile& f : *files) {
      if (!FileMatches(f.name, ed.file, locale)) continue;
      for (Section& s : f.sections) {
        if (s.name != W(ed.section)) continue;
        if (ed.kind == Edit::kAppend) {
          s.entries.push_back({W(ed.key), W(ed.value)});
          ++changed;
          continue;
        }
        if (ed.kind == Edit::kRemoveCommand || ed.kind == Edit::kRemoveKey) {
          const std::u16string cmd_tag = u"Command=\"";
          const std::u16string name_tag = u"Name=\"";
          const size_t before = s.entries.size();
          s.entries.erase(
              std::remove_if(
                  s.entries.begin(), s.entries.end(),
                  [&](const Entry& en) {
                    if (en.key != W(ed.key)) return false;
                    auto field = [&](const std::u16string& tag) {
                      size_t p = en.value.find(tag);
                      if (p == std::u16string::npos) return std::u16string();
                      p += tag.size();
                      return en.value.substr(p, en.value.find(u'"', p) - p);
                    };
                    const std::u16string name = field(name_tag), cmd = field(cmd_tag);
                    if (name.rfind(u"Xbox", 0) == 0 || name.rfind(u"GBA_", 0) == 0) return false;
                    if (ed.kind == Edit::kRemoveCommand) {
                      const std::u16string want = W(ed.value);
                      return cmd == want || cmd.rfind(want + u" |", 0) == 0;
                    }
                    return name == W(ed.value) && cmd.rfind(u"GBA_", 0) != 0;
                  }),
              s.entries.end());
          changed += int(before - s.entries.size());
          continue;
        }
        if (ed.kind == Edit::kRemoveValue) {
          const size_t before = s.entries.size();
          s.entries.erase(std::remove_if(s.entries.begin(), s.entries.end(),
                                         [&](const Entry& en) { return en.key == W(ed.key) && en.value == W(ed.value); }),
                          s.entries.end());
          changed += int(before - s.entries.size());
          continue;
        }
        if (ed.kind == Edit::kReplaceSub) {
          const size_t bar = ed.value.find('|');
          const std::u16string from = W(ed.value.substr(0, bar)), to = W(ed.value.substr(bar + 1));
          const std::u16string prefix = W(ed.key);
          for (Entry& en : s.entries) {
            if (en.key.compare(0, prefix.size(), prefix) != 0) continue;
            size_t p = 0;
            while ((p = en.value.find(from, p)) != std::u16string::npos) {
              en.value.replace(p, from.size(), to);
              p += to.size();
              ++changed;
            }
          }
          continue;
        }
        for (Entry& en : s.entries) {
          if (en.key != W(ed.key)) continue;
          if (ed.kind == Edit::kSet) {
            if (en.value != W(ed.value)) {
              en.value = W(ed.value);
              ++changed;
            }
          } else {  // kAddToNumber
            std::string cur;
            for (char16_t c : en.value) cur.push_back(char(c));
            char* end = nullptr;
            const double v = std::strtod(cur.c_str(), &end);
            if (end != cur.c_str()) {
              const int nv = int(v + std::atoi(ed.value.c_str()));
              en.value = W(std::to_string(nv));
              ++changed;
            }
          }
          break;
        }
      }
    }
  }
  if (!glyph_labels.empty()) {
    for (IniFile& f : *files)
      for (Section& sec : f.sections)
        for (Entry& en : sec.entries)
        {
          if (en.value.find(u"$ControllerFont") != std::u16string::npos)
            changed += ReplaceInlineGlyphs(&en.value, glyph_labels, u"$ControllerFont", false);
          if (en.value.find(u"Xbox360_18pt") != std::u16string::npos)
            changed += ReplaceInlineGlyphs(&en.value, glyph_labels, u"Xbox360_18pt", true);
        }
  }
  return changed;
}

}  // namespace

std::string ReadTomlValue(const std::filesystem::path& file, const std::string& key) {
  std::ifstream f(file);
  std::string line;
  while (std::getline(f, line)) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] == '#' || line[i] == '[') continue;
    size_t eq = line.find('=', i);
    if (eq == std::string::npos) continue;
    std::string k = line.substr(i, eq - i);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
    if (k != key) continue;
    std::string v = line.substr(eq + 1);
    size_t h = v.find('#');
    if (h != std::string::npos) v = v.substr(0, h);
    size_t a = v.find_first_not_of(" \t\""), b = v.find_last_not_of(" \t\r\"");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
  }
  return {};
}

CoalescedResult PrepareCoalescedOverrides(const std::filesystem::path& game_root,
                                          const std::filesystem::path& cache_dir,
                                          const CoalescedOptions& options) {
  CoalescedResult res;
  {
    char off[4];
    if (GetEnvironmentVariableA("GOWJ_COALESCED", off, sizeof off) && off[0] == '0') {
      res.summary = "COALESCED: disabled by GOWJ_COALESCED=0";
      return res;
    }
  }
  std::vector<Edit> edits;
  std::string groups;
  char skip[64] = {};
  GetEnvironmentVariableA("GOWJ_COALESCED_SKIP", skip, sizeof skip);
  auto skipped = [&](const char* g) { return std::strstr(skip, g) != nullptr; };
  if (options.low_end && !skipped("low")) {
    AddLowEnd(&edits);
    groups += "low-end ";
  }
  if (options.mouse_qol && !skipped("mouseqol")) {
    AddMouseQol(&edits);
    groups += "mouse-qol ";
  }
  if (options.skip_logos && !skipped("logos")) {
    AddSkipLogos(&edits);
    groups += "skip-logos ";
  }
  if (options.pc_graphics && !skipped("gfx")) {
    AddPcGraphics(&edits);
    groups += "pc-graphics ";
  }
  if (options.pc_bindings && !skipped("bind")) {
    AddBindings(&edits, options);
    groups += "bindings ";
  }
  if (!options.prompt_labels.empty() && !skipped("prompts")) {
    AddTips(&edits, options);
    AddPrompts(&edits, options);
    groups += "key-prompts ";
  }
  if ((options.no_camera_shake || options.fov > 0 || options.draw_distance > 0) && !skipped("comfort")) {
    AddComfort(&edits, options);
    groups += "comfort ";
  }
#ifdef GOWJ_DEV_TOOLS
  {
    // Test rigs: GOWJ_COALESCED_EXTRA="file|section|key|value;file|section|key|value" adds Set edits
    // (used to screen engine settings one at a time on the same scene).
    char buf[4096]{};
    if (GetEnvironmentVariableA("GOWJ_COALESCED_EXTRA", buf, sizeof buf) && buf[0]) {
      std::string all = buf;
      size_t pos = 0;
      while (pos < all.size()) {
        size_t semi = all.find(';', pos);
        std::string item = all.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? all.size() : semi + 1;
        std::vector<std::string> f;
        size_t p = 0;
        while (p <= item.size()) {
          size_t bar = item.find('|', p);
          f.push_back(item.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
          if (bar == std::string::npos) break;
          p = bar + 1;
        }
        if (f.size() == 4) edits.push_back({Edit::kSet, f[0], f[1], f[2], f[3]});
      }
      groups += "extra ";
    }
  }
#endif
  if (edits.empty()) return res;

  uint64_t h = Fnv("v" + std::to_string(kVersion));
  for (const Edit& e : edits) h = Fnv(e.file + "|" + e.section + "|" + e.key + "|" + e.value + "|" + char('0' + e.kind), h);

  std::error_code ec;
  const std::filesystem::path dir = game_root / "GearGame" / "CookedXbox360";
  const std::filesystem::path out_dir = cache_dir / "coalesced";
  std::filesystem::create_directories(out_dir, ec);
  int files_done = 0, values = 0, reused = 0;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::filesystem::path src = it->path();
    const std::string name = src.filename().string();
    if (name.rfind("Coalesced_", 0) != 0 || src.extension() != ".bin") continue;
    const std::filesystem::path dst = out_dir / name;
    const std::filesystem::path stamp_path = out_dir / (name + ".stamp");
    const auto size = std::filesystem::file_size(src, ec);
    const auto time = std::filesystem::last_write_time(src, ec).time_since_epoch().count();
    const std::string stamp = std::to_string(h) + " " + std::to_string(size) + " " + std::to_string(time);
    bool fresh = false;
    if (std::filesystem::exists(dst, ec)) {
      std::ifstream sf(stamp_path);
      std::string have;
      std::getline(sf, have);
      fresh = (have == stamp);
    }
    if (fresh) {
      ++reused;
    } else {
      std::vector<uint8_t> bytes;
      std::vector<IniFile> files;
      if (!ReadAll(src, &bytes) || !Parse(bytes, &files)) continue;  // unknown layout: leave alone
      const std::u16string locale = W(name.substr(10, name.size() - 14));  // Coalesced_XXX.bin -> XXX
      const int changed = Apply(&files, edits, locale);
      if (changed == 0) continue;  // this locale file has none of the sections
      const std::vector<uint8_t> patched = Serialize(files);
      {
        std::ofstream of(dst, std::ios::binary | std::ios::trunc);
        if (!of) continue;
        of.write(reinterpret_cast<const char*>(patched.data()), std::streamsize(patched.size()));
        if (!of) continue;
      }
      std::ofstream sf(stamp_path, std::ios::trunc);
      sf << stamp << "\n";
      values += changed;
    }
    ++files_done;
    if (!res.overrides.empty()) res.overrides += ";";
    res.overrides += "GearGame/CookedXbox360/" + name + "=" + dst.string();
  }
  if (files_done == 0) {
    res.summary = "COALESCED: no bundle could be prepared; the game files are used unchanged";
  } else {
    res.summary = "COALESCED: " + groups + "applied to " + std::to_string(files_done) + " file(s) (" +
                  std::to_string(values) + " values changed, " + std::to_string(reused) +
                  " reused from cache); the game folder is not modified";
  }
  return res;
}

namespace {

// Auto prompts follow the device used last session (written by the key pump, see gowj_native_input.cpp):
// keyboard and mouse unless the last thing used was a controller. A plugged-in controller alone no longer
// switches the menus to controller art for someone playing on the keyboard.
bool LastDeviceWasKeyboard() {
  wchar_t base[MAX_PATH]{};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return true;
  std::ifstream in(std::wstring(base) + L"\\GearsOfWarJudgmentPC\\user\\last_device.txt");
  std::string v;
  if (!(in >> v)) return true;
  return v != "pad";
}

bool AnyPhysicalGamepad() {
  HMODULE m = LoadLibraryW(L"xinput1_4.dll");
  if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
  if (!m) return false;
  struct State {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
  };
  using Fn = DWORD(WINAPI*)(DWORD, State*);
  auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "XInputGetState"));
  bool any = false;
  if (fn) {
    for (DWORD i = 0; i < 4 && !any; ++i) {
      State s{};
      any = fn(i, &s) == ERROR_SUCCESS;
    }
  }
  FreeLibrary(m);
  return any;
}

// "Return" -> "Enter" etc: the name the player sees for the first key in a keybind list.
std::string KeyLabel(const std::string& list, const std::string& fallback) {
  std::string first = list.substr(0, list.find(','));
  while (!first.empty() && first.front() == ' ') first.erase(first.begin());
  while (!first.empty() && first.back() == ' ') first.pop_back();
  if (first.empty()) first = fallback;
  if (first == "Return") first = "Enter";
  if (first == "Control") first = "Ctrl";
  if (first == "Escape") first = "Esc";
  if (first == "Space") first = "Space";
  return "[" + first + "]";
}

}  // namespace

CoalescedOptions OptionsFromConfig(const std::filesystem::path& config, bool low_end, bool pc_graphics, int tier,
                                   int dd_steps) {
  CoalescedOptions o;
  o.low_end = low_end;
  o.pc_graphics = pc_graphics;
  {
    const std::string mq = config.empty() ? std::string() : ReadTomlValue(config, "gowj_mouse_camera");
    o.mouse_qol = mq != "false";
  }
  {
    const std::string sl = config.empty() ? std::string() : ReadTomlValue(config, "gowj_skip_logos");
    o.skip_logos = sl == "true";
  }
  {
    std::string dd = config.empty() ? std::string() : ReadTomlValue(config, "gowj_draw_distance");
    if (dd.empty() || dd == "auto") {
      // High keeps the stock distance. Each auto-tuner step (saved when 60 fps was not held) takes 20% off.
      const float base = tier == 0 ? 0.5f : tier == 1 ? 0.75f : 1.0f;
      const float v = base * std::pow(0.8f, float(std::clamp(dd_steps, 0, 2)));
      o.draw_distance = (tier >= 2 && dd_steps <= 0) ? 0.f : std::max(v, 0.25f);
    }
    else o.draw_distance = std::clamp(float(std::atof(dd.c_str())), 0.25f, 3.0f);
  }
  auto get = [&](const char* k) { return config.empty() ? std::string() : ReadTomlValue(config, k); };
  std::string mode = get("gowj_prompts");
  if (mode.empty()) mode = "auto";
  const bool keyboard = mode == "keyboard" || (mode == "auto" && LastDeviceWasKeyboard());
  if (keyboard) {
    auto L = [&](const char* cfg, const char* def, const char* shown) {
      const std::string v = get(cfg);
      return KeyLabel(v.empty() ? std::string(def) : v, shown);
    };
    const std::string a = L("keybind_a", "Return,Space", "Enter");
    const std::string b = L("keybind_b", "Backspace", "Backspace");
    const std::string x = L("keybind_x", "R", "R");
    const std::string y = L("keybind_y", "Y", "Y");
    const std::string lb = L("keybind_left_shoulder", "Q", "Q");
    const std::string rb = L("keybind_right_shoulder", "F", "F");
    const std::string start = L("keybind_start", "Escape", "Esc");
    const std::string back = L("keybind_back", "Tab", "Tab");
    o.prompt_labels = {
        {"Controller_A", a},         {"Controller_B", b},          {"Controller_X", x},
        {"Controller_Y", y},         {"Controller_leftBumper", lb}, {"Controller_rightBumper", rb},
        {"Controller_Start", start}, {"Controller_Back", back},    {"Controller_leftStick", "[WASD]"},
        {"Controller_LeftStickDown", "[WASD]"}, {"Controller_rightStick", "[Mouse]"},
        {"Controller_leftStickClick", "[Ctrl]"}, {"Controller_rightStickClick", "[T]"},
        {"Controller_leftTrigger", "[RMB]"},     {"Controller_rightTrigger", "[LMB]"},
        {"Controller_dPad", "[Arrows]"},         {"Controller_DPadUp", "[Up]"},
        {"Controller_DPadDown", "[Down]"},       {"Controller_DPadLeft", "[Left]"},
        {"Controller_DPadRight", "[Right]"},     {"Controller_DPadUpDown", "[Up/Down]"},
        {"Controller_DPadLeftRight", "[Left/Right]"}, {"Controller_ArrowUp", "[Up]"},
        {"Controller_ArrowDown", "[Down]"},      {"Controller_ArrowLeft", "[Left]"},
        {"Controller_ArrowRight", "[Right]"},    {"Controller_UpDown", "[Up/Down]"},
        {"Controller_UpDownLeftRight", "[Arrows]"}};
  }
  struct Def {
    const char* toml;
    const char* command;
    const char* keys;
    const char* suffix;
  };
  static const Def kDefs[] = {
      {"bind_forward", "GBA_MoveForward", "W", ""},
      {"bind_backward", "GBA_MoveBackward", "S", ""},
      {"bind_left", "GBA_StrafeLeft", "A", ""},
      {"bind_right", "GBA_StrafeRight", "D", ""},
      {"bind_action", "GBA_MoveAction", "SpaceBar", " | OnRelease CancelCine 2.0"},
      {"bind_crouch", "GBA_Crouch", "LeftControl,C", ""},
      {"bind_walk", "GBA_Walk", "Z", ""},
      {"bind_fire", "GBA_Shoot", "LeftMouseButton", ""},
      {"bind_aim", "GBA_Target", "RightMouseButton", ""},
      {"bind_melee", "GBA_Melee", "F,V", ""},
      {"bind_reload", "GBA_Reload", "R,ThumbMouseButton", ""},
      {"bind_zoom", "GBA_Zoom", "T,MiddleMouseButton", ""},
      {"bind_swap_weapon", "GBA_SwitchWeapon", "One,MouseScrollUp,MouseScrollDown", ""},
      {"bind_grenade", "GBA_Grenades", "Two", ""},
      {"bind_pickup", "GBA_Pickup", "E", ""},
      {"bind_poi", "GBA_POI", "Q", ""},
      {"bind_tactical", "GBA_Taccom", "H", ""},
      {"bind_weapon1", "GBA_Weap1", "Three", ""},
      {"bind_weapon2", "GBA_Weap2", "Four", ""},
      {"bind_scoreboard", "GBA_Scoreboard", "Slash", ""},
  };
  for (const Def& d : kDefs) {
    std::string spec = get(d.toml);
    if (spec.empty()) spec = d.keys;
    BindingChoice bc;
    bc.command = d.command;
    bc.suffix = d.suffix;
    size_t pos = 0;
    while (pos <= spec.size()) {
      size_t comma = spec.find(',', pos);
      std::string k = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      while (!k.empty() && k.front() == ' ') k.erase(k.begin());
      while (!k.empty() && k.back() == ' ') k.pop_back();
      if (!k.empty() && IsKnownKeyName(k.c_str())) bc.keys.push_back(k);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    o.bindings.push_back(std::move(bc));
  }
  o.no_camera_shake = get("gowj_no_camera_shake") == "true";
  const std::string fov = get("gowj_fov");
  if (!fov.empty()) o.fov = std::atoi(fov.c_str());
  return o;
}

std::string ForwardedKeys(const CoalescedOptions& options) {
  std::string out;
  std::vector<std::string> seen;
  for (const BindingChoice& bc : options.bindings)
    for (const std::string& k : bc.keys)
      if (std::find(seen.begin(), seen.end(), k) == seen.end()) {
        seen.push_back(k);
        if (!out.empty()) out += ",";
        out += k;
      }
  return out;
}

}  // namespace rex::glue
