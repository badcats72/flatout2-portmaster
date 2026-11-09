#!/usr/bin/env python3
"""Make an ANALYSIS copy of a game DLL relocated to its fixed guest base, plus seeds.

  prepare_module.py <game.dll> <base> <analysis.dll> <seeds.json>

Lifted code bakes absolute addresses, so a DLL is lifted as if loaded at the
base the runtime maps it at (server.dll: 0x00A00000, between SPEED2.EXE's image
and the first heap arena on both ABIs). The copy has every base relocation
applied for that base; the runtime applies the same relocations to the
original file at load time. Seeds: the entry point, every export and every
relocated pointer that lands in code (vtables, callbacks, thread starts and
jump-table targets). Both outputs are derived from the game binary: write
them under ../work (never committed). The original file is never written.
"""
import json, sys, pefile


def main(src, base, dst, seeds_path):
    pe = pefile.PE(src)
    old = pe.OPTIONAL_HEADER.ImageBase
    pe.relocate_image(base)
    pe.OPTIONAL_HEADER.ImageBase = base
    pe.write(dst)

    pe = pefile.PE(dst)
    code = [s for s in pe.sections if s.Characteristics & 0x20000000]
    def in_code(va):
        return any(base + s.VirtualAddress <= va < base + s.VirtualAddress + s.Misc_VirtualSize for s in code)
    seeds = [{"address": base + pe.OPTIONAL_HEADER.AddressOfEntryPoint, "note": "entry"}]
    for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        seeds.append({"address": base + e.address, "note": "export " + (e.name or b"#%d" % e.ordinal).decode()})
    image = pe.get_memory_mapped_image()
    # Microsoft's DLLs merge read-only data into .text, so many relocated pointers there are strings,
    # tables and vtables. A pointer is a seed only where a function plausibly starts: a standard prologue,
    # or right after padding (int3/nop) or a ret. (d3dx9_30.dll: all 7,578 pointers sent the catalog to 11 GB.)
    prologues = (b"\x55\x8b\xec", b"\x8b\xff\x55", b"\x6a\xff\x68", b"\x83\xec", b"\x81\xec", b"\x56", b"\x57", b"\x53",
                 b"\x8b\x44\x24", b"\x8b\x4c\x24", b"\x8b\x54\x24", b"\x6a", b"\x68", b"\x33\xc0", b"\xb8")
    def looks_like_function(img, rva):
        head = img[rva:rva + 3]
        before = img[rva - 1] if rva else 0
        if before in (0xCC, 0x90, 0xC3):
            return any(head.startswith(p) for p in prologues) or before != 0xC3
        if rva >= 3 and img[rva - 3] == 0xC2:   # ret n
            return any(head.startswith(p) for p in prologues)
        return head.startswith((b"\x55\x8b\xec", b"\x8b\xff\x55"))
    pointers = set()
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        for entry in block.entries:
            if entry.type != 3:  # IMAGE_REL_BASED_HIGHLOW
                continue
            value = int.from_bytes(image[entry.rva:entry.rva + 4], "little")
            if in_code(value) and looks_like_function(image, value - base):
                pointers.add(value)
    seeds += [{"address": va, "note": "relocated code pointer"} for va in sorted(pointers)]
    json.dump(seeds, open(seeds_path, "w"), indent=1)
    print(f"[*] {src}: base {old:#x} -> {base:#x}, {len(seeds)} seeds ({len(pointers)} code pointers) -> {dst}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(sys.argv[1], int(sys.argv[2], 0), sys.argv[3], sys.argv[4])
