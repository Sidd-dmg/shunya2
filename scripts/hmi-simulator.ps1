param(
    [ValidateSet("setup", "build", "run", "test", "capture", "clean")]
    [string]$Action = "build",
    [string]$Msys2Root = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$SimulatorRoot = Join-Path $RepoRoot "simulator"
$BuildRoot = Join-Path $SimulatorRoot "build"

function Find-Msys2Root {
    $candidates = @(
        $Msys2Root,
        $env:MSYS2_ROOT,
        "C:\msys64",
        (Join-Path $env:LOCALAPPDATA "Programs\MSYS2"),
        (Join-Path $env:LOCALAPPDATA "MSYS2")
    ) | Where-Object { $_ }

    foreach ($candidate in $candidates) {
        $full = [System.IO.Path]::GetFullPath($candidate)
        if (Test-Path (Join-Path $full "usr\bin\bash.exe")) {
            return $full
        }
    }
    return $null
}

function Invoke-Ucrt64 {
    param([Parameter(Mandatory = $true)][string]$Command)

    $root = Find-Msys2Root
    if (-not $root) {
        throw "MSYS2 was not found. Run '.\scripts\hmi-simulator.ps1 setup' first, or pass -Msys2Root."
    }

    $bash = Join-Path $root "usr\bin\bash.exe"
    $oldSystem = $env:MSYSTEM
    $oldChere = $env:CHERE_INVOKING
    $env:MSYSTEM = "UCRT64"
    $env:CHERE_INVOKING = "1"
    try {
        & $bash -lc "export PATH=/ucrt64/bin:/usr/bin:`$PATH; $Command"
        if ($LASTEXITCODE -ne 0) {
            throw "MSYS2 UCRT64 command failed with exit code $LASTEXITCODE."
        }
    } finally {
        $env:MSYSTEM = $oldSystem
        $env:CHERE_INVOKING = $oldChere
    }
}

function Convert-ToMsysPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $escaped = $Path.Replace("'", "'\''")
    $root = Find-Msys2Root
    $bash = Join-Path $root "usr\bin\bash.exe"
    $result = & $bash -lc "cygpath -u '$escaped'"
    if ($LASTEXITCODE -ne 0 -or -not $result) {
        throw "Could not convert Windows path '$Path' for MSYS2."
    }
    return ($result | Select-Object -First 1)
}

function Assert-Ucrt64Toolchain {
    $root = Find-Msys2Root
    $required = @("cmake.exe", "ninja.exe", "g++.exe", "pkg-config.exe")
    $missing = @(
        $required | Where-Object { -not (Test-Path (Join-Path $root "ucrt64\bin\$_")) }
    )
    if ($missing.Count) {
        throw "MSYS2 UCRT64 is missing $($missing -join ', '). Run '.\scripts\hmi-simulator.ps1 setup' to install the required packages."
    }
    if (-not (Test-Path (Join-Path $root "ucrt64\lib\pkgconfig\sdl2.pc"))) {
        throw "MSYS2 UCRT64 SDL2 development files are missing. Run '.\scripts\hmi-simulator.ps1 setup'."
    }
    if (-not (Test-Path (Join-Path $root "usr\bin\git.exe"))) {
        throw "MSYS2 git is missing. Run '.\scripts\hmi-simulator.ps1 setup' so CMake can fetch pinned LVGL sources."
    }
}

if ($Action -eq "setup") {
    $root = Find-Msys2Root
    if (-not $root) {
        $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
        if (-not $winget) {
            throw "MSYS2 is missing and winget is unavailable. Install MSYS2 from https://www.msys2.org, then rerun setup."
        }
        Write-Host "[hmi-simulator] Installing MSYS2 noninteractively..." -ForegroundColor Cyan
        & $winget.Source install --id MSYS2.MSYS2 --exact --silent `
            --accept-package-agreements --accept-source-agreements --disable-interactivity
        if ($LASTEXITCODE -ne 0) {
            throw "winget could not install MSYS2 (exit $LASTEXITCODE). No compiler packages were changed."
        }
        $root = Find-Msys2Root
        if (-not $root) {
            throw "MSYS2 installation completed but its root was not detected. Re-run with -Msys2Root '<path>'."
        }
    }

    Write-Host "[hmi-simulator] Updating MSYS2 and installing UCRT64 packages..." -ForegroundColor Cyan
    # Two passes handle a fresh installation whose core runtime updates first.
    Invoke-Ucrt64 "pacman -Syu --noconfirm"
    Invoke-Ucrt64 "pacman -Syu --noconfirm"
    Invoke-Ucrt64 "pacman -S --needed --noconfirm git mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-pkgconf"
    Write-Host "[hmi-simulator] Setup complete at $root" -ForegroundColor Green
    exit 0
}

if ($Action -eq "clean") {
    if (Test-Path $BuildRoot) {
        Remove-Item -Recurse -Force $BuildRoot
    }
    Write-Host "[hmi-simulator] Native build directory removed." -ForegroundColor Green
    exit 0
}

Assert-Ucrt64Toolchain
$source = Convert-ToMsysPath $SimulatorRoot
$build = Convert-ToMsysPath $BuildRoot

Write-Host "[hmi-simulator] Configuring LVGL 8.4 native simulator..." -ForegroundColor Cyan
Invoke-Ucrt64 "cmake -S '$source' -B '$build' -G Ninja -DCMAKE_BUILD_TYPE=Debug"
Invoke-Ucrt64 "cmake --build '$build' --parallel"

if ($Action -eq "run") {
    Write-Host "[hmi-simulator] Starting 800x480 simulator..." -ForegroundColor Cyan
    Invoke-Ucrt64 "'$build/shunya-hmi-simulator.exe'"
} elseif ($Action -eq "test") {
    Write-Host "[hmi-simulator] Running native interaction self-test..." -ForegroundColor Cyan
    Invoke-Ucrt64 "'$build/shunya-hmi-simulator.exe' --self-test"
} elseif ($Action -eq "capture") {
    Write-Host "[hmi-simulator] Capturing every 800x480 HMI screen..." -ForegroundColor Cyan
    $hmiRoot = Join-Path $RepoRoot "hmi"
    $hmiRootMsys = Convert-ToMsysPath $hmiRoot
    Invoke-Ucrt64 "'$build/shunya-hmi-simulator.exe' --capture-all '$hmiRootMsys'"

    Add-Type -AssemblyName System.Drawing
    foreach ($name in @(
        "dashboard", "node-trend", "relays", "sensors",
        "programs-queue", "programs-automation", "system"
    )) {
        $bmp = Join-Path $hmiRoot "$name.bmp"
        $png = Join-Path $hmiRoot "$name.png"
        $image = [System.Drawing.Image]::FromFile($bmp)
        try {
            $image.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $image.Dispose()
        }
        Remove-Item -Force $bmp
    }
    Write-Host "[hmi-simulator] PNG screenshots updated under hmi\." -ForegroundColor Green
}
