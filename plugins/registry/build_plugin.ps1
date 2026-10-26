# SPDX-License-Identifier: MPL-2.0
<#
.SYNOPSIS
  Build one in-tree Airbound plugin on its own and package it as a marketplace ZIP.

.DESCRIPTION
  Builds plugins/ standalone (no Qt, no app): a two-line Conan graph holding
  plotjuggler_sdk and nlohmann_json at exactly the pins of the root
  conanfile.txt (and conan.lock), the same MSVC 2022 toolchain and Conan
  settings as build.bat, then a Release build of plugins/.

  The ZIP has the layout of the official plugin archives:
    <id>/<name>_plugin.dll
    <id>/manifest.json        (the plugin's source manifest)
  and is written to <OutDir>/<id>-<version>-windows-x64.zip, next to a
  <zip>.sha256 file. Under GitHub Actions the id, version, zip path and sha256
  are also written to GITHUB_OUTPUT.

  Windows PowerShell 5.1, ASCII. Needs conan, cmake and ninja on PATH and
  Visual Studio 2022 (the same prerequisites as build.bat).

.PARAMETER Plugin
  Folder under plugins/, e.g. data_load_apbin.

.PARAMETER OutDir
  Where the ZIP goes (default: dist).

.PARAMETER BuildDir
  Scratch build tree (default: build\plugin-<Plugin>, a separate tree inside the
  ignored app build folder).
#>
param(
  [Parameter(Mandatory = $true)][string]$Plugin,
  [string]$OutDir = 'dist',
  [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. (Join-Path $PSScriptRoot '..\..\scripts\windows\PjWindowsCommon.ps1')

$pluginsDir = Join-Path $PjRepoRoot 'plugins'
$manifestPath = Join-Path (Join-Path $pluginsDir $Plugin) 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) {
  Write-Err "No manifest.json in plugins\$Plugin"
  exit 1
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$id = [string]$manifest.id
$version = [string]$manifest.version
if (-not $id -or -not $version) {
  Write-Err "plugins\$Plugin\manifest.json needs an id and a version"
  exit 1
}
if (-not $BuildDir) { $BuildDir = Join-Path $PjBuildDir "plugin-$Plugin" }
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
Write-Host "Building $id $version from plugins\$Plugin into $BuildDir"

Enable-PjMsvc2022Env
Assert-PjBuildTools
Register-PjConanRemote

# Only the plugin SDK and nlohmann_json, copied from the app's conanfile.txt so
# the plugin can never drift from the SDK the app pins.
$requires = @(Get-Content -LiteralPath (Join-Path $PjRepoRoot 'conanfile.txt') |
              Where-Object { $_ -match '^\s*(plotjuggler_sdk|nlohmann_json)/' } |
              ForEach-Object { $_.Trim() })
if ($requires.Count -ne 2) {
  Write-Err 'conanfile.txt must pin plotjuggler_sdk and nlohmann_json exactly once each'
  exit 1
}
$conanfile = "[requires]`r`n" + ($requires -join "`r`n") + "`r`n`r`n[generators]`r`nCMakeDeps`r`nCMakeToolchain`r`n"
[System.IO.File]::WriteAllText((Join-Path $BuildDir 'conanfile.txt'), $conanfile)

# Same settings as build.ps1, except Release: the published DLL carries no debug info.
$conanArgs = @('install', $BuildDir, "--output-folder=$BuildDir", '--build=missing',
               '-r', $PjConanRemoteName, '-r', 'conancenter')
$lockfile = Join-Path $PjRepoRoot 'conan.lock'
if (Test-Path -LiteralPath $lockfile) { $conanArgs += "--lockfile=$lockfile", '--lockfile-partial' }
$conanArgs += @(
  '-s', 'arch=x86_64',
  '-s', 'os=Windows',
  '-s', 'compiler=msvc',
  '-s', 'compiler.version=194',
  '-s', 'compiler.runtime=dynamic',
  '-s', 'compiler.runtime_type=Release',
  '-s', 'compiler.cppstd=20',
  '-s', 'build_type=Release',
  '-c', 'tools.cmake.cmaketoolchain:generator=Ninja'
)
Invoke-Native conan.exe $conanArgs

$cmakeDir = Join-Path $BuildDir 'cmake'
Invoke-Native cmake.exe @('-S', $pluginsDir, '-B', $cmakeDir, '-G', 'Ninja',
                          "-DCMAKE_TOOLCHAIN_FILE=$BuildDir\conan_toolchain.cmake",
                          '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW')
Invoke-Native cmake.exe @('--build', $cmakeDir)

# The plugin's DLL is the one whose generated manifest sidecar carries its id.
$dll = $null
foreach ($sidecar in Get-ChildItem -LiteralPath (Join-Path $cmakeDir 'airbound_plugins') -Filter '*.pjmanifest.json') {
  $built = Get-Content -LiteralPath $sidecar.FullName -Raw | ConvertFrom-Json
  if ([string]$built.id -eq $id) {
    $dll = Join-Path $sidecar.DirectoryName ($sidecar.Name -replace '\.pjmanifest\.json$', '.dll')
  }
}
if (-not $dll -or -not (Test-Path -LiteralPath $dll)) {
  Write-Err "No built DLL declares the id '$id'"
  exit 1
}

# Written with explicit forward-slash entry names: Compress-Archive in Windows
# PowerShell 5.1 stores backslashes, which other unzip implementations misread.
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path (Resolve-Path -LiteralPath $OutDir).Path "$id-$version-windows-x64.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
  $level = [System.IO.Compression.CompressionLevel]::Optimal
  [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $dll, "$id/" + (Split-Path -Leaf $dll), $level)
  [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $manifestPath, "$id/manifest.json", $level)
} finally {
  $archive.Dispose()
}

$sha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$zip.sha256", "$sha256  $(Split-Path -Leaf $zip)`n")
Write-Host "Packaged $zip"
Write-Host "sha256 $sha256"

if ($env:GITHUB_OUTPUT) {
  $outputs = @("id=$id", "version=$version", "zip=$zip", "zip_name=$(Split-Path -Leaf $zip)", "sha256=$sha256")
  [System.IO.File]::AppendAllText($env:GITHUB_OUTPUT, ($outputs -join "`n") + "`n")
}
