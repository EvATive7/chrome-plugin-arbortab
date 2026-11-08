param(
  [Parameter(Mandatory=$true)][string]$Installer,
  [Parameter(Mandatory=$true)][string]$Output,
  [Parameter(Mandatory=$true)][string]$Version
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$stage1 = Join-Path $Output 'installer'
$stage2 = Join-Path $Output 'chrome'

function Show-Tree([string]$Path) {
  if (-not (Test-Path $Path)) { Write-Host "  <missing> $Path"; return }
  Get-ChildItem -Path $Path -Recurse -File | Select-Object -First 40 |
    ForEach-Object { Write-Host ("  {0} ({1} bytes)" -f $_.FullName.Substring($Path.Length), $_.Length) }
}

# 7-Zip returns 1 for warnings (the Chrome installer PE reports a benign checksum warning),
# so only codes above 1 are treated as failures.
& 7z x -y "-o$stage1" $Installer | Out-Host
if ($LASTEXITCODE -gt 1) { throw "First 7-Zip extraction failed ($LASTEXITCODE)" }

function Find-ChromeDll([string]$Root) {
  $found = Get-ChildItem -Path $Root -Recurse -Filter 'chrome.dll' -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*$Version*" } | Select-Object -First 1
  if (-not $found) {
    $found = Get-ChildItem -Path $Root -Recurse -Filter 'chrome.dll' -ErrorAction SilentlyContinue |
      Select-Object -First 1
  }
  return $found
}

$dll = Find-ChromeDll $stage1
if (-not $dll) {
  # The installer payload is an embedded 7z archive (usually .rsrc\BN\CHROME.7Z).
  $archive = Get-ChildItem -Path $stage1 -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ieq 'CHROME.7Z' -or $_.Extension -ieq '.7z' } | Select-Object -First 1
  if (-not $archive) {
    Write-Host 'Extracted installer layout:'
    Show-Tree $stage1
    throw 'no chrome.7z payload was found in the installer'
  }
  Write-Host "payload archive: $($archive.FullName)"
  & 7z x -y "-o$stage2" $archive.FullName | Out-Host
  if ($LASTEXITCODE -gt 1) { throw "Second 7-Zip extraction failed ($LASTEXITCODE)" }
  $dll = Find-ChromeDll $stage2
}
if (-not $dll) {
  Write-Host 'Extracted payload layout:'
  Show-Tree $stage2
  throw 'chrome.dll was not found after extraction'
}
Copy-Item -Force $dll.FullName (Join-Path $Output 'chrome.dll')
Write-Host "chrome.dll=$($dll.FullName)"
