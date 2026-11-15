# Installs the ArborTab native module: version.dll next to chrome.exe, and the Native Messaging
# bridge (arbortab-bridge.exe) under %LOCALAPPDATA%\ArborTab\bridge, registered for the current
# user. The bridge is how the extension reaches the module of its own browser.
#
# One line (what the settings page shows): downloads the latest native package and installs it
#   powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"
#
# From an unpacked package: install.cmd, or
#   powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 [-ChromeDir <dir>] [-Language <tag>] [-Yes]
#
# The security trade-off is explained where the command is offered (settings page, README);
# running it is the consent. Written for Windows PowerShell 5.1, which every supported Windows
# ships with. Under `irm | iex` the script must never call `exit`: that would close the user's
# own console.
param(
  [string]$ChromeDir,
  [string]$Language,  # optional BCP 47 override; otherwise ARBORTAB_LANG or the Windows UI language
  [switch]$Yes,  # non-interactive: fail instead of waiting for Chrome to be closed
  [switch]$SkipBridge  # set by the elevated re-run: the bridge belongs to the original user
)

$ErrorActionPreference = 'Stop'
$MessageCatalog = @'
{
  "en": {
    "LookingUp": "Looking up the latest ArborTab release...",
    "ReleaseNoPackage": "Release {0} has no native package.",
    "Downloading": "Downloading {0}...",
    "ChromeRunningNonInteractive": "Chrome is running from {0}; quit it completely and run the installer again.",
    "ChromeRunning": "Chrome is running from {0}.",
    "CloseChromePrompt": "WARNING: Press Enter to close Chrome automatically. Unsaved work in Chrome may be lost.",
    "ChromeCloseFailed": "Chrome could not be closed automatically. Close it manually and run the installer again.",
    "NotArborTabBuild": "{0} is not an ArborTab build.",
    "NoChromeFound": "No Chrome installation was found. Pass -ChromeDir <folder containing chrome.exe>.",
    "InstallingFor": "Installing the ArborTab native module {0} for:",
    "CannotReplaceBridge": "Cannot replace {0}\\arbortab-bridge.exe: quit every Chrome window (and other Chrome installs) and run the installer again.",
    "Installed": "installed {0}",
    "NeedsAdmin": "Chrome is installed for all users: Windows will ask for administrator rights.",
    "ElevatedFailed": "The elevated installer failed.",
    "Done": "Done. Start Chrome: the ArborTab settings page should show \"Connected\".",
    "RefuseOverwrite": "{0} exists and is not ArborTab's; refusing to overwrite it."
  },
  "de": {
    "LookingUp": "Die neueste ArborTab-Version wird gesucht...",
    "ReleaseNoPackage": "Release {0} enthält kein natives Paket.",
    "Downloading": "{0} wird heruntergeladen...",
    "ChromeRunningNonInteractive": "Chrome wird aus {0} ausgeführt. Beenden Sie Chrome vollständig und führen Sie das Installationsprogramm erneut aus.",
    "ChromeRunning": "Chrome wird aus {0} ausgeführt.",
    "CloseChromePrompt": "WARNUNG: Wenn Sie die EINGABETASTE drücken, wird Chrome automatisch geschlossen. Nicht gespeicherte Arbeit in Chrome kann verloren gehen.",
    "ChromeCloseFailed": "Chrome konnte nicht automatisch geschlossen werden. Schließen Sie Chrome manuell und führen Sie das Installationsprogramm erneut aus.",
    "NotArborTabBuild": "{0} ist kein ArborTab-Build.",
    "NoChromeFound": "Keine Chrome-Installation gefunden. Geben Sie -ChromeDir <Ordner mit chrome.exe> an.",
    "InstallingFor": "ArborTab-Nativmodul {0} wird installiert für:",
    "CannotReplaceBridge": "{0}\\arbortab-bridge.exe kann nicht ersetzt werden. Beenden Sie alle Chrome-Fenster (und andere Chrome-Installationen), und führen Sie das Installationsprogramm erneut aus.",
    "Installed": "installiert: {0}",
    "NeedsAdmin": "Chrome ist für alle Benutzer installiert. Windows fragt nach Administratorrechten.",
    "ElevatedFailed": "Das Installationsprogramm mit erhöhten Rechten ist fehlgeschlagen.",
    "Done": "Fertig. Starten Sie Chrome; auf der ArborTab-Einstellungsseite sollte „Verbunden“ angezeigt werden.",
    "RefuseOverwrite": "{0} ist vorhanden und stammt nicht von ArborTab; die Datei wird nicht überschrieben."
  },
  "es": {
    "LookingUp": "Buscando la versión más reciente de ArborTab...",
    "ReleaseNoPackage": "La versión {0} no tiene un paquete nativo.",
    "Downloading": "Descargando {0}...",
    "ChromeRunningNonInteractive": "Chrome se está ejecutando desde {0}. Ciérrelo por completo y vuelva a ejecutar el instalador.",
    "ChromeRunning": "Chrome se está ejecutando desde {0}.",
    "CloseChromePrompt": "ADVERTENCIA: Al pulsar Entrar, Chrome se cerrará automáticamente. El trabajo no guardado en Chrome podría perderse.",
    "ChromeCloseFailed": "Chrome no se pudo cerrar automáticamente. Ciérrelo manualmente y vuelva a ejecutar el instalador.",
    "NotArborTabBuild": "{0} no es una compilación de ArborTab.",
    "NoChromeFound": "No se encontró ninguna instalación de Chrome. Indique -ChromeDir <carpeta que contiene chrome.exe>.",
    "InstallingFor": "Instalando el módulo nativo de ArborTab {0} para:",
    "CannotReplaceBridge": "No se puede reemplazar {0}\\arbortab-bridge.exe. Cierre todas las ventanas de Chrome (y otras instalaciones de Chrome) y vuelva a ejecutar el instalador.",
    "Installed": "instalado: {0}",
    "NeedsAdmin": "Chrome está instalado para todos los usuarios. Windows solicitará permisos de administrador.",
    "ElevatedFailed": "El instalador con privilegios elevados falló.",
    "Done": "Listo. Inicie Chrome; la página de configuración de ArborTab debería mostrar \"Conectado\".",
    "RefuseOverwrite": "{0} ya existe y no pertenece a ArborTab; no se sobrescribirá."
  },
  "fr": {
    "LookingUp": "Recherche de la dernière version d’ArborTab...",
    "ReleaseNoPackage": "La version {0} ne contient aucun paquet natif.",
    "Downloading": "Téléchargement de {0}...",
    "ChromeRunningNonInteractive": "Chrome est en cours d’exécution depuis {0}. Fermez-le complètement puis relancez le programme d’installation.",
    "ChromeRunning": "Chrome est en cours d’exécution depuis {0}.",
    "CloseChromePrompt": "AVERTISSEMENT : appuyer sur Entrée fermera automatiquement Chrome. Les travaux non enregistrés dans Chrome risquent d’être perdus.",
    "ChromeCloseFailed": "Chrome n’a pas pu être fermé automatiquement. Fermez-le manuellement puis relancez le programme d’installation.",
    "NotArborTabBuild": "{0} n’est pas une version d’ArborTab.",
    "NoChromeFound": "Aucune installation de Chrome n’a été trouvée. Indiquez -ChromeDir <dossier contenant chrome.exe>.",
    "InstallingFor": "Installation du module natif ArborTab {0} pour :",
    "CannotReplaceBridge": "Impossible de remplacer {0}\\arbortab-bridge.exe. Fermez toutes les fenêtres Chrome (ainsi que les autres installations de Chrome), puis relancez le programme d’installation.",
    "Installed": "installé : {0}",
    "NeedsAdmin": "Chrome est installé pour tous les utilisateurs. Windows va demander les droits d’administrateur.",
    "ElevatedFailed": "Le programme d’installation avec élévation de privilèges a échoué.",
    "Done": "Terminé. Démarrez Chrome ; la page des paramètres d’ArborTab devrait afficher « Connecté ».",
    "RefuseOverwrite": "{0} existe déjà et n’appartient pas à ArborTab ; le fichier ne sera pas remplacé."
  },
  "ja": {
    "LookingUp": "最新の ArborTab リリースを確認しています...",
    "ReleaseNoPackage": "リリース {0} にネイティブパッケージがありません。",
    "Downloading": "{0} をダウンロードしています...",
    "ChromeRunningNonInteractive": "Chrome は {0} から実行中です。完全に終了してからインストーラーを再実行してください。",
    "ChromeRunning": "Chrome は {0} から実行中です。",
    "CloseChromePrompt": "警告: Enter キーを押すと Chrome を自動的に終了します。Chrome の未保存の作業が失われる可能性があります。",
    "ChromeCloseFailed": "Chrome を自動的に終了できませんでした。手動で終了してからインストーラーを再実行してください。",
    "NotArborTabBuild": "{0} は ArborTab のビルドではありません。",
    "NoChromeFound": "Chrome のインストール先が見つかりません。-ChromeDir <chrome.exe を含むフォルダー> を指定してください。",
    "InstallingFor": "ArborTab ネイティブモジュール {0} のインストール先:",
    "CannotReplaceBridge": "{0}\\arbortab-bridge.exe を置き換えられません。すべての Chrome ウィンドウ（他の Chrome インストールを含む）を終了してから、インストーラーを再実行してください。",
    "Installed": "インストール済み: {0}",
    "NeedsAdmin": "Chrome はすべてのユーザー向けにインストールされています。Windows が管理者権限を要求します。",
    "ElevatedFailed": "管理者権限でのインストールに失敗しました。",
    "Done": "完了しました。Chrome を起動し、ArborTab の設定ページに「接続済み」と表示されることを確認してください。",
    "RefuseOverwrite": "{0} は既に存在し、ArborTab のファイルではないため上書きしません。"
  },
  "ko": {
    "LookingUp": "최신 ArborTab 릴리스를 확인하는 중...",
    "ReleaseNoPackage": "릴리스 {0}에 네이티브 패키지가 없습니다.",
    "Downloading": "{0} 다운로드 중...",
    "ChromeRunningNonInteractive": "Chrome이 {0}에서 실행 중입니다. 완전히 종료한 후 설치 프로그램을 다시 실행하세요.",
    "ChromeRunning": "Chrome이 {0}에서 실행 중입니다.",
    "CloseChromePrompt": "경고: Enter 키를 누르면 Chrome이 자동으로 종료됩니다. Chrome에서 저장하지 않은 작업이 손실될 수 있습니다.",
    "ChromeCloseFailed": "Chrome을 자동으로 종료하지 못했습니다. 직접 종료한 후 설치 프로그램을 다시 실행하세요.",
    "NotArborTabBuild": "{0}은(는) ArborTab 빌드가 아닙니다.",
    "NoChromeFound": "Chrome 설치를 찾을 수 없습니다. -ChromeDir <chrome.exe가 있는 폴더>를 지정하세요.",
    "InstallingFor": "ArborTab 네이티브 모듈 {0} 설치 대상:",
    "CannotReplaceBridge": "{0}\\arbortab-bridge.exe를 교체할 수 없습니다. 모든 Chrome 창과 다른 Chrome 설치를 종료한 후 설치 프로그램을 다시 실행하세요.",
    "Installed": "설치됨: {0}",
    "NeedsAdmin": "Chrome이 모든 사용자용으로 설치되어 있습니다. Windows에서 관리자 권한을 요청합니다.",
    "ElevatedFailed": "관리자 권한으로 실행한 설치 프로그램이 실패했습니다.",
    "Done": "완료되었습니다. Chrome을 시작하면 ArborTab 설정 페이지에 \"연결됨\"이 표시되어야 합니다.",
    "RefuseOverwrite": "{0}이(가) 이미 존재하며 ArborTab 파일이 아니므로 덮어쓰지 않습니다."
  },
  "zh_CN": {
    "LookingUp": "正在查找最新的 ArborTab 版本...",
    "ReleaseNoPackage": "版本 {0} 没有原生模块安装包。",
    "Downloading": "正在下载 {0}...",
    "ChromeRunningNonInteractive": "Chrome 正从 {0} 运行；请将其完全退出，然后重新运行安装程序。",
    "ChromeRunning": "Chrome 正从 {0} 运行。",
    "CloseChromePrompt": "警告：按 Enter 键后将自动关闭 Chrome。Chrome 中未保存的内容可能会丢失。",
    "ChromeCloseFailed": "无法自动关闭 Chrome。请手动退出 Chrome，然后重新运行安装程序。",
    "NotArborTabBuild": "{0} 不是 ArborTab 构建。",
    "NoChromeFound": "未找到 Chrome 安装。请传入 -ChromeDir <包含 chrome.exe 的文件夹>。",
    "InstallingFor": "正在为以下位置安装 ArborTab 原生模块 {0}：",
    "CannotReplaceBridge": "无法替换 {0}\\arbortab-bridge.exe；请退出所有 Chrome 窗口（包括其他 Chrome 安装），然后重新运行安装程序。",
    "Installed": "已安装：{0}",
    "NeedsAdmin": "Chrome 是为所有用户安装的；Windows 将请求管理员权限。",
    "ElevatedFailed": "以管理员权限运行的安装程序失败。",
    "Done": "完成。请启动 Chrome；ArborTab 设置页应显示“已连接”。",
    "RefuseOverwrite": "{0} 已存在且不属于 ArborTab；拒绝覆盖。"
  },
  "zh_TW": {
    "LookingUp": "正在查詢最新的 ArborTab 版本...",
    "ReleaseNoPackage": "版本 {0} 沒有原生模組安裝套件。",
    "Downloading": "正在下載 {0}...",
    "ChromeRunningNonInteractive": "Chrome 正從 {0} 執行；請將其完全結束，然後重新執行安裝程式。",
    "ChromeRunning": "Chrome 正從 {0} 執行。",
    "CloseChromePrompt": "警告：按 Enter 鍵後將自動關閉 Chrome。Chrome 中尚未儲存的內容可能會遺失。",
    "ChromeCloseFailed": "無法自動關閉 Chrome。請手動結束 Chrome，然後重新執行安裝程式。",
    "NotArborTabBuild": "{0} 不是 ArborTab 組建。",
    "NoChromeFound": "找不到 Chrome 安裝。請傳入 -ChromeDir <包含 chrome.exe 的資料夾>。",
    "InstallingFor": "正在為下列位置安裝 ArborTab 原生模組 {0}：",
    "CannotReplaceBridge": "無法取代 {0}\\arbortab-bridge.exe；請結束所有 Chrome 視窗（包括其他 Chrome 安裝），然後重新執行安裝程式。",
    "Installed": "已安裝：{0}",
    "NeedsAdmin": "Chrome 是為所有使用者安裝的；Windows 將要求系統管理員權限。",
    "ElevatedFailed": "以系統管理員權限執行的安裝程式失敗。",
    "Done": "完成。請啟動 Chrome；ArborTab 設定頁應顯示「已連線」。",
    "RefuseOverwrite": "{0} 已存在且不屬於 ArborTab；拒絕覆寫。"
  }
}
'@ | ConvertFrom-Json

