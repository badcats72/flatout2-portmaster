#!/usr/bin/env python3
"""Entry points for the catalog from the exe's switch jump tables.

  jump_table_seeds.py <exe> <catalog.json> <out.json>

Finds every "jmp dword ptr [reg*4 + table]" among the instructions the catalog decoded (its basic
blocks: a linear sweep loses its footing on the tables MSVC's CRT keeps inside the code, e.g.
memcpy's at 0x56a8dc), reads each table (consecutive dwords that point into the code section) and
writes each target as a seed, in the format of pcrecomp's vtable_scan/rtti seeds
([{"address": va, "source": ...}]). Run it again on the new catalog until it finds nothing new.

pcrecomp's discovery misses some tables: their targets were only labels inside one function, so an
indirect jump through the table from another lifted function had nowhere to go ("ITAIL unresolved").
"""
import json, struct, sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM


def main(exe, catalog, out):
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    lo, hi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    tables, seeds = set(), {}
    blocks = {(b[0], b[1]) for f in json.load(open(catalog))["functions"] for b in f["blocks"]}
    for start, end in sorted(blocks):
        for ins in md.disasm(img[start - base:end - base], start):
            if ins.mnemonic != "jmp" or len(ins.operands) != 1 or ins.operands[0].type != X86_OP_MEM:
                continue
            m = ins.operands[0].mem
            if m.scale != 4 or m.index == 0 or m.base != 0:
                continue
            table = m.disp & 0xffffffff
            if table in tables or not (base <= table < base + len(img)):
                continue
            tables.add(table)
            # Forward from the address, and backward too: MSVC's memcpy indexes some tables from their
            # end with a negative count (jmp [ecx*4 + 0x56a8e0] reaches 0x56a8c4..0x56a8dc).
            for step in (4, -4):
                off = table - base if step > 0 else table - base - 4
                while 0 <= off and off + 4 <= len(img):
                    target = struct.unpack_from("<I", img, off)[0]
                    if not (lo <= target < hi):
                        break
                    seeds.setdefault(target, f"jump table {table:08x} via {ins.address:08x}")
                    off += step
    doc = [{"address": a, "source": why} for a, why in sorted(seeds.items())]
    json.dump(doc, open(out, "w"))
    print(f"[*] {len(tables)} jump tables, {len(doc)} targets -> {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
