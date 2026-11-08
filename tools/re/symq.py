"""Query a PDB via dbghelp. usage: symq.py <pdb> sym <mask>... | type <TypeName>..."""
import ctypes as C, ctypes.wintypes as W, sys

dbg = C.WinDLL(r"C:\Windows\System32\dbghelp.dll")
BASE = 0x180000000
hp = W.HANDLE(0x1234)

class SYMBOL_INFOW(C.Structure):
    _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", C.c_uint64 * 2),
                ("Index", W.ULONG), ("Size", W.ULONG), ("ModBase", C.c_uint64), ("Flags", W.ULONG),
                ("Value", C.c_uint64), ("Address", C.c_uint64), ("Register", W.ULONG), ("Scope", W.ULONG),
                ("Tag", W.ULONG), ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG), ("Name", W.WCHAR * 1)]

import os
dbg.SymSetOptions((0 if os.environ.get("RAW") else 0x2) | 0x200)  # UNDNAME | DEFERRED_LOADS(off below) | FAIL_CRITICAL
dbg.SymInitializeW(hp, None, False)
dbg.SymLoadModuleExW.restype = C.c_uint64
dbg.SymLoadModuleExW.argtypes = [W.HANDLE, W.HANDLE, W.LPCWSTR, W.LPCWSTR, C.c_uint64, W.DWORD, C.c_void_p, W.DWORD]
mb = dbg.SymLoadModuleExW(hp, None, sys.argv[1], None, BASE, 0x20000000, None, 0)
assert mb, C.GetLastError()

CB = C.WINFUNCTYPE(W.BOOL, C.POINTER(SYMBOL_INFOW), W.ULONG, C.c_void_p)
TAGS = {5: "Func", 7: "Data", 10: "Public", 12: "UDT"}

def name_of(p):
    return C.wstring_at(C.addressof(p.contents) + SYMBOL_INFOW.Name.offset, p.contents.NameLen)

def sym(mask):
    out = []
    def cb(p, size, _):
        s = p.contents
        out.append((s.Address - BASE, s.Size, TAGS.get(s.Tag, s.Tag), name_of(p)))
        return True
    dbg.SymEnumSymbolsW(hp, C.c_uint64(mb), mask, CB(cb), None)
    for rva, size, tag, n in sorted(set(out), key=lambda x: x[3]):
        print(f"{rva:#010x} {size:6} {tag:6} {n}")
    if not out:
        print("  (none)", mask)

TI_GET_SYMNAME, TI_GET_OFFSET, TI_GET_CHILDRENCOUNT, TI_FINDCHILDREN, TI_GET_LENGTH, TI_GET_SYMTAG = 1, 10, 13, 7, 2, 0

def ti(idx, kind, ctype):
    v = ctype()
    ok = dbg.SymGetTypeInfo(hp, C.c_uint64(mb), idx, kind, C.byref(v))
    return v.value if ok else None

def typ(name):
    cands = []
    def cb(p, size, _):
        cands.append(p.contents.TypeIndex); return True
    dbg.SymEnumTypesByNameW(hp, C.c_uint64(mb), name, CB(cb), None)
    cands = [c for c in dict.fromkeys(cands) if ti(c, TI_GET_LENGTH, C.c_uint64)]
    if not cands:
        print("  (no type)", name); return
    for idx in cands[:1]:
        print(f"type {name} size={ti(idx, TI_GET_LENGTH, C.c_uint64):#x} (candidates={len(cands)})")
        n = ti(idx, TI_GET_CHILDRENCOUNT, W.DWORD) or 0
        class FC(C.Structure):
            _fields_ = [("Count", W.ULONG), ("Start", W.ULONG), ("ChildId", W.ULONG * n)]
        fc = FC(n, 0)
        dbg.SymGetTypeInfo(hp, C.c_uint64(mb), idx, TI_FINDCHILDREN, C.byref(fc))
        for c in fc.ChildId:
            if ti(c, TI_GET_SYMTAG, W.DWORD) not in (7, 18):  # Data, BaseClass
                continue
            p = C.c_void_p()
            dbg.SymGetTypeInfo(hp, C.c_uint64(mb), c, TI_GET_SYMNAME, C.byref(p))
            nm = C.wstring_at(p.value) if p.value else "<base>"
            off = ti(c, TI_GET_OFFSET, W.DWORD)
            if off is not None:
                print(f"  +{off:#06x} {nm}")

mode = sys.argv[2]
for a in sys.argv[3:]:
    (sym if mode == "sym" else typ)(a)
