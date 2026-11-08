"""Generate a versioned ArborTab offsets table from chrome.dll and its PDB.

Usage:
  uv run --with pefile --with pyyaml python resolver/resolve.py \
    --dll path/to/chrome.dll --pdb path/to/chrome.dll.pdb --out offsets.json

The resolver deliberately fails closed: every requested symbol, field, type size,
prologue and vtable slot must resolve unambiguously before JSON is written.
"""
from __future__ import annotations

import argparse
import ctypes as C
import ctypes.wintypes as W
import datetime as dt
import json
import os
from pathlib import Path
import struct
import sys
from typing import Any

import pefile
import yaml

BASE = 0x180000000
SYMOPT_UNDNAME = 0x00000002
SYMOPT_DEFERRED_LOADS = 0x00000004
SYMOPT_FAIL_CRITICAL_ERRORS = 0x00000200

TI_GET_SYMNAME = 1
TI_GET_LENGTH = 2
TI_FINDCHILDREN = 7
TI_GET_OFFSET = 10
TI_GET_CHILDRENCOUNT = 13
TI_GET_SYMTAG = 0
SYM_TAG_DATA = 7
SYM_TAG_BASE_CLASS = 18
SYM_TAG_FUNCTION = 5
SYM_TAG_PUBLIC = 10


class SYMBOL_INFOW(C.Structure):
    _fields_ = [
        ("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG),
        ("Reserved", C.c_uint64 * 2), ("Index", W.ULONG),
        ("Size", W.ULONG), ("ModBase", C.c_uint64), ("Flags", W.ULONG),
        ("Value", C.c_uint64), ("Address", C.c_uint64),
        ("Register", W.ULONG), ("Scope", W.ULONG), ("Tag", W.ULONG),
        ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG), ("Name", W.WCHAR * 1),
    ]


CALLBACK = C.WINFUNCTYPE(W.BOOL, C.POINTER(SYMBOL_INFOW), W.ULONG, C.c_void_p)


def symbol_name(pointer: C.POINTER(SYMBOL_INFOW)) -> str:
    address = C.addressof(pointer.contents) + SYMBOL_INFOW.Name.offset
    return C.wstring_at(address, pointer.contents.NameLen).rstrip("\0")


