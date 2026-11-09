#!/usr/bin/env python3
"""data_pointer_rescue.py <exe> <catalog.json>: code pointers kept in data, decoded as functions on their own.

The catalog drops a seed that lands inside another decoded run ("not an instruction boundary"). In
FlatOut2.exe a switch table at 0x5AAB84 was decoded as code straight across 0x5AABA0, a function whose
only reference is a function-pointer table in .rdata: its indirect call stopped the game ("ICALL
unresolved 0x005aaba0"). Every base-relocated pointer stored OUTSIDE the code section (vtables, callback
and handler tables) that targets code and is not yet a function start is disassembled on its own and
appended to the catalog. Pointers stored inside .text are taken too (immediates: `push offset fn`,
`mov [x], offset fn`; 0x6439E0 is registered that way and follows a jump table with no padding, so the
function before it was decoded across it: "ICALL unresolved 0x006439e0"), except the entries of the jump
tables found by the jump-table pass (jumptable_seeds.json next to the catalog), which are case labels.
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PCRECOMP = os.environ.get("PCRECOMP", os.path.normpath(os.path.join(HERE, "..", "..", "pcrecomp")))
for sub in ("pe", "disasm"):
    sys.path.insert(0, os.path.join(PCRECOMP, "tools", sub))
import pefile                                      # noqa: E402
from pe_analyze import analyze_pe, build_iat_map   # noqa: E402
from disasm32 import Disassembler                  # noqa: E402

exe, catalog_path = sys.argv[1], sys.argv[2]
info = analyze_pe(exe)
data = open(exe, "rb").read()
pe = pefile.PE(exe)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
code = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections if s.Characteristics & 0x20000000]
in_code_rva = lambda rva: any(lo <= rva < hi for lo, hi in code)
cat = json.load(open(catalog_path))
known = {f["address"] for f in cat["functions"]}
# jump tables: "jump table <table> via <jmp>"; a table runs while its dwords are relocated code pointers
tables = set()
jt_path = os.path.join(os.path.dirname(os.path.abspath(catalog_path)), "jumptable_seeds.json")
if os.path.exists(jt_path):
    tables = {int(s["source"].split()[2], 16) - base for s in json.load(open(jt_path))}
reloc_rvas = {e.rva for b in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []) for e in b.entries if e.type == 3}
in_table = set()
for t in tables:
    p = t
    while p in reloc_rvas and in_code_rva(int.from_bytes(img[p:p + 4], "little") - base):
        in_table.add(p)
        p += 4
def starts_code(rva):
    """For pointers held in .text: most of them point at data kept in .text (jump tables, the byte index
    tables of switches). A target counts only if it is not itself a table (relocated dword, or a 00 byte)
    and follows padding, a ret, or the end of a table (0x6439E0)."""
    if rva in reloc_rvas or img[rva] == 0:
        return False
    return img[rva - 1] in (0xCC, 0x90, 0xC3) or img[rva - 3] == 0xC2 or (rva - 4) in reloc_rvas
wanted = set()
for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
    for entry in block.entries:
        if entry.type != 3 or entry.rva in in_table:
            continue
        value = int.from_bytes(img[entry.rva:entry.rva + 4], "little")
        if in_code_rva(value - base) and value not in known and (not in_code_rva(entry.rva) or starts_code(value - base)):
            wanted.add(value)
dis = Disassembler(data, info.image_base, info.sections)
iat = build_iat_map(info)
added = 0
for va in sorted(wanted):
    fn = dis.disassemble_function(va, iat)
    if fn is None or not fn.blocks:
        continue
    cat["functions"].append({
        "address": fn.address, "end": fn.end, "size": fn.size, "entry_kind": fn.entry_kind, "is_thunk": fn.is_thunk,
        "jump_targets": sorted(fn.jump_targets), "calls_to": sorted(fn.calls_to),
        "blocks": [[b.start, b.end, sorted(set(b.successors)), int(b.is_exit)]
                   for b in sorted(fn.blocks.values(), key=lambda b: b.start)]})
    added += 1
cat["functions"].sort(key=lambda f: f["address"])
json.dump(cat, open(catalog_path, "w"))
print(f"[*] {len(wanted)} data-held code pointers not yet functions, {added} added to {catalog_path}")
