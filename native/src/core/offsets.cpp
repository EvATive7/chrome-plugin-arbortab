#include "core/offsets.h"

#include <winhttp.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "core/log.h"
#include "core/paths.h"
#include "proxy/exports.h"
#include "json.hpp"

namespace arbortab {
namespace {

constexpr const wchar_t* kOffsetsUrlPrefix =
    L"https://raw.githubusercontent.com/EvATive7/chrome-plugin-arbortab/offsets/win64/";
constexpr const wchar_t* kMirrorUrlPrefix =
    L"https://cdn.jsdelivr.net/gh/EvATive7/chrome-plugin-arbortab@offsets/win64/";
constexpr int kRetryMinutes = 30;

std::vector<uint8_t> HexBytes(const std::string& text) {
  std::vector<uint8_t> bytes;
  std::string token;
  std::istringstream stream(text);
  while (stream >> token) {
    if (token.size() > 2) {
      return {};
    }
    try {
      bytes.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
    } catch (...) {
      return {};
    }
  }
  return bytes;
}

std::string JsonString(const nlohmann::json& node) {
  return node.is_string() ? node.get<std::string>() : std::string();
}

std::string JsonField(const nlohmann::json& node, const char* key) {
  const auto found = node.find(key);
  return found != node.end() && found->is_string() ? found->get<std::string>() : std::string();
}

uint32_t ParseHex(const std::string& text, uint32_t fallback = 0) {
  if (text.empty()) {
    return fallback;
  }
  try {
    return static_cast<uint32_t>(std::stoul(text, nullptr, 0));
  } catch (...) {
    return fallback;
  }
}

bool ParseFunction(const nlohmann::json& node, FunctionEntry& entry, std::string& error) {
  if (!node.is_object() || !node.contains("rva")) {
    error = "function entry without rva";
    return false;
  }
  entry.rva = ParseHex(JsonString(node["rva"]));
  if (entry.rva == 0) {
    error = "function entry with unparsable rva";
    return false;
  }
  if (node.contains("prologue")) {
    entry.prologue = HexBytes(JsonString(node["prologue"]));
  }
  return true;
}

bool ReadWholeFile(const std::wstring& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  return true;
}

}  // namespace

const wchar_t* OffsetsUrlPrefix() { return kOffsetsUrlPrefix; }

std::string PdbKeyFromModule(HMODULE chrome_dll, std::string* pdb_name) {
  auto* base = reinterpret_cast<uint8_t*>(chrome_dll);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return {};
  }
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return {};
  }
  const IMAGE_DATA_DIRECTORY& directory =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (!directory.VirtualAddress || !directory.Size) {
    return {};
  }
  auto* debug = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
  const DWORD count = directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
  for (DWORD index = 0; index < count; ++index) {
    if (debug[index].Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
      continue;
    }
    auto* cv = reinterpret_cast<uint8_t*>(base + debug[index].AddressOfRawData);
    const DWORD signature = *reinterpret_cast<DWORD*>(cv);
    if (signature != 0x53445352) {  // 'RSDS'
      continue;
    }
    const auto* guid = reinterpret_cast<GUID*>(cv + 4);
    const DWORD age = *reinterpret_cast<DWORD*>(cv + 20);
    const char* name = reinterpret_cast<const char*>(cv + 24);
    if (pdb_name) {
      *pdb_name = name;
      const size_t slash = pdb_name->find_last_of("\\/");
      if (slash != std::string::npos) {
        *pdb_name = pdb_name->substr(slash + 1);
      }
    }
    char key[64];
    _snprintf_s(key, sizeof(key), _TRUNCATE,
                "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X", guid->Data1, guid->Data2,
                guid->Data3, guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
                guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7], age);
    return key;
  }
  return {};
}

