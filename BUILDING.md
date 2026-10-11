# Building from source

This builds the same two files that are published on the Releases page, from your own copy of the game:

- `GearsOfWarJudgment.exe`: the game as one self-contained file
- `Graphics Settings.exe`: the settings window

**What is and is not in this repository.** The repository holds the source code of the runtime
integration, the settings window, the patch scripts and the manifest. It does **not** hold the
game's recompiled code: that is generated on your machine from your own `default.xex` (about 250 MB
of C++) and is never committed. You therefore need your own copy of the game.

On a 16-thread CPU the whole build, from `default.xex` to the finished exe, takes about 2 to 5
minutes (the first build compiles roughly 250 large generated files) and uses about 700 MB of disk.

## The files you have to provide

**Your own copy of the game.** Use the retail Xbox 360 disc version of *Gears of War: Judgment*
(title ID `4D530A26`), as a folder on your PC (a dump of the disc). The folder contains
`default.xex` and a `GearGame` folder.

- To **build**, copy `default.xex` from that folder to `game\default.xex` in this repository.
- To **play**, the game asks for the whole folder on first start.

**The ReXGlue SDK** (Windows x64, version 0.10.0.9-dev) from <https://github.com/rexglue/rexglue-sdk>.
Unpack it anywhere; the build needs the folder that contains `bin\rexglue.exe`.

You also need these free tools: Visual Studio 2022 (or Build Tools) with the C++ and Clang components, CMake, Ninja and Python. Details below.

## What you need

| | |
|---|---|
| Windows 10 or 11, 64-bit | |
| **Visual Studio 2022** or **Build Tools for Visual Studio 2022** | with the workloads/components **Desktop development with C++**, **C++ Clang tools for Windows**, and a **Windows 10/11 SDK**. Tested with clang 19.1.5, MSVC 14.44 and SDK 10.0.26100. |
| **CMake** 3.25 or newer | |
| **Ninja** | |
| **Python** 3.9 or newer | used by the build for the source patch scripts |
| **ReXGlue SDK** | the runtime this project builds on: <https://github.com/rexglue/rexglue-sdk>. Tested with version **0.10.0.9-dev** (commit `923c1a5`), Windows x64 build. Other versions may need changes. Unpack it somewhere; the folder you need contains `bin\rexglue.exe`. |
| **Your own `default.xex`** | from your copy of Gears of War: Judgment (Xbox 360, retail disc version: title ID `4D530A26`, media ID `3528321A`). Other versions, such as a title-update build, have different code addresses and will not work. |

CMake and Ninja must be on your `PATH`. (The Visual Studio installer can install both, and
`winget install Kitware.CMake Ninja-build.Ninja` works too.)

## Steps

1. Get the source:

   ```
   git clone <this repository>
   cd <this repository>
   ```

2. Put your game executable at **`game\default.xex`** (create the `game` folder). Only this one file
   is needed to build. (The `game` folder is ignored by git.)

3. Run the build script, pointing it at the SDK folder:

   ```
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Sdk C:\path\to\rexglue-sdk
   ```

   (or set the `REXGLUE_SDK` environment variable once and omit `-Sdk`).

4. When it finishes, the two files are in **`out\package\`**. Copy them anywhere together and run
   `GearsOfWarJudgment.exe`. The first start asks for your game folder (the one that contains
   `default.xex` and `GearGame`) and unpacks the program's files; see the README.

`-Clean` deletes `generated\` and `out\` first for a from-scratch build.

## Optional: DLSS and FSR upscalers (GPU plugin, switched off in this release)

The upscalers are switched off in the game (`kTemporalUpscalers` in `src/gowj_quality.h`) and their libraries are not packed. The plugin still builds with them for development. The stock ReXGlue graphics plugin knows nothing about upscalers. The repository carries a patch set
(`gpu\patches`) and the new plugin sources (`gpu\files`) that add them. To build with them, run this
**before** `build.ps1`:

```
powershell -ExecutionPolicy Bypass -File scripts\build-gpu.ps1
```

It clones the ReXGlue SDK at the tested commit (`923c1a5`) into `out\gpu-build`, applies the patches,
fetches the public NVIDIA DLSS and AMD FidelityFX headers, and builds `rexruntime.dll` and
`rexgpu-xenos.dll` into `out\gpu`. The first run needs about 3 GB of disk space and 10 to 20 minutes.
The same step copies the official `nvngx_dlss.dll` and the signed AMD FidelityFX loader and upscaler
DLLs into `out\gpu`. `build.ps1` then packs the two plugin DLLs into the game exe (the vendor DLLs are not copied while the upscalers are off). Without this step the game
builds and runs with the stock plugin; only the upscaler options do nothing. Add `-Dev` for the
development tools (frame dumps, D3D12 debug layer).

## What the script does

You can run the same steps by hand from a *x64 Native Tools Command Prompt for VS 2022*:

```
:: 1. generate C++ from default.xex (writes generated\)
C:\path\to\rexglue-sdk\bin\rexglue.exe codegen gowj_manifest.toml

:: 2. configure
cmake -S . -B out\build\release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
      -DCMAKE_PREFIX_PATH=C:\path\to\rexglue-sdk

:: 3. build and pack
cmake --build out\build\release --target gowj_package
```

The result of step 3 is in `out\build\release\package\`.

During the build, small Python scripts in `tools\` rewrite the generated sources before they are
compiled. They are listed, with what each one does, in `CMakeLists.txt`: they make jump tables
dispatch correctly, turn the game's hard-coded 720p render size into a setting, remove the game's
thumbstick filter, route the mouse into the look controls, force the radial motion blur off, and
apply the community game patches. They are why the generated sources must come from this
repository's `gowj_manifest.toml`.

## How the single exe is made

`GearsOfWarJudgment.exe` is a small launcher (`launcher\stub.cpp`) with the game and its runtime
files appended to it by `launcher\pack.cpp`: the game exe, the two ReXGlue DLLs, the Visual C++
runtime DLLs, the controller database and the default settings, compressed with the Windows LZMS
compressor. On first start the launcher unpacks them to
`%LOCALAPPDATA%\GearsOfWarJudgmentPC\bin\<id>\` and starts the game from there, with
`GOWJ_HOME` pointing at the folder the launcher was started from so that settings, saves and logs
stay next to it. Both launcher programs link the C runtime statically and import only Windows
components, so nothing has to be installed. The Visual C++ runtime DLLs that are embedded are taken from your
Visual Studio installation (`VCToolsRedistDir`); to use another copy, pass
`-DGOWJ_VCRT_DIR=<folder with vcruntime140.dll>` to CMake.

## Troubleshooting

- **`clang was not found`**: in the Visual Studio Installer, modify your installation and add
  *C++ Clang tools for Windows*.
- **`rexglue.exe not found`**: `-Sdk` must be the folder that contains `bin\rexglue.exe`.
- **`game\default.xex is missing`**: copy your game's `default.xex` there.
- **Codegen reports a different title or version**: only the retail disc executable (title ID
  `4D530A26`) is supported.
- **The build stops with a message that a source file is newer than its object**: run the script
  again. The first build can notice the patch scripts rewrote a file after the compiler had started; the second
  build recompiles it. (The script already retries once.)
- **`gowj_package: the Visual C++ runtime DLLs were not found`**: run from a Visual Studio developer
  prompt, or pass `-DGOWJ_VCRT_DIR=...`.
- **`GOWJ_ASYNC_EH`**: the generated code is compiled without asynchronous exception handling because
  it contains no structured exception scopes, which makes it faster. If a future code generator
  emits them, configure with `-DGOWJ_ASYNC_EH=ON`.
