# Removes the ArborTab native module from every place install.ps1 put it.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/uninstall.ps1 | iex"
#   uninstall.cmd                     (double-click, from an unpacked package)
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-Purge] [-Yes]
#
# -Purge also deletes %LOCALAPPDATA%\ArborTab (logs, cached offsets, config.ini).
param(
  [switch]$Purge,
  [switch]$Yes
)

$ErrorActionPreference = 'Stop'
$ScriptPath = $MyInvocation.MyCommand.Path
if (-not $ScriptPath) {
  # Running from `irm | iex`: elevation needs a file, so save the latest copy and run that.
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor
    [Net.SecurityProtocolType]::Tls12
  $file = Join-Path $env:TEMP ("arbortab-uninstall-" + [Guid]::NewGuid().ToString('N') + '.ps1')
  Invoke-WebRequest -UseBasicParsing -OutFile $file `
    -Uri 'https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/uninstall.ps1'
  $arguments = @{}
  if ($Purge) { $arguments.Purge = $true }
  if ($Yes) { $arguments.Yes = $true }
  & $file @arguments
  return
}
$dataDir = Join-Path $env:LOCALAPPDATA 'ArborTab'
$record = Join-Path $dataDir 'install.json'

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

$paths = @()
if (Test-Path $record) {
  $paths = @((Get-Content $record -Raw | ConvertFrom-Json).paths)
}
# Also look in the usual places, in case the record was lost.
foreach ($dir in @(
    (Join-Path $env:ProgramFiles 'Google\Chrome\Application'),
    (Join-Path ${env:ProgramFiles(x86)} 'Google\Chrome\Application'),
    (Join-Path $env:LOCALAPPDATA 'Google\Chrome\Application'))) {
  if ($dir) { $paths += (Join-Path $dir 'version.dll') }
}
$paths = @($paths | Where-Object { (Test-Path $_) -and (Get-Item $_).VersionInfo.ProductName -eq 'ArborTab' } |
  Select-Object -Unique)

if ($paths.Count -eq 0 -and -not $Purge) { Write-Host 'ArborTab native module is not installed.'; exit 0 }
Write-Host 'Will remove:'
$paths | ForEach-Object { Write-Host "  $_" }
if ($Purge) { Write-Host "  $dataDir" }
if (-not $Yes) {
  $answer = Read-Host 'Continue? [y/N]'
  if ($answer -notmatch '^(y|yes)$') { Write-Host 'Cancelled.'; exit 1 }
}

$needsAdmin = $paths | Where-Object { -not (Test-Writable (Split-Path -Parent $_)) }
if ($needsAdmin -and -not (Test-Admin)) {
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ScriptPath`"", '-Yes')
  if ($Purge) { $arguments += '-Purge' }
  $process = Start-Process powershell -Verb RunAs -ArgumentList $arguments -Wait -PassThru
  exit $process.ExitCode
}

foreach ($path in $paths) {
  $chrome = Join-Path (Split-Path -Parent $path) 'chrome.exe'
  while (Get-Process chrome -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $chrome }) {
    if ($Yes) { throw "Chrome is running from $(Split-Path -Parent $path); quit it and run the uninstaller again." }
    Read-Host 'Chrome is running. Quit it completely, then press Enter'
  }
  Remove-Item -LiteralPath $path -Force
  Write-Host "  removed $path" -ForegroundColor Green
}
if ($Purge) {
  Remove-Item -LiteralPath $dataDir -Recurse -Force -ErrorAction SilentlyContinue
} elseif (Test-Path $record) {
  Remove-Item -LiteralPath $record -Force
}
Write-Host 'Done. Chrome is back to its own tab strip after the next start.' -ForegroundColor Green
exit 0