class DbgHelp:
    def __init__(self, pdb: Path):
        self.dbg = C.WinDLL(str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "dbghelp.dll"))
        self.process = W.HANDLE(0xA7B07AB)
        self.dbg.SymInitializeW.argtypes = [W.HANDLE, W.LPCWSTR, W.BOOL]
        self.dbg.SymInitializeW.restype = W.BOOL
        self.dbg.SymLoadModuleExW.argtypes = [W.HANDLE, W.HANDLE, W.LPCWSTR, W.LPCWSTR, C.c_uint64, W.DWORD, C.c_void_p, W.DWORD]
        self.dbg.SymLoadModuleExW.restype = C.c_uint64
        self.dbg.SymEnumSymbolsW.argtypes = [W.HANDLE, C.c_uint64, W.LPCWSTR, CALLBACK, C.c_void_p]
        self.dbg.SymEnumSymbolsW.restype = W.BOOL
        self.dbg.SymEnumTypesByNameW.argtypes = [W.HANDLE, C.c_uint64, W.LPCWSTR, CALLBACK, C.c_void_p]
        self.dbg.SymEnumTypesByNameW.restype = W.BOOL
        self.dbg.SymGetTypeInfo.argtypes = [W.HANDLE, C.c_uint64, W.ULONG, C.c_int, C.c_void_p]
        self.dbg.SymGetTypeInfo.restype = W.BOOL
        self.dbg.SymSetOptions(SYMOPT_FAIL_CRITICAL_ERRORS)
        if not self.dbg.SymInitializeW(self.process, None, False):
            raise OSError(C.get_last_error(), "SymInitializeW failed")
        self.module = self.dbg.SymLoadModuleExW(self.process, None, str(pdb), None, BASE, 0x20000000, None, 0)
        if not self.module:
            raise OSError(C.get_last_error(), f"SymLoadModuleExW failed: {pdb}")
        self._public_map: dict[int, list[str]] | None = None

    def set_undecorated(self, enabled: bool) -> None:
        self.dbg.SymSetOptions(SYMOPT_FAIL_CRITICAL_ERRORS | (SYMOPT_UNDNAME if enabled else 0))

    def symbols(self, mask: str, undecorated: bool) -> list[dict[str, Any]]:
        self.set_undecorated(undecorated)
        result: list[dict[str, Any]] = []

        def collect(pointer, _size, _context):
            value = pointer.contents
            result.append({
                "name": symbol_name(pointer),
                "rva": int(value.Address - BASE),
                "size": int(value.Size),
                "tag": int(value.Tag),
                "typeIndex": int(value.TypeIndex),
            })
            return True

        callback = CALLBACK(collect)
        self.dbg.SymEnumSymbolsW(self.process, self.module, mask, callback, None)
        unique = {(x["name"], x["rva"], x["tag"]): x for x in result}
        return list(unique.values())

    def symbols_at(self, rva: int) -> list[str]:
        """All public symbol names that start at exactly this address."""
        return self.public_symbol_map().get(rva, [])

    def public_symbol_map(self) -> dict[int, list[str]]:
        """Map RVA -> public symbol names, enumerated once and cached."""
        if self._public_map is None:
            self.set_undecorated(True)
            mapping: dict[int, set[str]] = {}

            def collect(pointer, _size, _context):
                value = pointer.contents
                mapping.setdefault(int(value.Address - BASE), set()).add(symbol_name(pointer))
                return True

            callback = CALLBACK(collect)
            self.dbg.SymEnumSymbolsW(self.process, self.module, "*", callback, None)
            self._public_map = {key: sorted(names) for key, names in mapping.items()}
        return self._public_map

    def unique_symbol(self, query: str, *, private: bool = False) -> dict[str, Any]:
        matches = self.symbols(query if not private else f"*{query}*", undecorated=private)
        if private:
            matches = [x for x in matches if x["name"] == query]
        else:
            matches = [x for x in matches if x["name"] == query]
        addresses = sorted({x["rva"] for x in matches})
        if len(addresses) != 1:
            details = ", ".join(f"{x['name']}@0x{x['rva']:x}" for x in matches[:10]) or "none"
            raise ValueError(f"symbol must resolve to one address: {query}: {details}")
        preferred = next((x for x in matches if x["tag"] == SYM_TAG_FUNCTION), matches[0])
        return preferred

    def type_indices(self, name: str) -> list[int]:
        self.set_undecorated(True)
        result: list[int] = []

        def collect(pointer, _size, _context):
            result.append(int(pointer.contents.TypeIndex))
            return True

        callback = CALLBACK(collect)
        self.dbg.SymEnumTypesByNameW(self.process, self.module, name, callback, None)
        return list(dict.fromkeys(result))

    def type_info(self, index: int, kind: int, ctype):
        value = ctype()
        if not self.dbg.SymGetTypeInfo(self.process, self.module, index, kind, C.byref(value)):
            return None
        return value.value

    def type_layout(self, name: str) -> tuple[int, dict[str, int]]:
        candidates = []
        for index in self.type_indices(name):
            length = self.type_info(index, TI_GET_LENGTH, C.c_uint64)
            if length:
                candidates.append((index, int(length)))
        if not candidates:
            raise ValueError(f"type not found: {name}")
        # Duplicate forward declarations have no length and were filtered above.
        layouts = []
        for index, length in candidates:
            count = int(self.type_info(index, TI_GET_CHILDRENCOUNT, W.DWORD) or 0)

            class FIND_CHILDREN(C.Structure):
                _fields_ = [("Count", W.ULONG), ("Start", W.ULONG), ("ChildId", W.ULONG * count)]

            children = FIND_CHILDREN(count, 0)
            if count and not self.dbg.SymGetTypeInfo(self.process, self.module, index, TI_FINDCHILDREN, C.byref(children)):
                continue
            fields: dict[str, int] = {}
            for child in children.ChildId:
                tag = self.type_info(child, TI_GET_SYMTAG, W.DWORD)
                if tag not in (SYM_TAG_DATA, SYM_TAG_BASE_CLASS):
                    continue
                name_pointer = C.c_void_p()
                if not self.dbg.SymGetTypeInfo(self.process, self.module, child, TI_GET_SYMNAME, C.byref(name_pointer)):
                    continue
                child_name = C.wstring_at(name_pointer.value) if name_pointer.value else "<base>"
                offset = self.type_info(child, TI_GET_OFFSET, W.DWORD)
                if offset is not None:
                    fields[child_name] = int(offset)
            layouts.append((length, fields))
        unique = {(length, tuple(sorted(fields.items()))) for length, fields in layouts}
        if len(unique) != 1:
            raise ValueError(f"type is ambiguous: {name} ({len(unique)} layouts)")
        return layouts[0]


def pdb_key_and_name(pe: pefile.PE) -> tuple[str, str]:
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"]])
    for debug in pe.DIRECTORY_ENTRY_DEBUG:
        entry = debug.entry
        if hasattr(entry, "Signature_String"):
            pdb_name = entry.PdbFileName.decode(errors="replace").rstrip("\0")
            # pefile's Signature_String is already the symbol-server key: the
            # 32 hex GUID digits followed by the hexadecimal Age. Appending
            # entry.Age again produces a subtly invalid 34-character key.
            return entry.Signature_String.upper(), Path(pdb_name).name
    raise ValueError("chrome.dll has no CodeView debug record")


def chrome_version(pe: pefile.PE) -> str:
    if not getattr(pe, "VS_FIXEDFILEINFO", None):
        pe.parse_data_directories()
    info = pe.VS_FIXEDFILEINFO[0]
    return ".".join(map(str, [info.FileVersionMS >> 16, info.FileVersionMS & 0xFFFF, info.FileVersionLS >> 16, info.FileVersionLS & 0xFFFF]))


