#!/usr/bin/env python3
"""reloc_seeds.py <exe> <seeds.json>: every base-relocated pointer that lands in the code section.

FlatOut2.exe keeps its .reloc section, so every absolute code address the compiler emitted (vtables,
callbacks, thread starts, jump-table entries) is listed there: catalog seeds the vtable/RTTI scans can miss.
"""
import json, sys, pefile

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
code = [(base + s.VirtualAddress, base + s.VirtualAddress + s.Misc_VirtualSize)
        for s in pe.sections if s.Characteristics & 0x20000000]
image = pe.get_memory_mapped_image()
seeds = set()
for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
    for entry in block.entries:
        if entry.type != 3:
            continue
        value = int.from_bytes(image[entry.rva:entry.rva + 4], "little")
        if any(lo <= value < hi for lo, hi in code):
            seeds.add(value)
json.dump([{"address": va, "note": "relocated code pointer"} for va in sorted(seeds)], open(sys.argv[2], "w"))
print(f"[*] {len(seeds)} relocated code pointers -> {sys.argv[2]}")