function Resolve-Language([string]$requested) {
  if (-not $requested) { $requested = $env:ARBORTAB_LANG }
  if (-not $requested) { $requested = [Globalization.CultureInfo]::CurrentUICulture.Name }
  $normalized = $requested.Replace('_', '-').ToLowerInvariant()
  if ($normalized -match '^zh-(tw|hk|mo|hant)(-|$)') { return 'zh_TW' }
  if ($normalized -match '^zh') { return 'zh_CN' }
  foreach ($code in @('de', 'en', 'es', 'fr', 'ja', 'ko')) {
    if ($normalized -eq $code -or $normalized.StartsWith("$code-")) { return $code }
  }
  return 'en'
}

$Language = Resolve-Language $Language

function Msg([string]$key, [object[]]$arguments) {
  $locale = $MessageCatalog.PSObject.Properties[$Language].Value
  $property = $locale.PSObject.Properties[$key]
  if (-not $property) { throw "Missing localized message: $Language/$key" }
  $text = [string]$property.Value
  if ($arguments -and $arguments.Count -gt 0) {
    return [string]::Format([Globalization.CultureInfo]::CurrentCulture, $text, $arguments)
  }
  return $text
}
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
    Write-Host (Msg 'LookingUp')
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -UseBasicParsing
    $asset = $release.assets | Where-Object { $_.name -like 'arbortab-native-*-win64.zip' } | Select-Object -First 1
    if (-not $asset) { throw (Msg 'ReleaseNoPackage' @($release.tag_name)) }
    Write-Host (Msg 'Downloading' @($asset.name))
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip -UseBasicParsing
  }
  Expand-Archive -LiteralPath $zip -DestinationPath $work -Force
  Get-ChildItem -LiteralPath $work -File | Unblock-File
  $arguments = @{}
  if ($ChromeDir) { $arguments.ChromeDir = $ChromeDir }
  $arguments.Language = $Language
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

