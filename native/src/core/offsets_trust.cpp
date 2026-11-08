#include "core/offsets_trust.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/offsets.h"
#include "core/offsets_key.h"
#include "core/paths.h"
#include "json.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace arbortab {
namespace {

// Roots of the offsets branch, tried in order.
constexpr const wchar_t* kMirrors[] = {
    L"https://raw.githubusercontent.com/EvATive7/chrome-plugin-arbortab/offsets/",
    L"https://cdn.jsdelivr.net/gh/EvATive7/chrome-plugin-arbortab@offsets/",
};
constexpr const wchar_t* kRevokedName = L"revoked.json";
constexpr long long kRevocationMaxAgeSeconds = 24 * 60 * 60;

bool Sha256(const std::string& data, uint8_t out[32]) {
  return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                   static_cast<ULONG>(data.size()), out, 32));
}

std::vector<uint8_t> Base64Decode(const std::string& text) {
  static const std::string table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> out;
  uint32_t buffer = 0;
  int bits = 0;
  for (const char ch : text) {
    if (ch == '=' || ch == '\r' || ch == '\n' || ch == ' ') {
      continue;
    }
    const size_t value = table.find(ch);
    if (value == std::string::npos) {
      return {};
    }
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>(buffer >> bits));
    }
  }
  return out;
}

bool VerifySignature(const std::string& data, const std::string& signature_text) {
  const std::vector<uint8_t> signature = Base64Decode(signature_text);
  uint8_t hash[32];
  if (signature.size() != 64 || !Sha256(data, hash)) {
    return false;
  }
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
    return false;
  }
  uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + sizeof(kOffsetsPublicKey)];
  auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
  header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
  header->cbKey = 32;
  memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), kOffsetsPublicKey, sizeof(kOffsetsPublicKey));
  BCRYPT_KEY_HANDLE key = nullptr;
  bool valid = false;
  if (BCRYPT_SUCCESS(BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob,
                                         sizeof(blob), 0))) {
    valid = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, hash, sizeof(hash),
                                                 const_cast<PUCHAR>(signature.data()),
                                                 static_cast<ULONG>(signature.size()), 0));
    BCryptDestroyKey(key);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return valid;
}

std::wstring RevocationPath() {
  const std::wstring dir = DataDir() + L"\\offsets";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\revoked.json";
}

bool ReadFileBytes(const std::wstring& path, std::string& data) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

long long AgeSeconds(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return -1;
  }
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  const auto ticks = [](const FILETIME& time) {
    return (static_cast<long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  };
  return (ticks(now) - ticks(data.ftLastWriteTime)) / 10000000;
}

std::mutex g_refresh_mutex;

}  // namespace

void RefreshRevocationList() {
  std::lock_guard<std::mutex> lock(g_refresh_mutex);
  const std::wstring path = RevocationPath();
  const long long age = AgeSeconds(path);
  if (age >= 0 && age < kRevocationMaxAgeSeconds) {
    return;
  }
  std::string body;
  const FetchResult result = FetchSigned(kRevokedName, body);
  if (result == FetchResult::kMissing) {
    body = R"({"format":1,"sha256":[]})";  // nothing published: nothing is revoked
  } else if (result == FetchResult::kRejected) {
    ARB_ERROR("the published revocation list has no valid signature; keeping the stored one");
    return;
  }
  const std::wstring temp = path + L".tmp";
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
  }
  MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
  ARB_INFO("revocation list refreshed");
}

FetchResult FetchSigned(const std::wstring& name, std::string& body) {
  // config.ini `offsetsMirror` is tried first (testing, self-hosting); signatures still apply.
  std::vector<std::wstring> mirrors;
  const std::string extra = ReadConfigString("offsetsMirror", "");
  if (!extra.empty()) {
    mirrors.emplace_back(extra.begin(), extra.end());
  }
  mirrors.insert(mirrors.end(), std::begin(kMirrors), std::end(kMirrors));
  bool rejected = false;
  for (const std::wstring& mirror : mirrors) {
    const std::wstring url = mirror + name;
    std::string data;
    const int status = HttpGet(url, data);
    if (status != 200 || data.empty()) {
      ARB_WARN("download failed (%d): %ls", status, url.c_str());
      continue;
    }
    std::string signature;
    if (HttpGet(url + L".sig", signature) != 200 || !VerifySignature(data, signature)) {
      // Fail closed: a file that is there but not signed by us is never used.
      ARB_ERROR("rejected %ls: missing or invalid signature", url.c_str());
      rejected = true;
      continue;
    }
    body = std::move(data);
    return FetchResult::kOk;
  }
  return rejected ? FetchResult::kRejected : FetchResult::kMissing;
}

std::string Sha256Hex(const std::string& data) {
  uint8_t hash[32];
  if (!Sha256(data, hash)) {
    return {};
  }
  static const char* digits = "0123456789abcdef";
  std::string hex;
  for (const uint8_t byte : hash) {
    hex += digits[byte >> 4];
    hex += digits[byte & 0xF];
  }
  return hex;
}

bool IsRevoked(const std::string& sha256_hex) {
  std::string text;
  if (sha256_hex.empty() || !ReadFileBytes(RevocationPath(), text)) {
    return false;
  }
  const nlohmann::json list = nlohmann::json::parse(text, nullptr, false);
  if (!list.is_object() || !list.contains("sha256") || !list["sha256"].is_array()) {
    return false;
  }
  for (const auto& entry : list["sha256"]) {
    if (entry.is_string() && entry.get<std::string>() == sha256_hex) {
      return true;
    }
  }
  return false;
}

void RefreshRevocationListAsync() {
  std::thread([] {
    try {
      RefreshRevocationList();
    } catch (...) {
      ARB_ERROR("refreshing the revocation list failed");
    }
  }).detach();
}

}  // namespace arbortab
