param(
  [Parameter(Mandatory=$true)][string]$Installer,
  [Parameter(Mandatory=$true)][string]$Output,
  [Parameter(Mandatory=$true)][string]$Version
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$stage1 = Join-Path $Output 'installer'
$stage2 = Join-Path $Output 'chrome'
& 7z x -y "-o$stage1" $Installer | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'First 7-Zip extraction failed' }
$archive = Get-ChildItem -Path $stage1 -Recurse -Filter 'chrome.7z' | Select-Object -First 1
if (-not $archive) { throw 'chrome.7z was not found in installer' }
& 7z x -y "-o$stage2" $archive.FullName | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Second 7-Zip extraction failed' }
$dll = Get-ChildItem -Path $stage2 -Recurse -Filter 'chrome.dll' |
  Where-Object { $_.FullName -like "*$Version*" } | Select-Object -First 1
if (-not $dll) { $dll = Get-ChildItem -Path $stage2 -Recurse -Filter 'chrome.dll' | Select-Object -First 1 }
if (-not $dll) { throw 'chrome.dll was not found after extraction' }
Copy-Item -Force $dll.FullName (Join-Path $Output 'chrome.dll')
Write-Host "chrome.dll=$($dll.FullName)"
