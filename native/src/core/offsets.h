// Chrome version detection and offsets table handling.
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace arbortab {

// Offsets file format this build understands; must equal `schema` in resolver/spec.yaml.
// Bump it whenever the module needs entries older files do not have, or the format changes:
// files live under win64/s<schema>/ remotely and offsets\s<schema>\ locally, so a new
// schema never reads a stale cache. See AGENTS.md ("偏移量").
constexpr int kOffsetsSchema = 1;

// Lifecycle of the offsets pipeline, reported to the extension.
enum class OffsetsStatus {
  kResolving,    // reading the cache or downloading
  kWaiting,      // nothing published for this Chrome build yet; retrying periodically
  kReady,        // loaded and validated
  kDisabled,     // downloads disabled by config.ini and nothing cached
  kError,        // chrome.dll unreadable, or every candidate file failed validation
};
const char* OffsetsStatusName(OffsetsStatus status);

struct FunctionEntry {
  uint32_t rva = 0;
  std::vector<uint8_t> prologue;

  // Absolute address inside the mapped module.
  uint8_t* Address(uint8_t* module_base) const { return module_base + rva; }
};

// Position of a bit field inside its storage unit.
struct BitField {
  uint32_t bit = 0;
  uint32_t width = 0;
};

struct Offsets {
  bool loaded = false;
  int schema = 0;
  std::string chrome_version;
  std::string pdb_key;
  std::string pdb_name;
  std::map<std::string, FunctionEntry> functions;
  std::map<std::string, FunctionEntry> hooks;  // subset of functions that may be detoured
  std::map<std::string, std::vector<uint32_t>> vtables;
  std::map<std::string, int> vtable_slots;
  std::map<std::string, uint32_t> fields;
  std::map<std::string, uint32_t> sizes;
  std::map<std::string, int> constants;  // enumerator values (e.g. ui::ColorIds)
  std::map<std::string, BitField> bitfields;
  std::map<std::string, uint32_t> bases;  // "Derived>Base" -> offset of the base subobject
  std::string source;  // "cache" or "download"
};

// Reads the CodeView record from the mapped chrome.dll and returns the symbol-server key
// (32 hex GUID digits + hexadecimal age). Empty string on failure.
std::string PdbKeyFromModule(HMODULE chrome_dll, std::string* pdb_name = nullptr);
// Full chrome version ("156.0.8078.80") from chrome.dll's version resource, empty on failure.
std::string ChromeVersionFromModule(HMODULE chrome_dll);

// Parses and validates an offsets file: it must use kOffsetsSchema and, when expected_pdb_key
// is not empty, describe exactly that chrome.dll build.
bool LoadOffsetsFromFile(const std::wstring& path, const std::string& expected_pdb_key,
                         Offsets& out, std::string& error);

// Synchronous HTTPS GET; returns the HTTP status code, 0 on transport failure.
int HttpGet(const std::wstring& url, std::string& body);

using OffsetsResolvedCallback = void (*)(const Offsets& offsets);
using OffsetsStatusCallback = void (*)(OffsetsStatus status, const std::string& chrome_version);

// Runs the "cache -> download -> retry" pipeline on a background thread. `on_status` reports
// every state change; `on_resolved` fires once, after the offsets are loaded and validated.
// Missing offsets are retried every 30 minutes.
void StartOffsetsResolution(HMODULE chrome_dll, OffsetsResolvedCallback on_resolved,
                            OffsetsStatusCallback on_status);


}  // namespace arbortab
