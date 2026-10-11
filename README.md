# Gears of War: Judgment for PC

A native Windows build of **Gears of War: Judgment** (Xbox 360, People Can Fly, 2013), made by
statically recompiling the original game code to x64 and running it on an emulated Xbox 360
graphics and system layer (the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)).

It is not an emulator running the game code: the game's PowerPC program is translated to C++ ahead of
time and compiled into a normal Windows executable. On top of that this project adds
a keyboard and mouse scheme with proper mouse look, an unlockable frame rate, SMAA and FSR
image processing, automatic quality selection and a number of fixes. See [docs/FEATURES.md](docs/FEATURES.md).

> **This repository and its releases contain no game data.** You need your own copy of
> Gears of War: Judgment for the Xbox 360. Nothing here is affiliated with or endorsed by
> Microsoft, Epic Games or People Can Fly.

**Status: early release.** The single-player campaign boots and plays, but testing so far has
mostly covered the early part of it. Multiplayer and online features are not supported. Please read
[Known issues](#known-issues) before reporting a problem.

## Download and play

1. Open the **[Releases](../../releases)** page of this repository and download the latest
   `GearsOfWarJudgment-PC-<version>.zip`.
2. Unzip it into a folder you can write to (not `Program Files`). It contains two programs:
   - **`GearsOfWarJudgment.exe`**: the game, one self-contained file.
   - **`Graphics Settings.exe`**: a settings window; keep it next to the game.
3. Have your game files ready: the folder from your own disc that contains `default.xex` and the
   `GearGame` folder. Only the retail version is supported (title ID `4D530A26`).
4. Double-click `GearsOfWarJudgment.exe`. The first time, a window asks for that game folder. It is
   remembered, so you are only asked once. (If you put the game exe inside the game folder, or the
   game folder next to it, nothing is asked.)

That is all: no installer, nothing else to download, no extra runtime. The first start takes a few
seconds longer because the exe unpacks its files into `%LOCALAPPDATA%\GearsOfWarJudgmentPC\bin`.
Your settings, saves and logs are kept in the folder you put the exe in.

To change graphics settings before playing, start **`Graphics Settings.exe`**. It has quality level,
texture filtering, anti-aliasing, FSR, frame limit, mouse sensitivity and more,
and a **Play** button. The same settings are in the game under **F4**.

### Requirements

- Windows 10 or 11, 64-bit
- A DirectX 12 graphics card. The quality is chosen from your card automatically
  (see [Quality](docs/FEATURES.md#automatic-quality)). It was developed and tested on an
  NVIDIA RTX 5070 Ti and **nothing else**; AMD, Intel and handheld GPUs use the same code but are
  untested.
- 8 GB of RAM or more (the game itself uses about 3 to 4 GB). The game files are roughly 7 GB; the
  cache adds a few hundred MB.
- A reasonably fast CPU. The game is limited by the processor, not the graphics card (see
  [Known issues](#known-issues)).

## What is in it

| | |
|---|---|
| **Keyboard and mouse** | Real keyboard and mouse input into the game's own controls (no gamepad emulation in play), fully rebindable, with raw mouse look: no smoothing, no acceleration, frame-rate independent. Menus work with the mouse and show keyboard prompts. Xbox and other controllers work too. |
| **Frame limiter and unlock** | Default 60 fps with evenly spaced frames. Set **Frame limit: Unlocked** in the settings window for a fully unlocked frame rate (it also turns vsync off). **F8** switches the limiter off and on in game. The game's own 62 fps cap is lifted when unlocked. |
| **FPS counter** | **F9** shows fps, frame time, 1% low and the current quality level. |
| **Settings menu** | **F4** opens the settings menu: every option below can be changed in game and is saved. |
| **SMAA + FSR** | SMAA anti-aliasing and AMD FSR 1.0 upscaling/sharpening of the final image, each switchable. |
| **Lighter Low level** | Only on the **Low** quality level, the engine's heaviest effects (dynamic lights and shadows, decals, bloom, depth of field, ambient occlusion and similar) are turned off to help weak graphics cards and handhelds. Medium and High are never touched, and the game folder is not modified. It can be switched off in Graphics Settings. |
| **Automatic quality** | Low / Medium / High chosen from your GPU, lowered for the next launch if 60 fps is not held. |
| **Shader and pipeline cache** | Compiled graphics pipelines are remembered on disk, so each area stutters only the first time. |
| **Cleaner image** | Film grain, motion blur and the game's own FXAA are off by default; community resolution-scaling fix applied. |
| **Saves** | Checkpoint saving and loading across restarts works. |

Everything is explained in detail, with every setting name, in **[docs/FEATURES.md](docs/FEATURES.md)**.

## Controls

Keyboard and mouse play goes straight into the game's own input system: it receives real key and
mouse events, so its aiming, cover and weapon logic behave exactly as with a controller. A
controller still works too (Xbox pads, and anything SDL recognises); the game switches between them
as you use them.

| Action | Default keys |
|---|---|
| Move | **W A S D** |
| Look and aim | **Mouse movement** |
| Fire / aim down the sights | **Left mouse button** / **Right mouse button** (hold) |
| Cover, vault, roll, sprint (hold) | **Space** |
| Pick up weapons and ammo, revive, use | **E** |
| Crouch | **Left Ctrl** or **C** |
| Melee, execute a downed enemy | **F** or **V** |
| Reload (hold to rev the chainsaw) | **R** or mouse button 4 |
| Zoom | **T** or middle mouse button |
| Swap weapons | **1**, or the **mouse wheel** |
| Grenade | **2** |
| Weapon slots | **3**, **4** |
| Look at a point of interest | **Q** |
| Tactical command | **H** |
| Walk (slow) | **Z** |
| Pause | **Escape** |
| Scoreboard | **/** |

Every one of these can be changed in **Graphics Settings.exe** (the **Key bindings** button: click an
action, then press the new key or mouse button; right-click adds a second key) or by hand in
`config/gowj.toml`, for example `bind_reload = "R,ThumbMouseButton"`. The list of setting names is
in the file's Controls section. A key can only do one thing: a key used elsewhere moves to the
action you give it.

**In menus** the game has no cursor, so the mouse drives the selection: scroll the **wheel** to move
up and down, **move the mouse** to step in that direction, **left click** to accept and **right
click** to go back. **Enter** accepts, **Backspace** goes back and **Escape** is Start. The button
prompts in menus show keyboard names such as `[Enter]` and `[Esc]` unless a controller is
connected when the game starts (setting `gowj_prompts`: auto, keyboard or controller). The small
button pictures in some in-game hints are part of the game's artwork and still show controller
buttons.

## Known issues

- **Only tested on one PC** (Ryzen 7 5800X3D, RTX 5070 Ti, Windows 11). AMD and Intel graphics and
  handheld PCs are untested. They should pick lower quality settings automatically, but expect
  rough edges and please report what you see.
- **CPU limited.** About 90 to 110 fps unlocked on a Ryzen 7 5800X3D; a slower CPU gives less.
- **For an unlocked frame rate, set Frame limit to Unlocked in Graphics Settings.exe.** F8 in game
  does not turn off vsync, so it may stay at half your monitor's refresh rate.
- **Unlocked can stutter or tear** without a G-Sync or FreeSync monitor. Use the 60 fps limit if so.
- **Short freezes the first time you enter an area.** Graphics pipelines are compiled on first use
  (a fraction of a second, occasionally a little more). They are saved, so the same area is
  smooth afterwards. The cache lives in `%LOCALAPPDATA%\GearsOfWarJudgmentPC\cache` and can be
  several hundred MB.
- **Small hitches in busy scenes**, about one frame in 200.
- **Single-player campaign only.** Multiplayer and Xbox Live features are not available.
- **Menus have no mouse cursor.** The game was not designed for one; the mouse moves the selection.
- **The settings menu (F4) pauses SMAA/FSR while it is open**, so the picture looks softer until you close it.
- Thin, distant geometry (antennas, wires) can shimmer, because there is no temporal anti-aliasing.
- The game renders at 720p on every quality level (1440p was removed because it was too heavy for most PCs).
- DLSS and FSR 3.1/4 temporal upscaling is not in this release. It flickered while moving at the render sizes the game can draw, so it is paused until that is fixed.
- Windows only.

## Building from source

See **[BUILDING.md](BUILDING.md)** for the full steps. In short, you provide two things:

- **`default.xex`** from your own copy of the game (the retail Xbox 360 disc version, title ID
  `4D530A26`), placed at `game/default.xex` in the repository folder
- the **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** (Windows x64, version 0.10.0.9-dev)

then run `scripts\build.ps1 -Sdk <sdk folder>`. The recompiled game code is generated on your machine
from your `default.xex` and is not part of this repository.

## Reporting problems

Open an issue and attach the newest file from the `logs` folder next to the program (and
`gowj_crash.log` if there is one). Say which graphics card and Windows version you use.

## License

The source code in this repository is under the BSD 3-Clause license ([LICENSE](LICENSE)). Third-party
components keep their own licenses ([THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)). The license does not
cover the game itself, which belongs to its owners and is not included.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), the runtime this builds on, which is derived
  from the [Xenia](https://github.com/xenia-project/xenia) project.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the idea and the recompilation approach.
- AMD FidelityFX Super Resolution 1.0 and SMAA (Jimenez et al.), see
  [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
- The Low level engine settings follow the community low-end "Coalesced_INT" mod for Xenia from ModDB. The
  values are applied to your own game files at run time; no game file and no file of that mod is shipped.
- The xenia-canary community patch set for this title (film grain, FXAA and resolution-scaling
  fixes), re-implemented here as switchable settings.
