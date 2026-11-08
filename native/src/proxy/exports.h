// version.dll export forwarding.
//
// chrome.exe statically imports VERSION.dll, so a proxy with the same name next to chrome.exe
// is loaded into every Chrome process. We forward all 17 exports to the system DLL; the
// real DLL is loaded from DllMain before any sandbox restrictions apply.
#pragma once

#include <windows.h>

namespace arbortab {

// Loads %SystemRoot%\System32\version.dll. Must be called from DllMain(DLL_PROCESS_ATTACH).
void LoadRealVersionModule();
// Resolves a system version.dll export (nullptr when the module is unavailable).
FARPROC RealVersionProc(const char* name);

}  // namespace arbortab
