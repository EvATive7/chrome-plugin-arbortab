# Removes the ArborTab native module (version.dll) and the Native Messaging bridge from every
# place install.ps1 put them.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/uninstall.ps1 | iex"
#   uninstall.cmd                     (double-click, from an unpacked package)
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-Purge] [-Language <tag>] [-Yes]
#
# -Purge also deletes %LOCALAPPDATA%\ArborTab (logs, cached offsets, config.ini).
param(
  [switch]$Purge,
  [string]$Language,  # optional BCP 47 override; otherwise ARBORTAB_LANG or the Windows UI language
  [switch]$Yes,
  [switch]$SkipBridge  # set by the elevated re-run: the bridge belongs to the original user
)

$ErrorActionPreference = 'Stop'
$MessageCatalog = @'
{
  "en": {
    "NotInstalled": "ArborTab native module is not installed.",
    "WillRemove": "Will remove:",
    "BridgeAndRegistration": "{0} (and its Native Messaging registration)",
    "ConfirmTitle": "Confirm removal",
    "ContinuePrompt": "Continue?",
    "ChoiceYes": "&Yes",
    "ChoiceNo": "&No",
    "Cancelled": "Cancelled.",
    "BridgeInUse": "unregistered the bridge; {0} is in use and stays until Chrome exits",
    "Removed": "removed {0}",
    "ChromeRunningNonInteractive": "Chrome is running from {0}; quit it and run the uninstaller again.",
    "CloseChromePrompt": "WARNING: Chrome is running from {0}. Press Enter to close it automatically. Unsaved work in Chrome may be lost.",
    "ChromeCloseFailed": "Chrome could not be closed automatically. Close it manually and run the uninstaller again.",
    "Done": "Done. Chrome is back to its own tab strip after the next start."
  },
  "de": {
    "NotInstalled": "Das ArborTab-Nativmodul ist nicht installiert.",
    "WillRemove": "Folgendes wird entfernt:",
    "BridgeAndRegistration": "{0} (einschließlich Native-Messaging-Registrierung)",
    "ConfirmTitle": "Entfernen bestätigen",
    "ContinuePrompt": "Fortfahren?",
    "ChoiceYes": "&Ja",
    "ChoiceNo": "&Nein",
    "Cancelled": "Abgebrochen.",
    "BridgeInUse": "Bridge wurde deregistriert; {0} wird verwendet und bleibt bestehen, bis Chrome beendet wird",
    "Removed": "entfernt: {0}",
    "ChromeRunningNonInteractive": "Chrome wird aus {0} ausgeführt. Beenden Sie Chrome und führen Sie das Deinstallationsprogramm erneut aus.",
    "CloseChromePrompt": "WARNUNG: Chrome wird aus {0} ausgeführt. Drücken Sie die EINGABETASTE, um Chrome automatisch zu schließen. Nicht gespeicherte Arbeit in Chrome kann verloren gehen.",
    "ChromeCloseFailed": "Chrome konnte nicht automatisch geschlossen werden. Schließen Sie Chrome manuell und führen Sie das Deinstallationsprogramm erneut aus.",
    "Done": "Fertig. Nach dem nächsten Start verwendet Chrome wieder seine eigene Tableiste."
  },
  "es": {
    "NotInstalled": "El módulo nativo de ArborTab no está instalado.",
    "WillRemove": "Se eliminará:",
    "BridgeAndRegistration": "{0} (y su registro de Native Messaging)",
    "ConfirmTitle": "Confirmar eliminación",
    "ContinuePrompt": "¿Desea continuar?",
    "ChoiceYes": "&Sí",
    "ChoiceNo": "&No",
    "Cancelled": "Cancelado.",
    "BridgeInUse": "se anuló el registro del puente; {0} está en uso y permanecerá hasta que Chrome se cierre",
    "Removed": "eliminado: {0}",
    "ChromeRunningNonInteractive": "Chrome se está ejecutando desde {0}. Ciérrelo y vuelva a ejecutar el desinstalador.",
    "CloseChromePrompt": "ADVERTENCIA: Chrome se está ejecutando desde {0}. Pulse Entrar para cerrarlo automáticamente. El trabajo no guardado en Chrome podría perderse.",
    "ChromeCloseFailed": "Chrome no se pudo cerrar automáticamente. Ciérrelo manualmente y vuelva a ejecutar el desinstalador.",
    "Done": "Listo. Chrome volverá a usar su propia barra de pestañas después del próximo inicio."
  },
  "fr": {
    "NotInstalled": "Le module natif ArborTab n’est pas installé.",
    "WillRemove": "Éléments à supprimer :",
    "BridgeAndRegistration": "{0} (et son inscription Native Messaging)",
    "ConfirmTitle": "Confirmer la suppression",
    "ContinuePrompt": "Continuer ?",
    "ChoiceYes": "&Oui",
    "ChoiceNo": "&Non",
    "Cancelled": "Annulé.",
    "BridgeInUse": "le pont a été désinscrit ; {0} est en cours d’utilisation et restera présent jusqu’à la fermeture de Chrome",
    "Removed": "supprimé : {0}",
    "ChromeRunningNonInteractive": "Chrome est en cours d’exécution depuis {0}. Fermez-le puis relancez le programme de désinstallation.",
    "CloseChromePrompt": "AVERTISSEMENT : Chrome est en cours d’exécution depuis {0}. Appuyez sur Entrée pour le fermer automatiquement. Les travaux non enregistrés dans Chrome risquent d’être perdus.",
    "ChromeCloseFailed": "Chrome n’a pas pu être fermé automatiquement. Fermez-le manuellement puis relancez le programme de désinstallation.",
    "Done": "Terminé. Chrome retrouvera sa propre barre d’onglets au prochain démarrage."
  },
  "ja": {
    "NotInstalled": "ArborTab ネイティブモジュールはインストールされていません。",
    "WillRemove": "次の項目を削除します:",
    "BridgeAndRegistration": "{0}（Native Messaging の登録を含む）",
    "ConfirmTitle": "削除の確認",
    "ContinuePrompt": "続行しますか?",
    "ChoiceYes": "&はい",
    "ChoiceNo": "&いいえ",
    "Cancelled": "キャンセルしました。",
    "BridgeInUse": "ブリッジの登録を解除しました。{0} は使用中のため、Chrome が終了するまで残ります",
    "Removed": "削除済み: {0}",
    "ChromeRunningNonInteractive": "Chrome は {0} から実行中です。終了してからアンインストーラーを再実行してください。",
    "CloseChromePrompt": "警告: Chrome は {0} から実行中です。Enter キーを押すと自動的に終了します。Chrome の未保存の作業が失われる可能性があります。",
    "ChromeCloseFailed": "Chrome を自動的に終了できませんでした。手動で終了してからアンインストーラーを再実行してください。",
    "Done": "完了しました。次回起動後、Chrome は標準のタブバーに戻ります。"
  },
  "ko": {
    "NotInstalled": "ArborTab 네이티브 모듈이 설치되어 있지 않습니다.",
    "WillRemove": "다음 항목을 제거합니다:",
    "BridgeAndRegistration": "{0}(Native Messaging 등록 포함)",
    "ConfirmTitle": "제거 확인",
    "ContinuePrompt": "계속하시겠습니까?",
    "ChoiceYes": "&예",
    "ChoiceNo": "&아니요",
    "Cancelled": "취소되었습니다.",
    "BridgeInUse": "브리지 등록을 해제했습니다. {0}은(는) 사용 중이므로 Chrome이 종료될 때까지 유지됩니다",
    "Removed": "제거됨: {0}",
    "ChromeRunningNonInteractive": "Chrome이 {0}에서 실행 중입니다. 종료한 후 제거 프로그램을 다시 실행하세요.",
    "CloseChromePrompt": "경고: Chrome이 {0}에서 실행 중입니다. Enter 키를 누르면 자동으로 종료됩니다. Chrome에서 저장하지 않은 작업이 손실될 수 있습니다.",
    "ChromeCloseFailed": "Chrome을 자동으로 종료하지 못했습니다. 직접 종료한 후 제거 프로그램을 다시 실행하세요.",
    "Done": "완료되었습니다. 다음에 시작하면 Chrome이 기본 탭 표시줄로 돌아갑니다."
  },
  "zh_CN": {
    "NotInstalled": "尚未安装 ArborTab 原生模块。",
    "WillRemove": "将移除：",
    "BridgeAndRegistration": "{0}（及其原生消息注册项）",
    "ConfirmTitle": "确认移除",
    "ContinuePrompt": "是否继续？",
    "ChoiceYes": "&是",
    "ChoiceNo": "&否",
    "Cancelled": "已取消。",
    "BridgeInUse": "已注销桥接程序；{0} 正在使用，将保留到 Chrome 退出",
    "Removed": "已移除：{0}",
    "ChromeRunningNonInteractive": "Chrome 正从 {0} 运行；请退出后重新运行卸载程序。",
    "CloseChromePrompt": "警告：Chrome 正从 {0} 运行。按 Enter 键后将自动关闭 Chrome，未保存的内容可能会丢失。",
    "ChromeCloseFailed": "无法自动关闭 Chrome。请手动退出 Chrome，然后重新运行卸载程序。",
    "Done": "完成。下次启动后，Chrome 将恢复使用自身的标签栏。"
  },
  "zh_TW": {
    "NotInstalled": "尚未安裝 ArborTab 原生模組。",
    "WillRemove": "將移除：",
    "BridgeAndRegistration": "{0}（及其原生訊息註冊項目）",
    "ConfirmTitle": "確認移除",
    "ContinuePrompt": "是否繼續？",
    "ChoiceYes": "&是",
    "ChoiceNo": "&否",
    "Cancelled": "已取消。",
    "BridgeInUse": "已取消註冊橋接程式；{0} 正在使用，將保留到 Chrome 結束",
    "Removed": "已移除：{0}",
    "ChromeRunningNonInteractive": "Chrome 正從 {0} 執行；請結束後重新執行解除安裝程式。",
    "CloseChromePrompt": "警告：Chrome 正從 {0} 執行。按 Enter 鍵後將自動關閉 Chrome，尚未儲存的內容可能會遺失。",
    "ChromeCloseFailed": "無法自動關閉 Chrome。請手動結束 Chrome，然後重新執行解除安裝程式。",
    "Done": "完成。下次啟動後，Chrome 將恢復使用本身的分頁列。"
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
  $arguments.Language = $Language
  if ($Yes) { $arguments.Yes = $true }
  & $file @arguments
  return
}
$dataDir = Join-Path $env:LOCALAPPDATA 'ArborTab'
$record = Join-Path $dataDir 'install.json'
$bridgeDir = Join-Path $dataDir 'bridge'
$hostKey = 'HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.arbortab.bridge'
$hasBridge = -not $SkipBridge -and ((Test-Path $hostKey) -or (Test-Path $bridgeDir))

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
  Write-Host (Msg 'CloseChromePrompt' @($dir)) -ForegroundColor Red
  [void](Read-Host)
  Stop-ChromeProcesses $chrome
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

if ($paths.Count -eq 0 -and -not $hasBridge -and -not $Purge) {
  Write-Host (Msg 'NotInstalled')
  exit 0
}
Write-Host (Msg 'WillRemove')
$paths | ForEach-Object { Write-Host "  $_" }
if ($hasBridge) { Write-Host "  $(Msg 'BridgeAndRegistration' @($bridgeDir))" }
if ($Purge) { Write-Host "  $dataDir" }
if (-not $Yes) {
  $choices = @(
    (New-Object System.Management.Automation.Host.ChoiceDescription (Msg 'ChoiceYes')),
    (New-Object System.Management.Automation.Host.ChoiceDescription (Msg 'ChoiceNo'))
  )
  if ($Host.UI.PromptForChoice((Msg 'ConfirmTitle'), (Msg 'ContinuePrompt'), $choices, 1) -ne 0) {
    Write-Host (Msg 'Cancelled')
    exit 1
  }
}

# Close Chrome while this console can still warn the user. This also releases the bridge before
# its per-user files and registration are removed.
foreach ($path in $paths) { Ensure-ChromeClosed (Split-Path -Parent $path) }

# Per user, so remove the bridge before any elevation.
if ($hasBridge) {
  Remove-Item -Path $hostKey -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $bridgeDir -Recurse -Force -ErrorAction SilentlyContinue
  if (Test-Path $bridgeDir) {
    Write-Host "  $(Msg 'BridgeInUse' @($bridgeDir))" -ForegroundColor Yellow
  } else {
    Write-Host "  $(Msg 'Removed' @($bridgeDir))" -ForegroundColor Green
  }
}

$needsAdmin = $paths | Where-Object { -not (Test-Writable (Split-Path -Parent $_)) }
if ($needsAdmin -and -not (Test-Admin)) {
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ScriptPath`"", '-Yes', '-SkipBridge', '-Language', "`"$Language`"")
  if ($Purge) { $arguments += '-Purge' }
  $process = Start-Process powershell -Verb RunAs -ArgumentList $arguments -Wait -PassThru
  exit $process.ExitCode
}

foreach ($path in $paths) {
  Remove-Item -LiteralPath $path -Force
  Write-Host "  $(Msg 'Removed' @($path))" -ForegroundColor Green
}
if ($Purge) {
  Remove-Item -LiteralPath $dataDir -Recurse -Force -ErrorAction SilentlyContinue
} elseif (Test-Path $record) {
  Remove-Item -LiteralPath $record -Force
}
Write-Host (Msg 'Done') -ForegroundColor Green
exit 0
