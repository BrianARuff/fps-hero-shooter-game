param()

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolchainRoot = Join-Path $repoRoot ".toolchain"
$downloadsRoot = Join-Path $toolchainRoot "downloads"
$cmakeRoot = Join-Path $toolchainRoot "cmake"
$ninjaRoot = Join-Path $toolchainRoot "ninja"
$devkitRoot = Join-Path $toolchainRoot "w64devkit"

New-Item -ItemType Directory -Force -Path $downloadsRoot, $cmakeRoot, $ninjaRoot, $devkitRoot | Out-Null

$cmakeExe = Join-Path $cmakeRoot "cmake-4.2.3-windows-x86_64\bin\cmake.exe"
$ninjaExe = Join-Path $ninjaRoot "ninja.exe"
$gppExe = Join-Path $devkitRoot "w64devkit\bin\g++.exe"

if (-not (Test-Path $cmakeExe)) {
    $cmakeZip = Join-Path $downloadsRoot "cmake.zip"
    Invoke-WebRequest "https://github.com/Kitware/CMake/releases/download/v4.2.3/cmake-4.2.3-windows-x86_64.zip" -OutFile $cmakeZip
    tar -xf $cmakeZip -C $cmakeRoot
}

if (-not (Test-Path $ninjaExe)) {
    $ninjaZip = Join-Path $downloadsRoot "ninja.zip"
    Invoke-WebRequest "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip" -OutFile $ninjaZip
    tar -xf $ninjaZip -C $ninjaRoot
}

if (-not (Test-Path $gppExe)) {
    $devkitArchive = Join-Path $downloadsRoot "w64devkit.7z.exe"
    Invoke-WebRequest "https://github.com/skeeto/w64devkit/releases/download/v2.5.0/w64devkit-x64-2.5.0.7z.exe" -OutFile $devkitArchive
    & $devkitArchive "-y" ("-o" + $devkitRoot)
}

Write-Host "Toolchain ready:"
Write-Host "  CMake:  $cmakeExe"
Write-Host "  Ninja:  $ninjaExe"
Write-Host "  G++:    $gppExe"

