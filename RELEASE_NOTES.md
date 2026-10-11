# Gears of War: Judgment for PC, v0.2.1

**No game data is included; you need your own copy of the retail game.**

## What's new in v0.2.1

- **Saves load for everyone.** Fixed "your saves are corrupted" and Continue starting from the beginning on a
  fresh install. The game asked who created the save, and the answer was missing on a first run, so it never read
  the file. Saves written by v0.2.0 are fine and now load.
- **Play in your language.** *Graphics Settings > Game language* lists every language that came with the game
  files: English, French, Spanish, Korean, Chinese, Polish, Czech, Hungarian, Danish, Dutch, Norwegian and
  Finnish. Menus, subtitles and the keyboard prompts follow the language. These are the languages on the disc this port
  was made from. German, Italian, Portuguese, Russian and Japanese were not on it, so we could not test or offer
  them; another release of the game may include them.
- **1440p is back, experimental.** Re-added due to player demand. *Graphics Settings > Internal resolution* has a
  1440p choice again. It is very demanding (see the notes below); 720p stays the default.
- **Resizable settings window.** Drag any edge or corner, wider or taller.

## Download

- `GearsOfWarJudgment-PC-v0.2.1.zip`: unzip to a writable folder, run `GearsOfWarJudgment.exe`, pick your game
  folder the first time.
- `Graphics Settings.exe` changes the settings. It has a Play button.
- Updating from v0.2.0: replace the two exe files. Your `config` and `saves` folders keep working.

## Notes on the new options

- Game language takes effect the next time the game starts. Czech, Hungarian, Danish, Dutch, Norwegian and Finnish
  have no console language id, so the game picks them from the country; the settings window sets both
  (`user_language` and `user_country` in `config/gowj.toml`). Spanish is the Latin American text on the disc.
- 1440p (*Graphics Settings > Internal resolution*) looks sharper and is experimental and very demanding: it needs
  a fast graphics card and can drop below 60 fps in heavy scenes. 720p stays the default. It is
  `draw_resolution_scale_x/y = 2`; the Auto quality level never picks it.

## Carried over from v0.2.0

- Real keyboard and mouse controls with rebinding, keyboard prompts, menus on keyboard.
- Draw distance setting, a lighter Low level, and an Auto quality that shortens draw distance first.
- Field of view, camera shake switch, skip the logo movie, sharper textures.
- Fixed the game closing at startup.

## For developers

- The save fix is in `src/gowj_content_probe.cpp` (the content call that reports the save's creator).
- DLSS and FSR 3.1/4 upscaling code is still in the repository but switched off (see `kTemporalUpscalers` in
  `src/gowj_quality.h`).

## Known issues

- Only the early campaign has been tested. Multiplayer and online features do not work.
- Tested only on an NVIDIA RTX 5070 Ti. AMD, Intel and handheld graphics are untested.
- 1440p is demanding and can show small visual glitches; 720p is the tested setting.
- A few in-game hint icons still show controller buttons.
- The game is limited by the processor: expect roughly 90 to 130 fps unlocked on a fast PC at 720p.
- The first minutes can stutter while shaders build.
