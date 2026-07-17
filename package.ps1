# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds the distributable (VST3-only, static CRT) MicHost and zips it.
# Usage: pwsh -File package.ps1 [-CMake <path-to-cmake.exe>]
param(
    [string] $CMake = "$PSScriptRoot\..\tools\cmake\bin\cmake.exe"
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $CMake)) { $CMake = 'cmake' } # fall back to PATH

$src  = $PSScriptRoot
$dist = Join-Path $src 'build-dist'

& $CMake -S $src -B $dist -G 'Visual Studio 17 2022' -A x64 -DMICHOST_DISABLE_VST2=ON
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }

# The distributable must never link VST2: fail loudly if the configure log
# claims otherwise.
$cache = Get-Content (Join-Path $dist 'CMakeCache.txt') -Raw
if ($cache -notmatch 'MICHOST_DISABLE_VST2:BOOL=ON') { throw 'VST2 was not disabled' }

& $CMake --build $dist --config Release --target MicHost -- /m
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$version = ([regex]::Match((Get-Content (Join-Path $src 'CMakeLists.txt') -Raw),
            'project\(MicHost VERSION ([0-9.]+)')).Groups[1].Value
$exe = Join-Path $dist 'MicHost_artefacts\Release\MicHost.exe'
$zip = Join-Path $src "MicHost-$version-win64.zip"

$staging = Join-Path $dist 'zip-staging'
New-Item -ItemType Directory -Force $staging | Out-Null
Copy-Item $exe, (Join-Path $src 'README.md'), (Join-Path $src 'LICENSE') $staging
Compress-Archive -Path "$staging\*" -DestinationPath $zip -Force

Write-Host "Packaged: $zip"
