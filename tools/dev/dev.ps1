# Rebuild the native DLL and restart the sandbox Chrome with it (see tools/dev/dev.py).
#
#   pwsh -File tools/dev/dev.ps1            # rebuild DLL + restart + reload extension
#   pwsh -File tools/dev/dev.ps1 -NoBuild   # restart only (e.g. after extension-only changes)
param([switch]$NoBuild)

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$arguments = @('run', '--with', 'websocket-client', 'python', (Join-Path $PSScriptRoot 'dev.py'))
if ($NoBuild) { $arguments += '--no-build' }
Push-Location $repo
try {
  & uv @arguments
  exit $LASTEXITCODE
} finally {
  Pop-Location
}
