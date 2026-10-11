# Gears of War: Judgment for PC, v0.2.0

**No game data is included; you need your own copy of the retail game.**

## What's new

- **Real keyboard and mouse controls.** Rebind every key, see keyboard prompts in menus and tips.
- **Faster on slow PCs.** A draw distance setting, a lighter Low level, and Auto quality that shortens draw distance first.
- **Fixed the game closing at startup**, which happened about one launch in six.
- **More options:** field of view, camera shake, skip the logo movie, sharper textures.

## Download

- `GearsOfWarJudgment-PC-v0.2.0.zip`: unzip to a writable folder, run `GearsOfWarJudgment.exe`, pick your game
  folder the first time.
- `Graphics Settings.exe` changes the settings. It has a Play button.

## Keyboard and mouse

- Keys and mouse go into the game's own controls, and the game switches between keyboard and controller as you
  use them, so rumble and controller hints stay with the controller.
- Menus work with Enter or Space to accept, Backspace to go back, the arrow keys to move, and Esc for Start.
- Rebind every action in *Graphics Settings > Key bindings* (click, press the key; right-click adds a second).
  Or edit `config/gowj.toml`, for example `bind_reload = "R"`.
- Keyboard names in menus and loading tips (`[Enter]`, `[Esc]`), or controller names if you use one. Menu prompts
  set to Auto follow the device you used last session (keyboard and mouse by default), not just whether a
  controller is plugged in; the hints during play switch the moment you change device.
- Mouse-friendly camera: no automatic pitch re-centring, no engine mouse smoothing, no on-screen debug messages.
- Defaults: WASD, Space (cover, vault, roll, sprint), E (pick up, revive), Ctrl or C (crouch), F or V (melee,
  execute), R (reload), T (zoom), 1 or wheel (swap weapons), 2 (grenade), 3 and 4, Q, H, Z (walk).

## Faster on slow PCs

- **Draw distance** setting (Short, Normal, Far). Low draws half as far and Medium three quarters.
  In our test street Low went from 104 to 134 fps.
- **Lighter Low level:** fewer dynamic lights and shadows, decals, bloom and depth of field.
  Medium and High never lose effects, and your game folder is never changed.
- **Smarter Auto quality:** if 60 fps is not held, Auto now shortens the draw distance first (up to two steps)
  and only then lowers the quality level.
- v0.1.0 was limited by the processor at about 90 to 110 fps unlocked on a Ryzen 7 5800X3D. Slower processors
  gain the most from these changes.

## More options

- Field of view slider.
- Switch for the roadie-run camera shake.
- Skip the startup logo movie.
- Sharper textures on Medium and High (trilinear filtering, larger texture pool).
- The Graphics Settings window is resizable and has new sections for all of the above. The "lighter game
  settings" switch only shows while the Low level is selected.

## Stability

- Fixed the game closing while starting, about one launch in six.
- Menus work with the new keyboard controls, and the "using a gamepad" flag no longer flips on and off
  while you play on keyboard and mouse (it made prompts flicker).
- Crash reports (`gowj_crash.log`) now include the registers and the failing instruction.

## For developers

- The GPU plugin is built from source here (`scripts\build-gpu.ps1`).
- Optional profile-guided optimisation of the game code (`GOWJ_PGO`).
- DLSS and FSR 3.1/4 upscaling code is still in the repository but switched off for this release (see
  `kTemporalUpscalers` in `src/gowj_quality.h`).

## Known issues

- Only the early campaign has been tested. Multiplayer and online features do not work.
- Tested only on an NVIDIA RTX 5070 Ti. AMD, Intel and handheld graphics are untested.
- DLSS and FSR 3.1/4 upscaling is not in this release. It flickered textures while moving, so it is paused.
- A few in-game hint icons still show controller buttons.
- The game renders at 720p on every quality level, scaled to your screen with the older FSR 1 and SMAA options.
  1440p was removed because it was too heavy for most PCs.
- The game is limited by the processor: expect roughly 90 to 130 fps unlocked on a fast PC.
- The first minutes can stutter while shaders build.
