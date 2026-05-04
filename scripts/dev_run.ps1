<#
.SYNOPSIS
    Build, deploy runtime dependencies, and launch PlotJuggler for local dev.

.DESCRIPTION
    Mirrors what the Windows CI workflow does for a release build, but skips
    the installer step so the inner edit-build-run loop stays fast.

      1. cmake --build (incremental)
      2. windeployqt against plotjuggler.exe and every plugin DLL
         (plugins pull in Qt5OpenGL / Qt5SerialPort / Qt5WebSockets that the
         main EXE doesn't reference, so deploying just the EXE is not enough)
      3. Drop the embeddable CPython distribution next to plotjuggler.exe so
         the Python custom-function feature initializes cleanly without
         depending on whatever Python is installed system-wide.
      4. Launch the EXE.

    Steps 2 and 3 are idempotent and skipped when their outputs already exist.
    Re-run after a Qt upgrade or Python version bump with -ForceDeploy.

.PARAMETER BuildDir
    CMake binary directory. Default: build\PlotJuggler

.PARAMETER QtBin
    Directory containing windeployqt.exe.
    Default: C:\Qt\5.15.2\msvc2019_64\bin

.PARAMETER PythonVersion
    CPython embeddable version to bundle. Must share the same minor version
    as whatever Python.h / pythonXY.lib your build linked against — check
    `_Python3_RUNTIME_LIBRARY_RELEASE` in $BuildDir\CMakeCache.txt if unsure.
    Default: 3.12.10

.PARAMETER SkipBuild
    Skip the cmake --build step (use when iterating on deploy / launch only).

.PARAMETER ForceDeploy
    Re-run windeployqt and re-extract the embeddable Python even if their
    outputs already exist.

.PARAMETER NoLaunch
    Build and deploy but do not start plotjuggler.exe.

.EXAMPLE
    scripts\dev_run.ps1
    Standard dev iteration: incremental build, deploy if needed, launch.

.EXAMPLE
    scripts\dev_run.ps1 -SkipBuild -NoLaunch -ForceDeploy
    Refresh deployed Qt + Python after upgrading Qt without rebuilding.
#>

[CmdletBinding()]
param(
    [string]$BuildDir = "build\PlotJuggler",
    [string]$QtBin = "C:\Qt\5.15.2\msvc2019_64\bin",
    [string]$PythonVersion = "3.12.10",
    [switch]$SkipBuild,
    [switch]$ForceDeploy,
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    $outDir = Join-Path $BuildDir "bin\Release"
    $exe = Join-Path $outDir "plotjuggler.exe"

    # ------------------------------------------------------------------
    # 1. Build
    # ------------------------------------------------------------------
    if (-not $SkipBuild) {
        Write-Host "==> cmake --build $BuildDir" -ForegroundColor Cyan
        & cmake --build $BuildDir --config Release -- /maxcpucount
        if ($LASTEXITCODE -ne 0) { throw "cmake --build failed (exit $LASTEXITCODE)" }
    }

    if (-not (Test-Path $exe)) {
        throw "plotjuggler.exe not found at $exe — has the build ever succeeded?"
    }

    # ------------------------------------------------------------------
    # 2. Qt deploy (EXE + every plugin DLL)
    # ------------------------------------------------------------------
    $windeployqt = Join-Path $QtBin "windeployqt.exe"
    if (-not (Test-Path $windeployqt)) {
        throw "windeployqt.exe not found at $windeployqt — pass -QtBin <path>"
    }

    # Sentinel: Qt5Core.dll lands next to the EXE on first deploy. If it's
    # already there and -ForceDeploy wasn't passed, skip — windeployqt would
    # still scan everything (slow on cold disk) for no behavior change.
    $qtSentinel = Join-Path $outDir "Qt5Core.dll"
    if ($ForceDeploy -or -not (Test-Path $qtSentinel)) {
        Write-Host "==> windeployqt: $exe" -ForegroundColor Cyan
        & $windeployqt --release $exe
        if ($LASTEXITCODE -ne 0) { throw "windeployqt on plotjuggler.exe failed (exit $LASTEXITCODE)" }

        # Plugin DLLs reference Qt modules the EXE doesn't (Qt5OpenGL via
        # qwt_plot_opengl_canvas, Qt5SerialPort via DataStreamSerialPort,
        # Qt5WebSockets via DataStreamWebSocket, …). Deploying each picks
        # those up.
        Get-ChildItem (Join-Path $outDir "*.dll") | ForEach-Object {
            Write-Host "    windeployqt: $($_.Name)"
            & $windeployqt --release $_.FullName | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "windeployqt on $($_.Name) failed (exit $LASTEXITCODE)" }
        }
    } else {
        Write-Host "==> Qt deploy already present (Qt5Core.dll); skipping. Pass -ForceDeploy to re-run." -ForegroundColor DarkGray
    }

    # ------------------------------------------------------------------
    # 3. Embeddable CPython bundle
    # ------------------------------------------------------------------
    $pyTag = ($PythonVersion -split '\.')[0..1] -join ''   # 3.12.10 -> "312"
    $pyDll = Join-Path $outDir "python$pyTag.dll"
    $pyPth = Join-Path $outDir "python$pyTag._pth"

    if ($ForceDeploy -or -not (Test-Path $pyPth)) {
        $url = "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip"
        $zip = Join-Path $env:TEMP "python-$PythonVersion-embed.zip"

        Write-Host "==> Downloading $url" -ForegroundColor Cyan
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

        # If a stray python$pyTag.dll is already there (e.g. the WindowsApps
        # copy), Expand-Archive's -Force still rewrites it cleanly.
        Write-Host "==> Extracting embeddable Python to $outDir"
        Expand-Archive -Path $zip -DestinationPath $outDir -Force

        # Drop the standalone CLI launchers — we only need the embedded
        # interpreter. Saves ~5 MB and avoids confusion about which python
        # is "the" python for this tree.
        Remove-Item (Join-Path $outDir "python.exe"),
                    (Join-Path $outDir "pythonw.exe") -ErrorAction Ignore

        # Comment out `import site` so the interpreter stays in isolated
        # mode: ignores PYTHONHOME / PYTHONPATH / user site-packages and
        # only walks paths listed in the _pth file. Matches CI exactly.
        if (Test-Path $pyPth) {
            (Get-Content $pyPth) -replace '^\s*import site', '#import site' |
                Set-Content $pyPth
        } else {
            throw "python$pyTag._pth missing after extraction — version $PythonVersion may not ship one"
        }

        Write-Host "    bundled $(Split-Path -Leaf $pyDll) + stdlib zip"
    } else {
        Write-Host "==> Embeddable Python already present ($pyPth); skipping. Pass -ForceDeploy to re-run." -ForegroundColor DarkGray
    }

    # ------------------------------------------------------------------
    # 4. Launch
    # ------------------------------------------------------------------
    if (-not $NoLaunch) {
        Write-Host "==> Launching $exe" -ForegroundColor Cyan
        & $exe
    }
}
finally {
    Pop-Location
}
