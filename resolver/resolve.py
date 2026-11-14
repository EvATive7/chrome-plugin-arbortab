"""Generate a versioned ArborTab offsets table from chrome.dll and its PDB.

Usage:
  uv run --with pefile --with pyyaml python resolver/resolve.py \
    --dll path/to/chrome.dll --pdb path/to/chrome.dll.pdb --out offsets.json

The resolver deliberately fails closed: every requested symbol, field, type size,
prologue and vtable slot must resolve unambiguously before JSON is written.

Progress goes to stdout as it happens (one line per resolved entry, timestamped from the
start); on GitHub Actions each stage is folded into a log group.
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes as C
import ctypes.wintypes as W
import datetime as dt
import json
import os
from pathlib import Path
import struct
import sys
import threading
import time
from typing import Any, Iterator

import pefile
import yaml

BASE = 0x180000000
SYMOPT_UNDNAME = 0x00000002
SYMOPT_DEFERRED_LOADS = 0x00000004
SYMOPT_FAIL_CRITICAL_ERRORS = 0x00000200
SYMOPT_PUBLICS_ONLY = 0x00004000

TI_GET_SYMNAME = 1
TI_GET_LENGTH = 2
TI_FINDCHILDREN = 7
TI_GET_OFFSET = 10
TI_GET_CHILDRENCOUNT = 13
TI_GET_TYPEID = 4
TI_GET_VALUE = 11
TI_GET_BITPOSITION = 14
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


class Progress:
    """Timestamped progress lines; remembers where it is so a failure can say so."""

    def __init__(self) -> None:
        self.start = time.monotonic()
        self.grouped = os.environ.get("GITHUB_ACTIONS") == "true"
        self.stage = "startup"
        self.item = ""
        self.passed = 0

    def log(self, message: str) -> None:
        print(f"[{time.monotonic() - self.start:6.1f}s] {message}", flush=True)

    def entry(self, name: str, detail: str) -> None:
        """One resolved and validated entry."""
        self.passed += 1
        self.log(f"  {name:<58} {detail}")

    @contextlib.contextmanager
    def section(self, title: str, count: int | None = None) -> Iterator[None]:
        self.stage, self.item = title, ""
        heading = f"{title} ({count})" if count is not None else title
        if self.grouped:
            print(f"::group::{heading}", flush=True)
        self.log(heading)
        began = time.monotonic()
        try:
            yield
        finally:
            self.log(f"{title}: {time.monotonic() - began:.1f}s")
            if self.grouped:
                print("::endgroup::", flush=True)

    @contextlib.contextmanager
    def waiting(self, what: str, every: float = 30.0) -> Iterator[None]:
        """For single blocking calls (dbghelp releases the GIL): show that it is still alive."""
        stop = threading.Event()
        began = time.monotonic()

        def beat() -> None:
            while not stop.wait(every):
                self.log(f"  still {what} ({time.monotonic() - began:.0f}s)")

        thread = threading.Thread(target=beat, daemon=True)
        thread.start()
        try:
            yield
        finally:
            stop.set()
            thread.join()


PROGRESS = Progress()


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

    def scan(self, *, undecorated: bool, publics_only: bool, wanted: set[str], by_address: bool
             ) -> tuple[dict[str, set[int]], dict[int, set[str]]]:
        """One pass over every symbol of the module (or every public symbol).

        Returns the addresses of each name in `wanted` (exact match) and, with `by_address`,
        every name per address. The cost is dominated by dbghelp walking the symbol stream
        (100 s or more cold for chrome.dll), not by the matching here, so all lookups of one
        name mode share one pass instead of scanning once per name.

        A full pass does not return the public symbol of an address that also has a private
        function symbol; a publics-only pass does, like a masked lookup.
        """
        self.dbg.SymSetOptions(SYMOPT_FAIL_CRITICAL_ERRORS | (SYMOPT_UNDNAME if undecorated else 0)
                               | (SYMOPT_PUBLICS_ONLY if publics_only else 0))
        found: dict[str, set[int]] = {name: set() for name in wanted}
        names_at: dict[int, set[str]] = {}

        def collect(pointer, _size, _context):
            name = symbol_name(pointer)
            rva = int(pointer.contents.Address - BASE)
            if name in found:
                found[name].add(rva)
            if by_address:
                names_at.setdefault(rva, set()).add(name)
            return True

        callback = CALLBACK(collect)
        if not self.dbg.SymEnumSymbolsW(self.process, self.module, "*", callback, None):
            raise OSError(C.get_last_error(), "SymEnumSymbolsW failed")
        return found, names_at

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


    def _children(self, index: int) -> list[int]:
        count = int(self.type_info(index, TI_GET_CHILDRENCOUNT, W.DWORD) or 0)

        class FIND_CHILDREN(C.Structure):
            _fields_ = [("Count", W.ULONG), ("Start", W.ULONG), ("ChildId", W.ULONG * count)]

        children = FIND_CHILDREN(count, 0)
        if count and not self.dbg.SymGetTypeInfo(self.process, self.module, index, TI_FINDCHILDREN, C.byref(children)):
            return []
        return list(children.ChildId)

    def _name(self, index: int) -> str | None:
        pointer = C.c_void_p()
        if not self.dbg.SymGetTypeInfo(self.process, self.module, index, TI_GET_SYMNAME, C.byref(pointer)):
            return None
        return C.wstring_at(pointer.value) if pointer.value else None

    def _member(self, type_name: str, member: str) -> int:
        """Child id of `member` in `type_name`; must be the same in every definition."""
        found = []
        for index in self.type_indices(type_name):
            for child in self._children(index):
                if self.type_info(child, TI_GET_SYMTAG, W.DWORD) == SYM_TAG_DATA and self._name(child) == member:
                    found.append(child)
        if not found:
            raise ValueError(f"member not found: {type_name}::{member}")
        return found[0]

    def enum_value(self, enum_name: str, enumerator: str) -> int:
        values = set()
        for index in self.type_indices(enum_name):
            for child in self._children(index):
                if self._name(child) != enumerator:
                    continue

                class VARIANT(C.Structure):
                    _fields_ = [("vt", W.USHORT), ("reserved", W.USHORT * 3), ("value", C.c_int64), ("pad", C.c_int64)]

                variant = VARIANT()
                if self.dbg.SymGetTypeInfo(self.process, self.module, child, TI_GET_VALUE, C.byref(variant)):
                    # VT_I1/UI1/I2/UI2/I4/UI4/INT/UINT: only the low bytes are meaningful.
                    widths = {16: 1, 17: 1, 2: 2, 18: 2, 3: 4, 19: 4, 22: 4, 23: 4}
                    width = widths.get(variant.vt, 8)
                    raw = variant.value & ((1 << (8 * width)) - 1)
                    signed = variant.vt in (16, 2, 3, 22, 20)
                    if signed and raw >> (8 * width - 1):
                        raw -= 1 << (8 * width)
                    values.add(raw)
        if len(values) != 1:
            raise ValueError(f"enumerator must resolve uniquely: {enum_name}::{enumerator} -> {sorted(values)}")
        return values.pop()

    def base_offset(self, type_name: str, base_name: str) -> int:
        offsets = set()
        for index in self.type_indices(type_name):
            for child in self._children(index):
                if self.type_info(child, TI_GET_SYMTAG, W.DWORD) != SYM_TAG_BASE_CLASS:
                    continue
                base_type = self.type_info(child, TI_GET_TYPEID, W.DWORD)
                if base_type is not None and self._name(base_type) == base_name:
                    offset = self.type_info(child, TI_GET_OFFSET, W.DWORD)
                    if offset is not None:
                        offsets.add(int(offset))
        if len(offsets) != 1:
            raise ValueError(f"base class must resolve uniquely: {type_name} -> {base_name}: {sorted(offsets)}")
        return offsets.pop()

    def bitfield(self, type_name: str, member: str) -> dict[str, int]:
        child = self._member(type_name, member)
        position = self.type_info(child, TI_GET_BITPOSITION, W.DWORD)
        width = self.type_info(child, TI_GET_LENGTH, C.c_uint64)
        if position is None or not width:
            raise ValueError(f"not a bit field: {type_name}::{member}")
        return {"bit": int(position), "width": int(width)}


def pdb_key_and_name(pe: pefile.PE) -> tuple[str, str]:
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"]])
    for debug in pe.DIRECTORY_ENTRY_DEBUG:
        # Read the raw RSDS record (same as PdbKeyFromModule in native/src/core/offsets.cpp) instead of
        # pefile's Signature_String, whose meaning (with or without the Age) changed between
        # pefile releases.
        raw = pe.get_data(debug.struct.AddressOfRawData, debug.struct.SizeOfData)
        if raw[:4] != b"RSDS":
            continue
        data1, data2, data3 = struct.unpack_from("<IHH", raw, 4)
        (age,) = struct.unpack_from("<I", raw, 20)
        pdb_name = raw[24:].split(b"\0", 1)[0].decode(errors="replace")
        key = f"{data1:08X}{data2:04X}{data3:04X}{raw[12:20].hex().upper()}{age:X}"
        return key, Path(pdb_name).name
    raise ValueError("chrome.dll has no CodeView debug record")


def chrome_version(pe: pefile.PE) -> str:
    if not getattr(pe, "VS_FIXEDFILEINFO", None):
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
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

    progress = PROGRESS
    spec = yaml.safe_load(args.spec.read_text(encoding="utf-8"))
    with progress.section("chrome.dll"):
        # Headers and sections only: the debug and resource directories are parsed on demand.
        # A full parse (imports, relocations, ...) of chrome.dll takes about 20 seconds.
        pe = pefile.PE(str(args.dll), fast_load=True)
        key, pdb_name = pdb_key_and_name(pe)
        version = chrome_version(pe)
        progress.log(f"  Chrome {version}, PDB {pdb_name} key {key}")

    with progress.section("loading PDB"):
        size = args.pdb.stat().st_size
        # dbghelp reads lazily: this is quick, the first lookups below pay for the size.
        progress.log(f"  {args.pdb} ({size / 2**30:.1f} GiB)")
        with progress.waiting("loading the PDB"):
            symbols = DbgHelp(args.pdb)

    functions: dict[str, dict[str, Any]] = {}
    resolved: dict[str, int] = {}
    hooks: dict[str, bool] = {}
    aliases: dict[int, list[str]] = {}
    # Two passes serve every lookup: public symbols by mangled name for `symbol:` entries
    # (what a masked lookup used to search), then all symbols by undecorated name for
    # `private:` entries and for the folding check below, which needs every name at an
    # address. Each name must still resolve to exactly one address.
    # `oneOf` groups describe code Chrome has restructured: each alternative is a small spec
    # (functions plus whatever fields/types/... only that shape needs). Exactly one alternative's
    # functions must all resolve uniquely. Its sections are then merged into the spec and receive
    # the same strict validation as the base sections, so the output never mixes two shapes.
    groups: dict[str, list[dict[str, Any]]] = spec.get("oneOf", {})
    all_requests = list(spec["functions"].values()) + [
        request for alternatives in groups.values() for alternative in alternatives
        for request in alternative["functions"].values()]
    public_queries = {r["symbol"] for r in all_requests if "private" not in r}
    private_queries = {r["private"] for r in all_requests if "private" in r}
    with progress.section("functions", len(spec["functions"]) + len(groups)):
        progress.item = "pass 1: public symbols"
        with progress.waiting("enumerating public symbols (mangled names)"):
            public_found, _ = symbols.scan(undecorated=False, publics_only=True, wanted=public_queries,
                                           by_address=False)
        progress.log("  pass 1 (public symbols, mangled names) done")
        progress.item = "pass 2: all symbols"
        with progress.waiting("enumerating all symbols (undecorated names; reads the whole symbol stream)"):
            private_found, names_at = symbols.scan(undecorated=True, publics_only=False, wanted=private_queries,
                                                   by_address=True)
        progress.log(f"  pass 2 (all symbols, undecorated names) done: {len(names_at)} addresses")

        def addresses_of(request: dict[str, Any]) -> list[int]:
            if "private" in request:
                return sorted(private_found[request["private"]])
            return sorted(public_found[request["symbol"]])

        for group_name, alternatives in groups.items():
            progress.item = f"oneOf {group_name}"
            complete = [index for index, alternative in enumerate(alternatives)
                        if all(len(addresses_of(r)) == 1 for r in alternative["functions"].values())]
            if len(complete) != 1:
                shapes = " | ".join(", ".join(a["functions"]) for a in alternatives)
                found = "none" if not complete else f"alternatives {complete}"
                raise ValueError(f"oneOf {group_name}: exactly one alternative must resolve, got {found}: {shapes}")
            chosen = alternatives[complete[0]]
            for section, entries in chosen.items():
                target = spec.setdefault(section, {})
                if clash := set(target) & set(entries):
                    raise ValueError(f"oneOf {group_name}: {section} already has {sorted(clash)}")
                target.update(entries)
            progress.entry(f"oneOf {group_name}", f"alternative {complete[0]}: {', '.join(chosen['functions'])}")

        for output_name, request in spec["functions"].items():
            progress.item = output_name
            is_private = "private" in request
            query = request.get("private") or request["symbol"]
            addresses = addresses_of(request)
            if len(addresses) != 1:
                details = ", ".join(f"0x{x:x}" for x in addresses[:10]) or "none"
                raise ValueError(f"symbol must resolve to one address: {query}: {details}")
            rva = addresses[0]
            resolved[output_name] = rva
            hooks[output_name] = bool(request.get("hook", True))
            functions[output_name] = {
                "rva": f"0x{rva:08x}",
                "prologue": read_rva(pe, rva, 16).hex(" "),
            }
            aliases.setdefault(rva, []).append(output_name)
            kind = ("hook" if hooks[output_name] else "call") + (", private" if is_private else "")
            progress.entry(output_name, f"0x{rva:08x}  {kind}")

    # Hooking a function that shares its address with other code would intercept that
    # code too, so every *hooked* function must be a unique entry point. Functions we
    # only call (vtable lookups, throw-away helpers) may legally fold onto other code.
    collisions: dict[str, str] = {}
    hook_targets: dict[str, dict[str, str]] = {}
    with progress.section("hook targets (identical code folding)", sum(hooks.values())):
        for output_name, rva in resolved.items():
            if not hooks.get(output_name, True):
                continue
            progress.item = output_name
            others = sorted(names_at.get(rva, ())) if len(aliases[rva]) == 1 else []
            if len(aliases[rva]) > 1:
                collisions[output_name] = "requested more than once"
                progress.log(f"  {output_name:<58} SHARED: requested more than once")
                continue
            others = [name for name in others if name != output_name]
            if others:
                collisions[output_name] = ", ".join(others[:4])
                progress.log(f"  {output_name:<58} SHARED with {collisions[output_name]}")
                continue
            hook_targets[output_name] = {"rva": functions[output_name]["rva"]}
            progress.entry(output_name, "unique entry point")
        if collisions:
            details = "; ".join(f"{name} <- {owner}" for name, owner in collisions.items())
            raise ValueError(f"hooked functions share their address with other code: {details}")

    type_cache: dict[str, tuple[int, dict[str, int]]] = {}

    def layout_of(type_name: str, announce: bool = False) -> tuple[int, dict[str, int]]:
        if type_name not in type_cache:
            with progress.waiting(f"reading type {type_name}"):
                type_cache[type_name] = symbols.type_layout(type_name)
            if announce:  # types only used through their fields are not pinned, but still reported
                size, members = type_cache[type_name]
                progress.log(f"  (type {type_name}: size 0x{size:x}, {len(members)} members)")
        return type_cache[type_name]

    with progress.section("type sizes", len(spec["types"])):
        for type_name, expected_size in spec["types"].items():
            progress.item = type_name
            layout = layout_of(type_name)
            expected = int(expected_size, 0) if isinstance(expected_size, str) else int(expected_size)
            if layout[0] != expected:
                raise ValueError(f"{type_name} size 0x{layout[0]:x}, expected 0x{expected:x}")
            progress.entry(type_name, f"0x{layout[0]:x}")

    fields: dict[str, str] = {}
    with progress.section("fields", len(spec["fields"])):
        for output_name, (type_name, field_name) in spec["fields"].items():
            progress.item = output_name
            layout = layout_of(type_name, announce=True)
            if field_name not in layout[1]:
                raise ValueError(f"field not found: {type_name}.{field_name}")
            fields[output_name] = f"0x{layout[1][field_name]:x}"
            progress.entry(output_name, fields[output_name])

    vtables: dict[str, list[str]] = {}
    vtable_rvas: dict[str, list[int]] = {}
    with progress.section("vtables", len(spec["vtables"])):
        for output_name, undecorated_name in spec["vtables"].items():
            progress.item = output_name
            owner = undecorated_name.split("::`vftable'")[0].split("::")[-1]
            with progress.waiting(f"looking up {undecorated_name}"):
                matches = symbols.symbols(f"*{owner}*vftable*", undecorated=True)
            rvas = sorted({x["rva"] for x in matches if x["name"] == undecorated_name})
            if not rvas:
                raise ValueError(f"vtable not found: {undecorated_name}")
            vtable_rvas[output_name] = rvas
            vtables[output_name] = [f"0x{x:08x}" for x in rvas]
            progress.entry(output_name, f"{len(rvas)} table(s): {' '.join(vtables[output_name])}")

    slots: dict[str, int] = {}
    image_base = int(pe.OPTIONAL_HEADER.ImageBase)
    with progress.section("vtable slots", len(spec["vtableSlots"])):
        for output_name, request in spec["vtableSlots"].items():
            progress.item = output_name
            target = resolved[request["function"]]
            found_slots = set()
            for table_rva in vtable_rvas[request["vtable"]]:
                values = struct.unpack("<64Q", read_rva(pe, table_rva, 64 * 8))
                found_slots.update(index for index, value in enumerate(values) if value - image_base == target)
            if len(found_slots) != 1:
                raise ValueError(f"vtable slot must be unique: {output_name}: {sorted(found_slots)}")
            slots[output_name] = found_slots.pop()
            progress.entry(output_name, f"slot {slots[output_name]}")

    constants: dict[str, int] = {}
    with progress.section("constants", len(spec.get("constants", {}))):
        for output_name, (enum_name, enumerator) in spec.get("constants", {}).items():
            progress.item = output_name
            constants[output_name] = symbols.enum_value(enum_name, enumerator)
            progress.entry(output_name, str(constants[output_name]))

    bitfields: dict[str, dict[str, int]] = {}
    with progress.section("bit fields", len(spec.get("bitfields", {}))):
        for output_name, (type_name, member) in spec.get("bitfields", {}).items():
            progress.item = output_name
            bitfields[output_name] = symbols.bitfield(type_name, member)
            progress.entry(output_name, f"bit {bitfields[output_name]['bit']}, width {bitfields[output_name]['width']}")

    bases: dict[str, str] = {}
    with progress.section("base classes", len(spec.get("bases", {}))):
        for output_name, (type_name, base_name) in spec.get("bases", {}).items():
            progress.item = output_name
            bases[output_name] = f"0x{symbols.base_offset(type_name, base_name):x}"
            progress.entry(output_name, bases[output_name])
    progress.stage, progress.item = "writing output", ""

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
        "constants": constants,
        "bitfields": bitfields,
        "bases": bases,
        "sizes": {name: f"0x{layout[0]:x}" for name, layout in type_cache.items()},
        "checks": spec["checks"],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    progress.log(f"done in {time.monotonic() - progress.start:.1f}s")
    print(f"resolved Chrome {version} ({key}) -> {args.out}")
    print(f"validated {len(functions)} functions, {len(fields)} fields, {len(type_cache)} types, {len(slots)} vtable slots, "
          f"{len(constants)} constants, {len(bitfields)} bit fields")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        where = PROGRESS.stage + (f" / {PROGRESS.item}" if PROGRESS.item else "")
        message = f"resolver failed in {where} after {PROGRESS.passed} validated entries: {error}"
        if PROGRESS.grouped:  # the failing section has already closed its log group
            print(f"::error title=resolver::{message}", flush=True)
        print(message, file=sys.stderr, flush=True)
        raise