function Get-ChromeProcesses([string]$chrome) {
  return @(Get-Process chrome -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $chrome })
}

function Stop-ChromeProcesses([string]$chrome) {
  $running = @(Get-ChromeProcesses $chrome)
  $deadline = [DateTime]::UtcNow.AddSeconds(5)
  while ($running.Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) {
    foreach ($process in $running) {
      if ($process.MainWindowHandle -ne 0) { [void]$process.CloseMainWindow() }
    }
    Start-Sleep -Milliseconds 200
    $running = @(Get-ChromeProcesses $chrome)
  }
  if ($running.Count -gt 0) {
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
  }

  $deadline = [DateTime]::UtcNow.AddSeconds(5)
  $running = @(Get-ChromeProcesses $chrome)
  while ($running.Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 200
    $running = @(Get-ChromeProcesses $chrome)
  }
  if ($running.Count -gt 0) { throw (Msg 'ChromeCloseFailed') }
}

function Ensure-ChromeClosed([string]$dir) {
  $chrome = Join-Path $dir 'chrome.exe'
  if (@(Get-ChromeProcesses $chrome).Count -eq 0) { return }
  if ($Yes) { throw (Msg 'ChromeRunningNonInteractive' @($dir)) }
  Write-Host (Msg 'ChromeRunning' @($dir)) -ForegroundColor Red
  Write-Host (Msg 'CloseChromePrompt') -ForegroundColor Red
  [void](Read-Host)
  Stop-ChromeProcesses $chrome
}

