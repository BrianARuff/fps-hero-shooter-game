param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$bootstrap = Join-Path $PSScriptRoot "bootstrap_toolchain.ps1"
& powershell -ExecutionPolicy Bypass -File $bootstrap

$cmakeExe = Join-Path $repoRoot ".toolchain\cmake\cmake-4.2.3-windows-x86_64\bin\cmake.exe"
$buildRoot = Join-Path $repoRoot "build\$Configuration"
$distRoot = Join-Path $repoRoot "dist"
$toolchain = Join-Path $repoRoot "cmake\toolchains\w64devkit-x64.cmake"
$devkitBin = Join-Path $repoRoot ".toolchain\w64devkit\w64devkit\bin"
$ninjaBin = Join-Path $repoRoot ".toolchain\ninja"

$env:PATH = "$devkitBin;$ninjaBin;$env:PATH"

New-Item -ItemType Directory -Force -Path $buildRoot, $distRoot | Out-Null

& $cmakeExe -S $repoRoot -B $buildRoot -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmakeExe --build $buildRoot --config $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$builtExe = Join-Path $buildRoot "Accretion.exe"
$distExe = Join-Path $distRoot "Accretion.exe"
Copy-Item $builtExe $distExe -Force

Write-Host "Built executable:"
Write-Host "  $distExe"
