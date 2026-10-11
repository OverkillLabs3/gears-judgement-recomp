<#
.SYNOPSIS
  Builds the GPU plugin with DLSS and FSR support (optional).

.DESCRIPTION
  The stock ReXGlue SDK plugin does not know about upscalers. This script
    1. clones the ReXGlue SDK at the version this project is built against,
    2. applies gpu\patches\*.patch and copies gpu\files\ into it,
    3. fetches the public NVIDIA DLSS and AMD FidelityFX API headers (nothing is redistributed),
    4. builds rexruntime.dll and rexgpu-xenos.dll into out\gpu\, together with the official nvngx_dlss.dll and
    AMD FidelityFX loader and upscaler DLLs.
  scripts\build.ps1 then picks the two DLLs up automatically. Without them the game still builds
  and runs with the stock plugin; only the upscaler options are unavailable.

  The first run downloads the SDK with its third-party libraries and compiles about 700 files;
  expect 10 to 20 minutes and about 3 GB of disk space in out\gpu-build\.

.PARAMETER Dev
  Also build the development tools (frame tracing, dumps, the D3D12 debug layer switch).

.PARAMETER Clean
  Delete out\gpu-build\ and out\gpu\ first.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build-gpu.ps1
#>
param(
    [switch]$Dev,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# The SDK commit the project and the patches are made for.
$SdkUrl = 'https://github.com/rexglue/rexglue-sdk.git'
$SdkCommit = '923c1a5'

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }
function Run($what, [scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE)." }
}

foreach ($tool in 'git', 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool was not found on PATH." }
}

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

$work = Join-Path $root 'out\gpu-build'
$outDir = Join-Path $root 'out\gpu'
if ($Clean) {
    Step 'Cleaning'
    foreach ($d in $work, $outDir) { if (Test-Path $d) { Remove-Item $d -Recurse -Force } }
}
New-Item -ItemType Directory -Force $work, $outDir | Out-Null

# ---- the SDK ---------------------------------------------------------------------------------
$sdk = Join-Path $work 'rexglue-sdk'
$marker = Join-Path $work 'sdk-patched.txt'
$patchFiles = Get-ChildItem (Join-Path $root 'gpu\patches') -Filter *.patch | Sort-Object Name
$stamp = ($patchFiles | ForEach-Object { (Get-FileHash $_.FullName).Hash }) -join ','
$filesStamp = (Get-ChildItem (Join-Path $root 'gpu\files') -Recurse -File | ForEach-Object { (Get-FileHash $_.FullName).Hash }) -join ','
$want = "$SdkCommit|$stamp|$filesStamp"

if (-not (Test-Path (Join-Path $sdk '.git'))) {
    Step "Cloning the ReXGlue SDK ($SdkCommit)"
    Run 'git clone' { git clone --filter=blob:none $SdkUrl $sdk }
}
if (-not (Test-Path $marker) -or (Get-Content $marker -Raw).Trim() -ne $want) {
    Step 'Preparing the SDK sources'
    Push-Location $sdk
    try {
        Run 'git checkout' { git checkout -f $SdkCommit }
        Run 'git clean' { git clean -fdq -e out }
        Run 'git submodule update' { git submodule update --init --recursive --depth 1 --jobs 8 }
        foreach ($p in $patchFiles) {
            Run "applying $($p.Name)" { git apply --ignore-whitespace --whitespace=nowarn $p.FullName }
        }
    } finally { Pop-Location }
    Copy-Item (Join-Path $root 'gpu\files\*') $sdk -Recurse -Force
    Set-Content $marker $want
}

# ---- public API headers (not redistributed) ----------------------------------------------------
$dlss = Join-Path $work 'dlss-sdk'
if (-not (Test-Path (Join-Path $dlss 'include\nvsdk_ngx.h'))) {
    Step 'Fetching the NVIDIA DLSS SDK headers'
    Run 'git clone DLSS' { git clone --depth 1 https://github.com/NVIDIA/DLSS.git $dlss }
}
$ffx = Join-Path $work 'ffx-sdk'
if (-not (Test-Path (Join-Path $ffx 'Kits\FidelityFX\signedbin\amd_fidelityfx_upscaler_dx12.dll'))) {
    Step 'Fetching the AMD FidelityFX API headers'
    if (-not (Test-Path (Join-Path $ffx '.git'))) {
    Run 'git clone FidelityFX' { git clone --depth 1 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git $ffx }
    }
    Push-Location $ffx
    try {
        Run 'sparse checkout' { git sparse-checkout set Kits/FidelityFX/api/include Kits/FidelityFX/upscalers/include Kits/FidelityFX/signedbin }
    } finally { Pop-Location }
}

# ---- build -------------------------------------------------------------------------------------
Step 'Configuring'
$devFlag = if ($Dev) { 'ON' } else { 'OFF' }
Push-Location $sdk
try {
    Run 'cmake configure' {
        cmake --preset win-amd64 "-DGOWJ_DEV_TOOLS=$devFlag" "-DGOWJ_DLSS_SDK=$($dlss.Replace([string][char]92, '/'))" `
            "-DGOWJ_FFX_SDK=$($ffx.Replace([string][char]92, '/'))/Kits/FidelityFX"
    }
    Step 'Building rexruntime.dll and rexgpu-xenos.dll (this takes a while)'
    Run 'cmake build' { cmake --build --preset win-amd64-release --target rexgpu-xenos }
} finally { Pop-Location }

$built = Join-Path $sdk 'out\win-amd64'
foreach ($dll in 'rexgpu-xenos.dll', 'rexruntime.dll') {
    $src = Get-ChildItem $built -Recurse -Filter $dll | Select-Object -First 1
    if (-not $src) { Fail "$dll was not produced." }
    Copy-Item $src.FullName $outDir -Force
}
# The official, signed runtime libraries the upscalers load next to the exe (redistribution is permitted
# by both vendors; see THIRD_PARTY_NOTICES.txt).
Copy-Item (Join-Path $dlss 'lib\Windows_x86_64\rel\nvngx_dlss.dll') $outDir -Force
foreach ($n in 'loader', 'upscaler') {
    Copy-Item (Join-Path $ffx "Kits\FidelityFX\signedbin\amd_fidelityfx_${n}_dx12.dll") $outDir -Force
}
Step 'Done'
Write-Host "GPU plugin with DLSS / FSR support: $outDir" -ForegroundColor Green
Write-Host 'Run scripts\build.ps1 to build the game with it.'
