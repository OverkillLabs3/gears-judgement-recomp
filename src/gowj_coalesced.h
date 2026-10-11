// Coalesced tweak layer.
//
// The game keeps its engine settings, input bindings and UI strings in the Coalesced_*.bin bundles
// inside GearGame\CookedXbox360 (an archive of ini files). The edits below are applied to a cached
// copy of the player's own files and the runtime is told to read the copy instead
// (GOWJ_VFS_OVERRIDES). Nothing of the game is shipped and the game folder is never modified.
//
// Edits are grouped by purpose and each group is switched on by an option:
//   low_end      Low preset only: lighter renderer switches (see FINDINGS / gowj_quality).
//   pc_bindings  mouse wheel -> weapon swap, so the wheel works through the engine's own table.
//   prompts      menu prompts show keyboard labels ([Enter], [Esc]...) instead of pad glyphs.
//   comfort      camera shake off / field of view, each only when the player asked for it.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rex::glue {

// One game action and the keys bound to it (engine key names, see gowj_keys.h).
struct BindingChoice {
  std::string command;            // GBA_* action as the engine's binding table names it
  std::vector<std::string> keys;  // engine key names
  std::string suffix;             // text kept after the command ("| OnRelease ...")
};

struct CoalescedOptions {
  bool low_end = false;
  bool skip_logos = false;  // drop the startup logo movie
  bool mouse_qol = true;    // no pitch auto-centre, no engine mouse smoothing, no on-screen debug messages
  bool pc_graphics = false;  // Medium and High: trilinear texture mips and a larger streaming pool
  bool pc_bindings = true;
  std::vector<BindingChoice> bindings;  // from gowj.toml bind_* keys (defaults when absent)
  // Key = a name in [ControllerFont] of GearGameUI.int ("Controller_A"), value = the label shown.
  // Empty means "leave the pad glyphs alone".
  std::vector<std::pair<std::string, std::string>> prompt_labels;
  bool no_camera_shake = false;
  float draw_distance = 0;  // MaxDrawDistanceScale (stock 1.0); 0 = leave the game's value
  int fov = 0;  // 0 = the game's own value, otherwise the gameplay camera's FOVAngle in degrees
};

struct CoalescedResult {
  std::string overrides;  // value for GOWJ_VFS_OVERRIDES, empty when nothing was prepared
  std::string summary;    // one log line
};

CoalescedResult PrepareCoalescedOverrides(const std::filesystem::path& game_root,
                                          const std::filesystem::path& cache_dir,
                                          const CoalescedOptions& options);

// The options the player set in gowj.toml (gowj_prompts, gowj_fov, gowj_no_camera_shake, the
// keybind_* labels), plus the preset's low-end flag.
CoalescedOptions OptionsFromConfig(const std::filesystem::path& config, bool low_end, bool pc_graphics, int tier,
                                   int dd_steps = 0);

// Comma-separated engine key names the native input pump must watch for these bindings.
std::string ForwardedKeys(const CoalescedOptions& options);

// Reads one `key = value` from a TOML file ("" when absent); quotes are stripped.
std::string ReadTomlValue(const std::filesystem::path& file, const std::string& key);

}  // namespace rex::glue
