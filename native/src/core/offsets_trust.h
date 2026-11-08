// Where offsets come from and why they are trusted: every file on the offsets branch carries a
// detached ECDSA P-256 signature (<name>.sig, made by tools/ci/offsets_sign.py, checked with
// core/offsets_key.h), and revoked.json lists the SHA-256 of files that must not be used even
// though they are signed (a bad table that was published).
#pragma once

#include <string>

namespace arbortab {

enum class FetchResult {
  kOk,        // downloaded and the signature verified
  kMissing,   // no mirror has the file (not published yet, or offline)
  kRejected,  // a mirror served it, but unsigned or with a bad signature
};

// Downloads `name` (a path below the offsets branch root, e.g. "win64/s2/<key>.json") and its
// signature from the mirrors in order, returning the first copy that verifies.
FetchResult FetchSigned(const std::wstring& name, std::string& body);

// Lower-case hex SHA-256 of `data`.
std::string Sha256Hex(const std::string& data);

// Whether a file with this SHA-256 is on the locally stored revocation list.
bool IsRevoked(const std::string& sha256_hex);

// Refreshes the stored revocation list when it is older than a day. The async form is used at
// start-up, so a cached table is never held up by the network: a newly revoked table is then
// dropped at the next browser start (the crash guard covers tables that crash). Before a
// download, which needs the network anyway, the list is refreshed synchronously.
void RefreshRevocationList();
void RefreshRevocationListAsync();

}  // namespace arbortab
