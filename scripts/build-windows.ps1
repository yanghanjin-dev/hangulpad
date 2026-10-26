param(
    [ValidateSet('Build','Test','Package','GoldenUpdate','VisualTest')][string]$Action = 'Test',
    [string]$ToolchainRoot = 'C:\msys64\ucrt64',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    # Build output root. Default: <source>\build. Point it outside a synced folder
    # (Synology Drive, OneDrive) to avoid syncing thousands of build files, e.g.
    # -BuildRoot "$env:LOCALAPPDATA\hangulpad-build"
    [string]$BuildRoot = '',
    [string]$PythonExe = 'python',
    # Delete the build directory first (use after a failed or changed configuration).
    [switch]$Clean
)
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts as ANSI.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrEmpty($BuildRoot)) { $BuildRoot = Join-Path $projectRoot 'build' }
$toolBin = Join-Path $ToolchainRoot 'bin'
$missing = @()
foreach ($tool in @('cmake.exe','ctest.exe','cpack.exe','gcc.exe','ninja.exe','windres.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $toolBin $tool))) { $missing += $tool }
}
if (-not (Test-Path -LiteralPath (Join-Path $ToolchainRoot 'lib/cmake/libzip'))) { $missing += 'libzip' }
if ($missing.Count -gt 0) {
    Write-Host ''
    Write-Host "Build tools not found in $ToolchainRoot : $($missing -join ', ')" -ForegroundColor Yellow
    if (-not (Test-Path -LiteralPath $ToolchainRoot)) {
        Write-Host '1) Install MSYS2 from https://www.msys2.org/ (default C:\msys64).'
    }
    Write-Host 'Open "MSYS2 UCRT64" from the Start menu and run:'
    Write-Host '  pacman -Syu' -ForegroundColor Cyan
    Write-Host '  pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-libzip' -ForegroundColor Cyan
    Write-Host '(If pacman -Syu closes the window, reopen it and run the second line.)'
    Write-Host 'MSYS2 elsewhere? Pass -ToolchainRoot <path>\ucrt64'
    exit 1
}
$savedPath = $env:PATH
try {
    $env:PATH = "$toolBin;$savedPath"
    $buildDir = Join-Path $BuildRoot "windows-$($Configuration.ToLowerInvariant())"
    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
    # CMake reads '\' in -D values as escape characters (e.g. '\m'): pass '/' paths.
    $cmTool = $toolBin -replace '\\', '/'
    $cmRoot = $ToolchainRoot -replace '\\', '/'
    & "$toolBin/cmake.exe" -S $projectRoot -B $buildDir -G Ninja `
        "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_C_COMPILER=$cmTool/gcc.exe" `
        "-DCMAKE_RC_COMPILER=$cmTool/windres.exe" "-DCMAKE_MAKE_PROGRAM=$cmTool/ninja.exe" `
        "-DCMAKE_PREFIX_PATH=$cmRoot"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed. If it failed before, retry with -Clean.' }
    & "$toolBin/cmake.exe" --build $buildDir --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    if ($Action -eq 'GoldenUpdate') {
        # After an intended reader change only. Files are written with LF, identical to Linux.
        & "$toolBin/cmake.exe" --build $buildDir --target golden-update
        if ($LASTEXITCODE -ne 0) { throw 'Golden update failed.' }
        Write-Host 'tests/golden regenerated. Review the differences before committing.'
    } elseif ($Action -ne 'Build') {
        & "$toolBin/ctest.exe" --test-dir $buildDir --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    if ($Action -eq 'VisualTest') {
        & $PythonExe -X utf8 "$projectRoot/scripts/visual-regression.py" --renderer "$buildDir/layout_render.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Visual regression failed. Review images before updating baselines.' }
    }
    if ($Action -eq 'Package') {
        & "$toolBin/cpack.exe" --config "$buildDir/CPackConfig.cmake" -B "$projectRoot/dist"
        if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
    }
} finally {
    $env:PATH = $savedPath
}
