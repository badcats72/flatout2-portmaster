#!/usr/bin/env python3
"""reachable_filter.py <exe> <module copy> <catalog.json> <out catalog.json>: keep the module code the exe can reach.

d3dx9_30.dll keeps data inside .text, and the catalog decoded a lot of it as code: the full lift gave about
5 GB of C, four files Clang refused ("ran out of source locations") and branches to labels that do not exist.
Only what the game can run is kept here. Roots: DllMain, the exports the exe imports from the module and
the code pointers stored outside the code section (vtables of the D3DX COM objects, callback tables).
Edges: direct calls, branches and block successors, and every relocated pointer inside the instruction
bytes of a kept function (callbacks pushed as arguments, vtables stored in constructors; a vtable in data
brings in all its entries). A function missing at run time stops with "ICALL unresolved <address>".
"""
import bisect, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PCRECOMP = os.environ.get("PCRECOMP", os.path.normpath(os.path.join(HERE, "..", "..", "pcrecomp")))
sys.path.insert(0, os.path.join(PCRECOMP, "tools", "pe"))
import pefile                                      # noqa: E402

exe_path, mod_path, cat_path, out_path = sys.argv[1:5]
mod = pefile.PE(mod_path)
base = mod.OPTIONAL_HEADER.ImageBase
img = mod.get_memory_mapped_image()
name = mod_path.replace("\\", "/").rsplit("/", 1)[-1].lower()
code = [(base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize)
        for s in mod.sections if s.Characteristics & 0x20000000]
in_code = lambda va: any(lo <= va < hi for lo, hi in code)
dword = lambda va: int.from_bytes(img[va - base:va - base + 4], "little")

cat = json.load(open(cat_path))
funcs = sorted(cat["functions"], key=lambda f: f["address"])
starts = [f["address"] for f in funcs]
by_addr = {f["address"]: f for f in funcs}

# functions holding an address (by their blocks), through a 4 KB page index
pages = {}
for f in funcs:
    for s, e, _succ, _x in f["blocks"]:
        for pg in range(s >> 12, ((e - 1) >> 12) + 1):
            pages.setdefault(pg, []).append((s, e, f["address"]))
holder_cache = {}
def holder(va):
    if va not in holder_cache:
        holder_cache[va] = {a for s, e, a in pages.get(va >> 12, ()) if s <= va < e}
    return holder_cache[va]

# A pointer is followed as code only where a function plausibly starts (prepare_module.py's rule): the
# string tables of the HLSL compiler (pointers to "texCUBE", "tex3Dproj"... kept inside .text) otherwise
# bring in thousands of overlapping pseudo-functions decoded from text.
PROLOGUES = (b"\x55\x8b\xec", b"\x8b\xff\x55", b"\x6a\xff\x68", b"\x83\xec", b"\x81\xec", b"\x56", b"\x57", b"\x53",
             b"\x8b\x44\x24", b"\x8b\x4c\x24", b"\x8b\x54\x24", b"\x6a", b"\x68", b"\x33\xc0", b"\xb8")
def looks_like_function(va):
    rva = va - base
    head, before = img[rva:rva + 3], img[rva - 1]
    if before in (0xCC, 0x90, 0xC3):
        return any(head.startswith(p) for p in PROLOGUES) or before != 0xC3
    if img[rva - 3] == 0xC2:                       # ret n
        return any(head.startswith(p) for p in PROLOGUES)
    return head.startswith((b"\x55\x8b\xec", b"\x8b\xff\x55"))

relocs = sorted(base + e.rva for b in getattr(mod, "DIRECTORY_ENTRY_BASERELOC", []) for e in b.entries if e.type == 3)
data_relocs = [r for r in relocs if not in_code(r)]
data_reloc_set = set(data_relocs)

# roots: entry point, exports imported by the exe, code pointers held in data
exe = pefile.PE(exe_path)
wanted_names = {imp.name.decode() if imp.name else imp.ordinal
                for d in exe.DIRECTORY_ENTRY_IMPORT if d.dll.decode().lower() == name for imp in d.imports}
roots = {base + mod.OPTIONAL_HEADER.AddressOfEntryPoint}
for sym in mod.DIRECTORY_ENTRY_EXPORT.symbols:
    if (sym.name and sym.name.decode() in wanted_names) or sym.ordinal in wanted_names:
        roots.add(base + sym.address)
# every relocated pointer, .text included: Microsoft merges .rdata (the CRT's __xi/__xc init tables,
# vtables) into .text, and DllMain's _initterm calls through them (0xC156F9 was missed otherwise)
for r in relocs:
    v = dword(r)
    if v in by_addr and looks_like_function(v):
        roots.add(v)
print(f"[*] {len(wanted_names)} imports from {name}, {len(roots)} roots")

def pointer_targets(va):
    """A relocated value: a function, code inside functions, or a data table whose code pointers follow."""
    out = set()
    if in_code(va):
        if va in by_addr and looks_like_function(va):
            out.add(va)
        return out
    # data: follow the run of relocated dwords starting there (a vtable / function table)
    p = va
    while p in data_reloc_set:
        t = dword(p)
        if t in by_addr and looks_like_function(t):
            out.add(t)
        p += 4
    return out

# switch cases: jumptable_seeds.json (next to the catalog) names the `jmp [table+r*4]` of each table;
# its cases are catalogued as functions of their own and reached only through it (0xB2891B)
cases = {}
jt_path = os.path.join(os.path.dirname(os.path.abspath(cat_path)), "jumptable_seeds.json")
if os.path.exists(jt_path):
    for s in json.load(open(jt_path)):
        site = int(s["source"].rsplit(" via ", 1)[1], 16)
        cases.setdefault(site, set()).add(s["address"])
case_sites = sorted(cases)

kept = set()
todo = [r for r in roots if r in by_addr]
while todo:
    a = todo.pop()
    if a in kept:
        continue
    kept.add(a)
    f = by_addr[a]
    own = {b[0] for b in f["blocks"]}
    nxt = set(f["calls_to"]) | set(f["jump_targets"])
    for s, e, succ, _x in f["blocks"]:
        nxt.update(succ)
        i = bisect.bisect_left(relocs, s)
        while i < len(relocs) and relocs[i] < e:
            nxt |= pointer_targets(dword(relocs[i]))
            i += 1
        i = bisect.bisect_left(case_sites, s)
        while i < len(case_sites) and case_sites[i] < e:
            nxt |= cases[case_sites[i]]
            i += 1
    nxt -= own                                     # branches inside the function itself
    nxt -= kept
    for t in nxt:
        if t in by_addr:
            todo.append(t)
        else:
            todo.extend(holder(t))

cat["functions"] = [f for f in funcs if f["address"] in kept]
json.dump(cat, open(out_path, "w"))
size = sum(f["size"] for f in cat["functions"])
print(f"[*] kept {len(kept)} of {len(funcs)} functions ({size:,} bytes of code) -> {out_path}")