if ((Get-ProductName $dll) -ne 'ArborTab') { throw (Msg 'NotArborTabBuild' @($dll)) }
if ($ChromeDir) {
  $targets = @($ChromeDir)
} else {
  $targets = @(Find-ChromeDirs)
}
if ($targets.Count -eq 0) { throw (Msg 'NoChromeFound') }

Write-Host (Msg 'InstallingFor' @((Get-Item $dll).VersionInfo.ProductVersion)) -ForegroundColor Green
$targets | ForEach-Object { Write-Host "  $_" }

# Chrome must be closed before anything else, while this console can still ask the user.
foreach ($dir in $targets) { Ensure-ChromeClosed $dir }

# The bridge is per user (HKCU + LOCALAPPDATA), so it is installed before any elevation, as the
# user who runs Chrome.
if (-not $SkipBridge) {
  $bridge = Join-Path $here 'arbortab-bridge.exe'
  if ((Get-ProductName $bridge) -ne 'ArborTab') { throw (Msg 'NotArborTabBuild' @($bridge)) }
  New-Item -ItemType Directory -Force -Path $bridgeDir | Out-Null
  try {
    Copy-Item -LiteralPath $bridge, (Join-Path $here 'com.arbortab.bridge.json') -Destination $bridgeDir -Force
  } catch {
    throw (Msg 'CannotReplaceBridge' @($bridgeDir))
  }
  Get-ChildItem -LiteralPath $bridgeDir -File | Unblock-File
  New-Item -Path $hostKey -Force | Out-Null
  Set-Item -Path $hostKey -Value (Join-Path $bridgeDir 'com.arbortab.bridge.json')
  Write-Host "  $(Msg 'Installed' @((Join-Path $bridgeDir 'arbortab-bridge.exe')))" -ForegroundColor Green
}