std::string ChromeVersionFromModule(HMODULE chrome_dll) {
  wchar_t path[MAX_PATH] = {0};
  if (!GetModuleFileNameW(chrome_dll, path, MAX_PATH)) {
    return {};
  }
  // The version APIs come from the system version.dll through our own forwarders, so the
  // proxy never has to link against an import library.
  using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
  using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
  using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
  auto get_size = reinterpret_cast<SizeFn>(RealVersionProc("GetFileVersionInfoSizeW"));
  auto get_info = reinterpret_cast<InfoFn>(RealVersionProc("GetFileVersionInfoW"));
  auto query = reinterpret_cast<QueryFn>(RealVersionProc("VerQueryValueW"));
  if (!get_size || !get_info || !query) {
    return {};
  }
  DWORD handle = 0;
  const DWORD size = get_size(path, &handle);
  if (!size) {
    return {};
  }
  std::vector<uint8_t> buffer(size);
  if (!get_info(path, 0, size, buffer.data())) {
    return {};
  }
  VS_FIXEDFILEINFO* info = nullptr;
  UINT length = 0;
  if (!query(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
    return {};
  }
  char version[64];
  _snprintf_s(version, sizeof(version), _TRUNCATE, "%u.%u.%u.%u", info->dwFileVersionMS >> 16,
              info->dwFileVersionMS & 0xFFFF, info->dwFileVersionLS >> 16,
              info->dwFileVersionLS & 0xFFFF);
  return version;
}

bool LoadOffsetsFromFile(const std::wstring& path, Offsets& out, std::string& error) {
  try {
  std::string text;
  if (!ReadWholeFile(path, text)) {
    error = "offsets file is missing";
    return false;
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(text);
  } catch (const std::exception& exception) {
    error = std::string("offsets JSON is invalid: ") + exception.what();
    return false;
  }

  if (!root.is_object()) {
    error = "offsets JSON is not an object";
    return false;
  }

  Offsets parsed;
  const auto number = [&root](const char* key) -> int {
    const auto found = root.find(key);
    return found != root.end() && found->is_number() ? found->get<int>() : 0;
  };
  parsed.schema = number("schema");
  parsed.chrome_version = JsonField(root, "chromeVersion");
  parsed.pdb_key = JsonField(root, "pdbKey");
  parsed.pdb_name = JsonField(root, "pdbName");

  const auto object = [&root](const char* key) -> nlohmann::json {
    const auto found = root.find(key);
    return found != root.end() && found->is_object() ? *found : nlohmann::json::object();
  };

  // Every section lives in a named local: nlohmann's items() proxy must not outlive the
  // container it iterates.
  const nlohmann::json functions = object("functions");
  const nlohmann::json hooks = object("hooks");
  const nlohmann::json vtables = object("vtables");
  const nlohmann::json vtable_slots = object("vtableSlots");
  const nlohmann::json fields = object("fields");
  const nlohmann::json sizes = object("sizes");
  const nlohmann::json constants = object("constants");
  const nlohmann::json bitfields = object("bitfields");
  const nlohmann::json bases = object("bases");

  for (const auto& [name, node] : functions.items()) {
      FunctionEntry entry;
      std::string entry_error;
      if (!ParseFunction(node, entry, entry_error)) {
        error = name + ": " + entry_error;
        return false;
      }
    parsed.functions[name] = entry;
  }

  for (const auto& [name, node] : hooks.items()) {
    const auto found = parsed.functions.find(name);
    if (found == parsed.functions.end()) {
      error = "hook listed without a function entry: " + name;
      return false;
    }
    parsed.hooks[name] = found->second;
  }

  for (const auto& [name, node] : vtables.items()) {
    std::vector<uint32_t> addresses;
    if (node.is_array()) {
      for (const auto& value : node) {
        addresses.push_back(ParseHex(JsonString(value)));
      }
    }
    parsed.vtables[name] = std::move(addresses);
  }

  for (const auto& [name, node] : vtable_slots.items()) {
    parsed.vtable_slots[name] = node.is_number() ? node.get<int>() : 0;
  }
  for (const auto& [name, node] : fields.items()) {
    parsed.fields[name] = ParseHex(JsonString(node));
  }
  for (const auto& [name, node] : sizes.items()) {
    parsed.sizes[name] = ParseHex(JsonString(node));
  }
  for (const auto& [name, node] : constants.items()) {
    if (node.is_number_integer()) {
      parsed.constants[name] = node.get<int>();
    }
  }
  for (const auto& [name, node] : bases.items()) {
    parsed.bases[name] = ParseHex(JsonString(node));
  }
  for (const auto& [name, node] : bitfields.items()) {
    if (node.is_object()) {
      parsed.bitfields[name] = BitField{node.value("bit", 0u), node.value("width", 0u)};
    }
  }

  if (parsed.functions.empty()) {
    error = "offsets table has no functions";
    return false;
  }
  parsed.loaded = true;
  out = std::move(parsed);
  return true;
  } catch (const std::exception& exception) {
    error = std::string("offsets table is malformed: ") + exception.what();
    return false;
  } catch (...) {
    error = "offsets table is malformed";
    return false;
  }
}

int HttpGet(const std::wstring& url, std::string& body) {
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
    return 0;
  }
  const std::wstring host(components.lpszHostName, components.dwHostNameLength);
  std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
  if (components.dwExtraInfoLength) {
    path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }

  HINTERNET session = WinHttpOpen(L"ArborTab/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    return 0;
  }
  int status = 0;
  HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
  if (connection) {
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request) {
      if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                             0, 0) &&
          WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0;
        DWORD code_size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_size,
                            WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        if (status == 200) {
          DWORD available = 0;
          do {
            available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
              break;
            }
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
              break;
            }
            chunk.resize(read);
            body += chunk;
          } while (available > 0);
        }
      }
      WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connection);
  }
  WinHttpCloseHandle(session);
  return status;
}

