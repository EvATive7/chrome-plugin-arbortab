# Builds native/version.dll with MSVC (VS 2022 Build Tools + cmake + Ninja).
#
#   pwsh -File tools/dev/build-native.ps1 [-Configuration Release] [-OutDir artifacts/native]
param(
  [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
  [string]$OutDir = 'artifacts/native',
  [switch]$SkipConfigure
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$native = Join-Path $repo 'native'
$build = Join-Path $repo ("build/native-" + $Configuration.ToLower())

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio Build Tools" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'MSVC toolchain not found' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

if (-not $SkipConfigure) {
  cmd /c "`"$vcvars`" >nul && cmake -S `"$native`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration"
  if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
}
cmd /c "`"$vcvars`" >nul && cmake --build `"$build`" --config $Configuration"
if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }

$dll = Join-Path $build 'version.dll'
if (-not (Test-Path $dll)) { throw "expected artifact missing: $dll" }
$target = Join-Path $repo $OutDir
New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item -Force $dll (Join-Path $target 'version.dll')
Copy-Item -Force (Join-Path $build 'arbortab-bridge.exe') (Join-Path $target 'arbortab-bridge.exe')
Write-Host "built $(Join-Path $target 'version.dll')"
