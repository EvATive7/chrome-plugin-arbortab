// Safe mode: keeps the feature hooks off after the browser repeatedly ended abnormally while
// they were installed. A session file marks "hooks are live in process <pid>"; a clean exit
// removes it, so a file left behind by a dead process counts as one crash. The count is kept
// per (module version, chrome.dll build): a new Chrome or a new module starts from zero.
#pragma once

#include <string>

namespace arbortab {

// Consecutive abnormal endings after which the hooks stay off.
constexpr int kCrashLimit = 2;

struct CrashGuardState {
  int crashes = 0;
  bool safe_mode = false;
};

// Collects the session files of dead browser processes and returns the crash count for this
// build. config.ini `safeMode=0` turns the guard off (crashes are still counted).
CrashGuardState CrashGuardCheck(const std::string& module_version, const std::string& pdb_key);

// Marks this process as running with the hooks installed.
void CrashGuardBeginSession();

// Clean end of the session. Called from DllMain(DLL_PROCESS_DETACH) while the process exits:
// only plain Win32 file calls on preformatted paths.
void CrashGuardEndSession();

// The user asked to try again: forget the crashes of this build.
void CrashGuardReset();

}  // namespace arbortab
