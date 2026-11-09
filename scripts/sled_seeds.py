#!/usr/bin/env python3
"""sled_seeds.py <exe> <catalog.json>: computed jumps into instruction sleds, added to an existing catalog.

AMD's memcpy (linked into FlatOut2.exe at 0x560F3C) copies the tail with
    mov ecx, ebx; shr ecx, 2; and ecx, 0xf; neg ecx; add ecx, 0x5610B4; jmp ecx
i.e. it jumps n bytes before 0x5610B4, into a run of one-byte `movsd`. The catalog only knows the start of
the run, so the `jmp ecx` found no function ("ITAIL unresolved 0x005610b1"). Every `neg r; add r, imm; jmp r`
in catalogued code makes each entry of the run of identical one-byte instructions before imm a function,
disassembled and appended to the catalog in place (no new catalog pass needed).
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PCRECOMP = os.environ.get("PCRECOMP", os.path.normpath(os.path.join(HERE, "..", "..", "pcrecomp")))
for sub in ("pe", "disasm"):
    sys.path.insert(0, os.path.join(PCRECOMP, "tools", sub))
from capstone import Cs, CS_ARCH_X86, CS_MODE_32   # noqa: E402
from pe_analyze import analyze_pe, build_iat_map    # noqa: E402
from disasm32 import Disassembler                   # noqa: E402

exe, catalog_path = sys.argv[1], sys.argv[2]
info = analyze_pe(exe)
data = open(exe, "rb").read()
import pefile
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
cat = json.load(open(catalog_path))
known = {f["address"] for f in cat["functions"]}
md = Cs(CS_ARCH_X86, CS_MODE_32)

targets = set()
for f in cat["functions"]:
    for s, e, _succ, _ex in f["blocks"]:
        ins = list(md.disasm(img[s - base:e - base], s))
        for a, b, c in zip(ins, ins[1:], ins[2:]):
            if (a.mnemonic == "neg" and b.mnemonic == "add" and c.mnemonic == "jmp"
                    and b.op_str.startswith(a.op_str + ", ") and c.op_str == a.op_str):
                try:
                    end = int(b.op_str.split(", ")[1], 0)
                except ValueError:
                    continue
                if not (info.code_start <= end < info.code_end):
                    continue
                op = img[end - 1 - base]
                va = end
                while img[va - 1 - base] == op and end - va < 64:
                    va -= 1
                targets.update(range(va, end + 1))
new = sorted(targets - known)
dis = Disassembler(data, info.image_base, info.sections)
iat = build_iat_map(info)
added = 0
for va in new:
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
print(f"[*] {len(targets)} sled entries, {added} added to {catalog_path}")