void StartOffsetsResolution(HMODULE chrome_dll, OffsetsResolvedCallback callback) {
  std::string pdb_name;
  const std::string key = PdbKeyFromModule(chrome_dll, &pdb_name);
  const std::string version = ChromeVersionFromModule(chrome_dll);
  if (key.empty()) {
    ARB_ERROR("chrome.dll has no CodeView record, offsets are unavailable");
    return;
  }
  ARB_INFO("chrome.dll version=%s pdb=%s key=%s", version.c_str(), pdb_name.c_str(), key.c_str());

  auto* base = reinterpret_cast<HMODULE>(chrome_dll);
  std::thread([key, version, callback, base] {
   try {
    for (;;) {
      Offsets offsets;
      std::string error;
      const std::wstring cached = CachedOffsetsPath(key);
      if (LoadOffsetsFromFile(cached, offsets, error)) {
        offsets.source = "cache";
        ARB_INFO("loaded offsets from cache: %ls", cached.c_str());
        callback(true, offsets, nullptr);
        return;
      }

      ARB_WARN("offsets not cached (%s)", error.c_str());
      if (ReadConfigInt("disableDownload", 0) != 0) {
        ARB_WARN("downloads are disabled by config.ini (disableDownload=1); features stay off");
        return;
      }
      ARB_WARN("downloading offsets for key %s", key.c_str());
      const std::wstring wide_key(key.begin(), key.end());
      std::vector<std::wstring> urls = {
          std::wstring(kOffsetsUrlPrefix) + wide_key + L".json",
          std::wstring(kMirrorUrlPrefix) + wide_key + L".json",
      };
      bool downloaded = false;
      for (const std::wstring& url : urls) {
        std::string body;
        const int status = HttpGet(url, body);
        if (status == 200 && !body.empty()) {
          const std::wstring temp = cached + L".tmp";
          {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file.write(body.data(), static_cast<std::streamsize>(body.size()));
          }
          std::string parse_error;
          Offsets candidate;
          if (LoadOffsetsFromFile(temp, candidate, parse_error)) {
            MoveFileExW(temp.c_str(), cached.c_str(), MOVEFILE_REPLACE_EXISTING);
            candidate.source = "download";
            offsets = std::move(candidate);
            downloaded = true;
            ARB_INFO("downloaded offsets for Chrome %s from %ls", version.c_str(), url.c_str());
            break;
          }
          ARB_ERROR("downloaded offsets failed validation: %s", parse_error.c_str());
          DeleteFileW(temp.c_str());
        } else {
          ARB_WARN("offsets download failed (%d): %ls", status, url.c_str());
        }
      }
      if (downloaded) {
        callback(true, offsets, nullptr);
        return;
      }
      ARB_WARN("no offsets for Chrome %s yet; retrying in %d minutes", version.c_str(),
               kRetryMinutes);
      std::this_thread::sleep_for(std::chrono::minutes(kRetryMinutes));
    }
   } catch (const std::exception& exception) {
     // Never let an exception escape: this thread lives inside the browser process.
     ARB_ERROR("offsets worker failed: %s", exception.what());
   } catch (...) {
     ARB_ERROR("offsets worker failed with an unknown exception");
   }
  }).detach();
}

}  // namespace arbortab