def read_rva(pe: pefile.PE, rva: int, count: int) -> bytes:
    offset = pe.get_offset_from_rva(rva)
    data = pe.__data__[offset:offset + count]
    if len(data) != count:
        raise ValueError(f"RVA 0x{rva:x} extends past chrome.dll")
    return data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", required=True, type=Path)
    parser.add_argument("--pdb", required=True, type=Path)
    parser.add_argument("--spec", type=Path, default=Path(__file__).with_name("spec.yaml"))
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    spec = yaml.safe_load(args.spec.read_text(encoding="utf-8"))
    pe = pefile.PE(str(args.dll), fast_load=False)
    key, pdb_name = pdb_key_and_name(pe)
    version = chrome_version(pe)
    symbols = DbgHelp(args.pdb)

    functions: dict[str, dict[str, Any]] = {}
    resolved: dict[str, int] = {}
    hooks: dict[str, bool] = {}
    aliases: dict[int, list[str]] = {}
    for output_name, request in spec["functions"].items():
        is_private = "private" in request
        query = request.get("private") or request["symbol"]
        found = symbols.unique_symbol(query, private=is_private)
        rva = found["rva"]
        resolved[output_name] = rva
        hooks[output_name] = bool(request.get("hook", True))
        functions[output_name] = {
            "rva": f"0x{rva:08x}",
            "prologue": read_rva(pe, rva, 16).hex(" "),
        }
        aliases.setdefault(rva, []).append(output_name)

    # Hooking a function that shares its address with other code would intercept that
    # code too, so every *hooked* function must be a unique entry point. Functions we
    # only call (vtable lookups, throw-away helpers) may legally fold onto other code.
    collisions: dict[str, str] = {}
    hook_targets: dict[str, dict[str, str]] = {}
    for output_name, rva in resolved.items():
        if not hooks.get(output_name, True):
            continue
        others = symbols.symbols_at(rva) if len(aliases[rva]) == 1 else []
        if len(aliases[rva]) > 1:
            collisions[output_name] = "requested more than once"
            continue
        others = [name for name in others if name != output_name]
        if others:
            collisions[output_name] = ", ".join(others[:4])
            continue
        hook_targets[output_name] = {"rva": functions[output_name]["rva"]}
    if collisions:
        details = "; ".join(f"{name} <- {owner}" for name, owner in collisions.items())
        raise ValueError(f"hooked functions share their address with other code: {details}")

    type_cache: dict[str, tuple[int, dict[str, int]]] = {}
    for type_name, expected_size in spec["types"].items():
        layout = symbols.type_layout(type_name)
        type_cache[type_name] = layout
        expected = int(expected_size, 0) if isinstance(expected_size, str) else int(expected_size)
        if layout[0] != expected:
            raise ValueError(f"{type_name} size 0x{layout[0]:x}, expected 0x{expected:x}")

    fields: dict[str, str] = {}
    for output_name, (type_name, field_name) in spec["fields"].items():
        layout = type_cache.setdefault(type_name, symbols.type_layout(type_name))
        if field_name not in layout[1]:
            raise ValueError(f"field not found: {type_name}.{field_name}")
        fields[output_name] = f"0x{layout[1][field_name]:x}"

    vtables: dict[str, list[str]] = {}
    vtable_rvas: dict[str, list[int]] = {}
    for output_name, undecorated_name in spec["vtables"].items():
        owner = undecorated_name.split("::`vftable'")[0].split("::")[-1]
        matches = symbols.symbols(f"*{owner}*vftable*", undecorated=True)
        rvas = sorted({x["rva"] for x in matches if x["name"] == undecorated_name})
        if not rvas:
            raise ValueError(f"vtable not found: {undecorated_name}")
        vtable_rvas[output_name] = rvas
        vtables[output_name] = [f"0x{x:08x}" for x in rvas]

    slots: dict[str, int] = {}
    image_base = int(pe.OPTIONAL_HEADER.ImageBase)
    for output_name, request in spec["vtableSlots"].items():
        target = resolved[request["function"]]
        found_slots = set()
        for table_rva in vtable_rvas[request["vtable"]]:
            values = struct.unpack("<64Q", read_rva(pe, table_rva, 64 * 8))
            found_slots.update(index for index, value in enumerate(values) if value - image_base == target)
        if len(found_slots) != 1:
            raise ValueError(f"vtable slot must be unique: {output_name}: {sorted(found_slots)}")
        slots[output_name] = found_slots.pop()

    output = {
        "schema": int(spec["schema"]),
        "chromeVersion": version,
        "pdbName": pdb_name,
        "pdbKey": key,
        "generatedAt": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "functions": functions,
        "hooks": hook_targets,
        "vtables": vtables,
        "vtableSlots": slots,
        "fields": fields,
        "sizes": {name: f"0x{layout[0]:x}" for name, layout in type_cache.items()},
        "checks": spec["checks"],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    print(f"resolved Chrome {version} ({key}) -> {args.out}")
    print(f"validated {len(functions)} functions, {len(fields)} fields, {len(type_cache)} types, {len(slots)} vtable slots")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"resolver failed: {error}", file=sys.stderr)
        raise
