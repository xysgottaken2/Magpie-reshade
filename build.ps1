$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$reshade = Join-Path $root "reshade-src"

if (-not (Test-Path (Join-Path $reshade "include\reshade.hpp"))) {
    Write-Host "Downloading ReShade 6.8.0 source..."
    git clone --depth 1 --branch v6.8.0 https://github.com/crosire/reshade.git $reshade
}

$build = Join-Path $root "build"
cmake -S $root -B $build -A x64 -DRESHADE_INCLUDE_DIR="$reshade\include"
cmake --build $build --config Release

Write-Host ""
Write-Host "Built DLL:"
Get-ChildItem $build -Recurse -Filter "MagpieReShadeInput.dll" | Select-Object -ExpandProperty FullName
