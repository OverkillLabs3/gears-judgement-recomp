<#
.SYNOPSIS
  Builds Gears of War: Judgment (PC) from source.

.DESCRIPTION
  1. Runs `rexglue codegen` on your own game\default.xex (writes generated\, about 250 MB of C++).
  2. Configures CMake (Ninja + clang) and builds.
  3. Collects the runnable program into out\package\.

  Requirements and background are in BUILDING.md.

.PARAMETER Sdk
  Folder of the RexGlue SDK (it contains bin\rexglue.exe and lib\cmake). May also be given
  through the REXGLUE_SDK environment variable.

.PARAMETER Clean
  Delete generated\ and out\ first.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Sdk C:\sdk\rexglue
#>
param(
    [string]$Sdk = $env:REXGLUE_SDK,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

# ---- checks -----------------------------------------------------------------------------
if (-not $Sdk) { Fail 'Give the RexGlue SDK folder with -Sdk <folder> or set REXGLUE_SDK.' }
$Sdk = (Resolve-Path $Sdk).Path
$rexglue = Join-Path $Sdk 'bin\rexglue.exe'
if (-not (Test-Path $rexglue)) { Fail "rexglue.exe not found in $Sdk\bin." }
if (-not (Test-Path 'game\default.xex')) {
    Fail 'game\default.xex is missing. Copy default.xex from your own copy of the game into the game folder.'
}
foreach ($tool in 'cmake', 'ninja', 'python') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool was not found on PATH." }
}

# ---- Visual Studio environment (Windows SDK, resource compiler) and clang ----------------------
Step 'Locating the C++ toolchain'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'Visual Studio 2022 (or Build Tools) is not installed.' }
$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) { Fail 'No Visual Studio installation found.' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { Fail "vcvars64.bat not found in $vsPath. Install the C++ workload." }
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$llvm = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin'
if (-not (Test-Path (Join-Path $llvm 'clang.exe'))) {
    $onPath = Get-Command clang -ErrorAction SilentlyContinue
    if ($onPath) { $llvm = Split-Path $onPath.Source } else {
        Fail 'clang was not found. In the Visual Studio installer add "C++ Clang tools for Windows".'
    }
}
$env:PATH = "$llvm;$env:PATH"
Write-Host "clang: $llvm"

if ($Clean) {
    Step 'Cleaning'
    if (Test-Path generated) { Remove-Item generated -Recurse -Force }
    if (Test-Path out) { Remove-Item out -Recurse -Force }
}

# ---- codegen --------------------------------------------------------------------------------
Step 'Generating C++ from default.xex (about a minute)'
& $rexglue codegen gowj_manifest.toml
if ($LASTEXITCODE -ne 0) { Fail 'rexglue codegen failed.' }

# ---- configure + build ------------------------------------------------------------------------
$build = Join-Path $root 'out\build\release'
Step 'Configuring'
cmake -S . -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_C_COMPILER=$llvm\clang.exe" "-DCMAKE_CXX_COMPILER=$llvm\clang++.exe" `
    "-DCMAKE_LINKER=$llvm\lld-link.exe" "-DCMAKE_PREFIX_PATH=$Sdk"
if ($LASTEXITCODE -ne 0) { Fail 'CMake configure failed.' }

Step 'Building (the first build compiles about 240 large files and takes several minutes)'
cmake --build $build --target gowj_package
if ($LASTEXITCODE -ne 0) {
    # The post-build freshness check fails once if a source patcher ran after ninja planned its
    # work; a second build recompiles what the patchers touched.
    Write-Host 'Build reported a problem; trying once more.' -ForegroundColor Yellow
    cmake --build $build --target gowj_package
    if ($LASTEXITCODE -ne 0) { Fail 'Build failed.' }
}

# ---- package ----------------------------------------------------------------------------------
Step 'Collecting the release files'
$pkg = Join-Path $root 'out\package'
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Path $pkg | Out-Null
foreach ($f in 'GearsOfWarJudgment.exe', 'Graphics Settings.exe') {
    $src = Join-Path $build "package\$f"
    if (-not (Test-Path $src)) { Fail "$f was not produced." }
    Copy-Item $src $pkg
}

# The official upscaler libraries (put into out\gpu by build-gpu.ps1) sit next to the exes, where the
# game and the settings window look for them. Without build-gpu.ps1 there are none and the upscaler
# options simply do nothing.
# Upscalers are switched off in this release (kTemporalUpscalers in src/gowj_quality.h), so their libraries are not shipped.
foreach ($dll in @()) {
    $src = Join-Path $root "out\gpu\$dll"
    if (Test-Path $src) { Copy-Item $src $pkg }
}

Write-Host "`nDone. Release files: $pkg" -ForegroundColor Green
Write-Host '  GearsOfWarJudgment.exe   the game, one self-contained file (no installer, nothing to download)'
Write-Host '  Graphics Settings.exe    settings window; keep it in the same folder as the game exe'
Write-Host 'Run GearsOfWarJudgment.exe. The first launch unpacks its files and asks for your game folder.'
