# Installs the ArborTab native module: version.dll next to chrome.exe, and the Native Messaging
# bridge (arbortab-bridge.exe) under %LOCALAPPDATA%\ArborTab\bridge, registered for the current
# user. The bridge is how the extension reaches the module of its own browser.
#
# One line (what the settings page shows): downloads the latest native package and installs it
#   powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"
#
# From an unpacked package: install.cmd, or
#   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-ChromeDir <dir>] [-Yes]
#
# The security trade-off is explained where the command is offered (settings page, README);
# running it is the consent. Written for Windows PowerShell 5.1, which every supported Windows
# ships with. Under `irm | iex` the script must never call `exit`: that would close the user's
# own console.
param(
  [string]$ChromeDir,
  [switch]$Yes,  # non-interactive: fail instead of waiting for Chrome to be closed
  [switch]$SkipBridge  # set by the elevated re-run: the bridge belongs to the original user
)

$ErrorActionPreference = 'Stop'
$Repo = 'EvATive7/chrome-plugin-arbortab'
# `irm | iex` cannot pass parameters: a portable Chrome is selected through the environment.
if (-not $ChromeDir -and $env:ARBORTAB_CHROME_DIR) { $ChromeDir = $env:ARBORTAB_CHROME_DIR }
$ScriptPath = $MyInvocation.MyCommand.Path

