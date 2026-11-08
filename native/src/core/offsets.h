// Chrome version detection and offsets table handling (PLAN.md section 5.2 / 7).
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace arbortab {

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

bool LoadOffsetsFromFile(const std::wstring& path, Offsets& out, std::string& error);

// Synchronous HTTPS GET; returns the HTTP status code, 0 on transport failure.
int HttpGet(const std::wstring& url, std::string& body);

using OffsetsResolvedCallback = void (*)(bool ok, const Offsets& offsets, const char* error);

// Runs the "cache -> download -> retry" pipeline on a background thread. The callback fires
// at most once per successful load; failures are logged and retried every 30 minutes.
void StartOffsetsResolution(HMODULE chrome_dll, OffsetsResolvedCallback callback);

const wchar_t* OffsetsUrlPrefix();

}  // namespace arbortab