# Folders that are not writable (Program Files) need elevation: re-run this file elevated.
$needsAdmin = $targets | Where-Object { -not (Test-Writable $_) }
if ($needsAdmin -and -not (Test-Admin)) {
  Write-Host (Msg 'NeedsAdmin')
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ScriptPath`"", '-Yes', '-SkipBridge', '-Language', "`"$Language`"")
  if ($ChromeDir) { $arguments += @('-ChromeDir', "`"$ChromeDir`"") }
  $process = Start-Process powershell -Verb RunAs -ArgumentList $arguments -Wait -PassThru
  if ($process.ExitCode -ne 0) { throw (Msg 'ElevatedFailed') }
  Write-Host (Msg 'Done') -ForegroundColor Green
  exit 0
}

foreach ($dir in $targets) {
  $target = Join-Path $dir 'version.dll'
  if ((Test-Path $target) -and (Get-ProductName $target) -ne 'ArborTab') {
    throw (Msg 'RefuseOverwrite' @($target))
  }
  Copy-Item -LiteralPath $dll -Destination $target -Force
  Unblock-File -LiteralPath $target
  Write-Host "  $(Msg 'Installed' @($target))" -ForegroundColor Green
}

New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
$installed = @()
if (Test-Path $record) {
  $installed = @((Get-Content $record -Raw | ConvertFrom-Json).paths)
}
$installed = @($installed + ($targets | ForEach-Object { Join-Path $_ 'version.dll' }) | Select-Object -Unique)
@{ version = (Get-Item $dll).VersionInfo.ProductVersion; paths = $installed } |
  ConvertTo-Json | Set-Content -Encoding UTF8 $record

Write-Host (Msg 'Done') -ForegroundColor Green
exit 0