if (-not $ScriptPath -or -not (Test-Path (Join-Path (Split-Path -Parent $ScriptPath) 'version.dll'))) {
  # Bootstrap: fetch the native package of the latest release and run the copy inside it.
  # ARBORTAB_PACKAGE (a local zip) replaces the download for testing.
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor
    [Net.SecurityProtocolType]::Tls12
  $work = Join-Path $env:TEMP ("arbortab-install-" + [Guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  $zip = Join-Path $work 'native.zip'
  if ($env:ARBORTAB_PACKAGE) {
    Copy-Item -LiteralPath $env:ARBORTAB_PACKAGE -Destination $zip
  } else {
    Write-Host 'Looking up the latest ArborTab release...'
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -UseBasicParsing
    $asset = $release.assets | Where-Object { $_.name -like 'arbortab-native-*-win64.zip' } | Select-Object -First 1
    if (-not $asset) { throw "Release $($release.tag_name) has no native package." }
    Write-Host "Downloading $($asset.name)..."
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -UseBasicParsing
  }
  Expand-Archive -LiteralPath $zip -DestinationPath $work -Force
  Get-ChildItem -LiteralPath $work -File | Unblock-File
  $arguments = @{}
  if ($ChromeDir) { $arguments.ChromeDir = $ChromeDir }
  if ($Yes) { $arguments.Yes = $true }
  if ($SkipBridge) { $arguments.SkipBridge = $true }
  & (Join-Path $work 'install.ps1') @arguments
  return
}

$here = Split-Path -Parent $ScriptPath
$dll = Join-Path $here 'version.dll'
$dataDir = Join-Path $env:LOCALAPPDATA 'ArborTab'
$record = Join-Path $dataDir 'install.json'
$bridgeDir = Join-Path $dataDir 'bridge'
$hostKey = 'HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.arbortab.bridge'

function Test-Writable([string]$dir) {
  $probe = Join-Path $dir ".arbortab-write-test-$PID"
  try {
    [IO.File]::WriteAllText($probe, '')
    Remove-Item -LiteralPath $probe -Force
    return $true
  } catch {
    return $false
  }
}

function Test-Admin {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  return (New-Object Security.Principal.WindowsPrincipal $identity).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-ProductName([string]$path) {
  return (Get-Item -LiteralPath $path).VersionInfo.ProductName
}

function Find-ChromeDirs {
  $candidates = @(
    (Join-Path $env:ProgramFiles 'Google\Chrome\Application'),
    (Join-Path ${env:ProgramFiles(x86)} 'Google\Chrome\Application'),
    (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application')
  )
  return $candidates | Where-Object { $_ -and (Test-Path (Join-Path $_ 'chrome.exe')) } | Select-Object -Unique
}

function Wait-ChromeClosed([string]$dir) {
  $chrome = Join-Path $dir 'chrome.exe'
  while (Get-Process chrome -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $chrome }) {
    if ($Yes) { throw "Chrome is running from $dir; quit it completely and run the installer again." }
    Write-Host "Chrome is running from $dir." -ForegroundColor Yellow
    Read-Host 'Quit Chrome completely (also check the tray for background apps), then press Enter'
  }
}

if ((Get-ProductName $dll) -ne 'ArborTab') { throw "$dll is not an ArborTab build" }
if ($ChromeDir) {
  $targets = @($ChromeDir)
} else {
  $targets = @(Find-ChromeDirs)
}
if ($targets.Count -eq 0) { throw 'No Chrome installation was found. Pass -ChromeDir <folder containing chrome.exe>.' }

Write-Host "Installing the ArborTab native module $((Get-Item $dll).VersionInfo.ProductVersion) for:" -ForegroundColor Green
$targets | ForEach-Object { Write-Host "  $_" }

# Chrome must be closed before anything else, while this console can still ask the user.
foreach ($dir in $targets) { Wait-ChromeClosed $dir }

# The bridge is per user (HKCU + LOCALAPPDATA), so it is installed before any elevation, as the
# user who runs Chrome.
if (-not $SkipBridge) {
  $bridge = Join-Path $here 'arbortab-bridge.exe'
  if ((Get-ProductName $bridge) -ne 'ArborTab') { throw "$bridge is not an ArborTab build" }
  New-Item -ItemType Directory -Force -Path $bridgeDir | Out-Null
  try {
    Copy-Item -LiteralPath $bridge, (Join-Path $here 'com.arbortab.bridge.json') -Destination $bridgeDir -Force
  } catch {
    throw "Cannot replace $bridgeDir\arbortab-bridge.exe: quit every Chrome window (and other Chrome installs) and run the installer again."
  }
  Get-ChildItem -LiteralPath $bridgeDir -File | Unblock-File
  New-Item -Path $hostKey -Force | Out-Null
  Set-Item -Path $hostKey -Value (Join-Path $bridgeDir 'com.arbortab.bridge.json')
  Write-Host "  installed $bridgeDir\arbortab-bridge.exe" -ForegroundColor Green
}

# Folders that are not writable (Program Files) need elevation: re-run this file elevated.
$needsAdmin = $targets | Where-Object { -not (Test-Writable $_) }
if ($needsAdmin -and -not (Test-Admin)) {
  Write-Host 'Chrome is installed for all users: Windows will ask for administrator rights.'
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ScriptPath`"", '-Yes', '-SkipBridge')
  if ($ChromeDir) { $arguments += @('-ChromeDir', "`"$ChromeDir`"") }
  $process = Start-Process powershell -Verb RunAs -ArgumentList $arguments -Wait -PassThru
  if ($process.ExitCode -ne 0) { throw 'The elevated installer failed.' }
  Write-Host 'Done. Start Chrome: the ArborTab settings page should show "Connected".' -ForegroundColor Green
  exit 0
}

foreach ($dir in $targets) {
  $target = Join-Path $dir 'version.dll'
  if ((Test-Path $target) -and (Get-ProductName $target) -ne 'ArborTab') {
    throw "$target exists and is not ArborTab's; refusing to overwrite it."
  }
  Copy-Item -LiteralPath $dll -Destination $target -Force
  Unblock-File -LiteralPath $target
  Write-Host "  installed $target" -ForegroundColor Green
}

New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
$installed = @()
if (Test-Path $record) {
  $installed = @((Get-Content $record -Raw | ConvertFrom-Json).paths)
}
$installed = @($installed + ($targets | ForEach-Object { Join-Path $_ 'version.dll' }) | Select-Object -Unique)
@{ version = (Get-Item $dll).VersionInfo.ProductVersion; paths = $installed } |
  ConvertTo-Json | Set-Content -Encoding UTF8 $record

Write-Host 'Done. Start Chrome: the ArborTab settings page should show "Connected".' -ForegroundColor Green
exit 0
