ArborTab — 在 Chrome 原生垂直分頁列中使用樹狀分頁

僅支援 Windows x64，需要 Chrome 原生垂直分頁列。完整的視覺整合需要另外從 GitHub 安裝開放原始碼原生模組；安裝前請閱讀下方安全說明。

ArborTab 在 Chrome 自己的垂直分頁列中維護真正的父子樹，不需要側邊欄，也不會再顯示一份重複的分頁清單。

功能：

- 從連結開啟的分頁自動成為來源分頁的子分頁；
- 收合和展開整棵子樹；
- 關閉父分頁時一併關閉所有後代；
- 把分頁拖到另一個分頁上成為其子項，或拖到邊緣調整同層順序；
- 縮排引導線、隱藏分頁計數、主題配色和可選動畫；
- 收合、關閉子樹、增加和減少層級的快速鍵；
- 支援八種介面語言。

只安裝擴充功能也可以維護樹、調整真實分頁順序、使用快速鍵和關閉子樹；但在安裝原生模組前，Chrome 分頁列在視覺上仍是平的。

原生模組安裝：

安裝擴充功能後開啟 ArborTab 設定頁，其中會提供最新 GitHub Release 連結和安裝命令。原始碼、安裝指令碼、軟體包、校驗和及解除安裝方式均公開：
https://github.com/EvATive7/chrome-plugin-arbortab

安裝命令：
powershell -NoExit -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/EvATive7/chrome-plugin-arbortab/releases/latest/download/install.ps1 | iex"

安全說明：

原生元件是安裝在 `chrome.exe` 同目錄的 `version.dll` 代理，會接入 Chrome 垂直分頁列的版面配置、繪製、滑鼠和拖放處理。由於目前散布的 DLL 沒有 Microsoft 簽章，它會為 Chrome 啟動的子行程關閉「只允許 Microsoft 簽章二進位檔」的一項緩解措施，因此降低這些行程的程式碼完整性保護。請只在審閱並接受此取捨後安裝。原生套件提供解除安裝程式。

隱私：

ArborTab 沒有廣告、分析或專案方營運的伺服器，不上傳瀏覽資料。擴充功能透過 Chrome Native Messaging 與本機模組通訊。原生模組會從 GitHub/jsDelivr 下載經過簽章的相容資料，但不會上傳分頁網址、標題或樹。診斷報告只保留在本機，除非使用者主動匯出並分享。
