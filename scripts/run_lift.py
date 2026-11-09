#!/usr/bin/env python3
"""Alien Shooter 2: Reloaded lift driver (pcrecomp lift32, global-register model).

Taken from the NFSU2 port (github.com/manuelpllull/nfsu2-recompiled-r36s, MIT):
the same catalog and lift steps, without SPEED2.EXE's variant table. Hooks,
overrides and exclusions come from config/{hooks,overrides,exclude}.txt here:
one "VA  comment" per line (# starts a comment).

NFSU2's own notes follow.


  run_lift.py catalog  <analysis.exe> <seeds.json> <catalog.json>
      Recursive descent (pcrecomp disasm32) to a catalog that keeps every
      function's BASIC BLOCK ranges, so lifting can re-decode without paying
      for the whole discovery again (it takes ~12 minutes on SPEED2.EXE).

  run_lift.py lift  <analysis.exe> <catalog.json> <outdir> [--split N] [--names names.json] [--prefix P]
      Lift every catalogued function with lift32.Lifter and write:
        recomp_NNNN.c          the functions, N per file
        recomp_funcs.h         declarations
        recomp_dispatch.c      sorted VA -> function table (binary search)
        recomp_imports.c       IAT slot VA -> "dll!name" (for crash trails)
        lift_stats.json        counts for docs/LIFT_REPORT.md
        recomp_variant.c       which SPEED2.EXE this is (config/exe_variants.txt)
        nfs_exe_variant.h      the same, for src/runtime/nfs_exe_addr.h
      The exe is recognised by its SHA-1 (or the .text SHA-1 of the analysis
      copy); config/hooks.txt, overrides.txt and exclude.txt are read in its
      column. An exe that is not listed stops the lift.
      --prefix P names a second module's output (P + every file and table
      above, e.g. server_recomp_dispatch_table for server.dll) so it links
      next to SPEED2.EXE's; no SPEED2.EXE configuration applies to it.

Everything written here is derived from the game binary and is gitignored.
"""
import json, os, sys, time, collections

HERE = os.path.dirname(os.path.abspath(__file__))
PCRECOMP = os.environ.get("PCRECOMP", os.path.normpath(os.path.join(HERE, "..", "..", "pcrecomp")))
for sub in ("pe", "disasm", "lift"):
    sys.path.insert(0, os.path.join(PCRECOMP, "tools", sub))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map   # noqa: E402
import disasm32                                     # noqa: E402
from disasm32 import Disassembler, Function, BasicBlock, Instruction  # noqa: E402


def cmd_catalog(exe, seeds_path, out):
    info = analyze_pe(exe)
    iat = build_iat_map(info)
    data = open(exe, "rb").read()
    seeds = {info.image_base + info.entry_point_rva}
    for e in json.load(open(seeds_path)):
        a = e.get("address") if isinstance(e, dict) else e
        seeds.add(int(a, 0) if isinstance(a, str) else a)
    dis = Disassembler(data, info.image_base, info.sections)
    t = time.time()
    funcs = dis.find_functions(info.code_start, info.code_end, iat, seeds=seeds,
                               release_operands=True)
    print(f"[*] {len(funcs)} functions in {time.time() - t:.0f}s")
    # Seeds that discovery dropped as "not an instruction boundary" because a neighbour decoded data as code
    # straight across them (0x494A50, a vtable method, sat under bytes of 0x494A2E). A seed that starts
    # with a function prologue is decoded on its own; the rest are mostly UTF-16 text read as pointers.
    prologues = (b"\x55\x8b\xec", b"\x8b\xff\x55", b"\x6a\xff\x68", b"\x83\xec", b"\x81\xec")
    rescued = 0
    for va in sorted(seeds - set(funcs)):
        if not (info.code_start <= va < info.code_end):
            continue
        head = data[va - info.image_base: va - info.image_base + 3] if va >= info.image_base else b""
        if not any(head.startswith(p) for p in prologues):
            continue
        f = dis.disassemble_function(va, iat)
        if f is not None and f.blocks:
            funcs[va] = f
            rescued += 1
    if rescued:
        print(f"[*] {rescued} dropped seeds with a function prologue decoded on their own")
    doc = {"exe": os.path.basename(exe), "image_base": info.image_base,
           "code_start": info.code_start, "code_end": info.code_end,
           "functions": []}
    for f in sorted(funcs.values(), key=lambda f: f.address):
        doc["functions"].append({
            "address": f.address, "end": f.end, "size": f.size,
            "entry_kind": f.entry_kind, "is_thunk": f.is_thunk,
            "jump_targets": sorted(f.jump_targets),
            "calls_to": sorted(f.calls_to),
            "blocks": [[b.start, b.end, sorted(set(b.successors)), int(b.is_exit)]
                       for b in sorted(f.blocks.values(), key=lambda b: b.start)],
        })
    json.dump(doc, open(out, "w"))
    print(f"[*] wrote {out}")


def rebuild(md, img, base, fd):
    """A disasm32.Function with full capstone operands, from a catalog entry."""
    f = Function(address=fd["address"], end=fd["end"], size=fd["size"],
                 name=f"sub_{fd['address']:08X}", is_thunk=fd["is_thunk"],
                 entry_kind=fd["entry_kind"])
    f.jump_targets = set(fd["jump_targets"])
    f.calls_to = set(fd["calls_to"])
    for s, e, succ, ex in fd["blocks"]:
        b = BasicBlock(start=s, end=e, successors=list(succ), is_exit=bool(ex))
        for ins in md.disasm(img[s - base:e - base], s):
            mnemonic = ins.mnemonic
            # "rep ret" (F3 C3) is the AMD branch-predictor idiom MSVC's CRT uses: a plain ret.
            if mnemonic in ("repz ret", "rep ret"):
                mnemonic = "ret"
            b.instructions.append(Instruction(ins.address, ins.size, mnemonic,
                                              ins.op_str, bytes(ins.bytes),
                                              list(ins.operands)))
        f.blocks[s] = b
    return f


def va_list(name):
    """config/<name>: "VA  comment" lines -> {va: comment} (missing file: empty)."""
    out = {}
    p = os.path.join(HERE, "..", "config", name)
    if os.path.exists(p):
        for line in open(p):
            line = line.split("#", 1)[0].strip()
            if line:
                va, _, why = line.partition(" ")
                out[int(va, 16)] = why.strip() or name
    return out


def cmd_lift(exe, catalog, outdir, split=400, names_path=None, prefix=""):
    import pefile
    from lift32 import Lifter
    info = analyze_pe(exe)
    iat = build_iat_map(info)
    module = os.path.basename(exe)
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    cat = json.load(open(catalog))
    fds = cat["functions"]
    lifted = {fd["address"] for fd in fds}
    names = {}
    if names_path and os.path.exists(names_path):
        names = {int(k, 16): v for k, v in json.load(open(names_path)).items()}
    overrides, excluded, hooks, setjmp = {}, {}, {}, {}
    if not prefix:
        overrides = va_list("overrides.txt")
        excluded = va_list("exclude.txt")
        hooks = va_list("hooks.txt")
        setjmp = {va: kind.split()[0] for va, kind in va_list("setjmp.txt").items()}
    # config/skippable.txt ("VA bytes comment"): functions with a hook that may ask to return at once,
    # by setting g_recomp_skip_function; bytes is what the function's own ret removes from the stack, with the return slot.
    skippable = {va: int(rest.split()[0]) for va, rest in va_list("skippable.txt").items()} if not prefix else {}
    # Direct calls to the CRT's setjmp/longjmp (config/setjmp.txt): a lifted longjmp can only "return" to
    # its own caller, so the call sites get a native setjmp and longjmp becomes native code.
    call_rewrites = []
    for va, kind in setjmp.items():
        call = f"RECOMP_CALL(sub_{va:08X});"
        if kind == "setjmp":
            call_rewrites.append((call, f"RECOMP_SETJMP_CALL(sub_{va:08X});"))
        elif kind == "longjmp":
            call_rewrites.append((call, "RECOMP_CALL(as2_longjmp);"))
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lifter = Lifter(iat_map=iat, func_names={}, lifted=lifted, precise_carry=True,
                    hook_sites=set(hooks))
    os.makedirs(outdir, exist_ok=True)
    stale = {fn for fn in os.listdir(outdir) if fn.startswith(prefix + "recomp_") and fn.endswith((".c", ".h"))}

    def write_if_changed(path, text):
        """Keep a file that already holds this text (and its timestamp: the object stays up to date)."""
        stale.discard(os.path.basename(path))
        if os.path.exists(path):
            with open(path, newline="\n") as fh:
                if fh.read() == text:
                    return
        with open(path, "w", newline="\n") as fh:
            fh.write(text)

    stats = collections.Counter()
    unimpl = collections.Counter()
    unimpl_sites = collections.defaultdict(list)
    failed = []
    entries = []
    t0 = time.time()
    chunk, idx, lines_total = [], 0, 0

    def flush():
        nonlocal chunk, idx, lines_total
        if not chunk:
            return
        p = os.path.join(outdir, f"{prefix}recomp_{idx:04d}.c")
        body = "\n\n".join(chunk)
        write_if_changed(p, f"/* AUTO-GENERATED from {module} by run_lift.py (pcrecomp lift32). DO NOT EDIT, DO NOT COMMIT. */\n"
                            f'#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n#include "{prefix}recomp_funcs.h"\n#include <math.h>\n#include <string.h>\n\n'
                            + body)
        lines_total += body.count("\n")
        idx += 1
        chunk = []

    for n, fd in enumerate(fds):
        a = fd["address"]
        try:
            if a in overrides:
                code = (f"/* OVERRIDE: {overrides[a]} */\n"
                        f"void sub_{a:08X}(void) {{ recomp_func_t _f = recomp_lookup_manual(0x{a:08X}u);"
                        f" if (_f) {{ _f(); return; }} RECOMP_UNIMPL(0x{a:08X}u, \"override missing\"); }}")
                stats["overridden"] += 1
            elif a in excluded:
                code = (f"/* EXCLUDED: {excluded[a]} */\n"
                        f"void sub_{a:08X}(void) {{ RECOMP_UNIMPL(0x{a:08X}u, \"excluded: not code\"); }}")
                stats["excluded"] += 1
            else:
                f = rebuild(md, img, base, fd)
                code = lifter.lift_function(f)
                for old, new in call_rewrites:
                    if old in code:
                        code = code.replace(old, new)
                        stats["setjmp_rewrites"] += 1
                if a in skippable:
                    hook = f"RECOMP_HOOK(0x{a:08X}u);"
                    if hook not in code:
                        raise ValueError("skippable function without its hook")
                    code = code.replace(hook, hook + " { extern uint32_t g_recomp_skip_function; if (g_recomp_skip_function) {"
                                        f" g_recomp_skip_function = 0; RECOMP_FLAGS_OUT(); esp += {skippable[a]}; RECOMP_RETURN(); }} }}", 1)
                    stats["skippable"] += 1
        except Exception as ex:  # noqa: BLE001
            failed.append((a, repr(ex)))
            code = (f"/* LIFT FAILED: {ex!r} */\n"
                    f"void sub_{a:08X}(void) {{ RECOMP_UNIMPL(0x{a:08X}u, \"lift failed\"); }}")
        for line in code.splitlines():
            if "RECOMP_UNIMPL(" in line:
                key = line.split('RECOMP_UNIMPL(', 1)[1].split('"')[1].split()[0] if '"' in line else "?"
                unimpl[key] += 1
                if len(unimpl_sites[key]) < 10:
                    unimpl_sites[key].append(f"{a:08X}")
            if "RECOMP_ICALL(" in line: stats["icall_sites"] += 1
            if "RECOMP_ITAIL(" in line: stats["itail_sites"] += 1
        if names.get(a):
            code = f"/* {names[a]} */\n" + code
        chunk.append(code)
        entries.append(a)
        if len(chunk) >= split:
            flush()
            el = time.time() - t0
            print(f"[*] {n + 1}/{len(fds)} lifted ({el:.0f}s)", flush=True)
    flush()

    write_if_changed(os.path.join(outdir, f"{prefix}recomp_funcs.h"),
                     "/* AUTO-GENERATED. DO NOT EDIT. */\n#pragma once\n#include <stdint.h>\n"
                     + "".join(f"void sub_{a:08X}(void);\n" for a in entries))
    write_if_changed(os.path.join(outdir, f"{prefix}recomp_dispatch.c"),
                     f'/* AUTO-GENERATED. DO NOT EDIT. */\n#include "recomp_types.h"\n#include "{prefix}recomp_funcs.h"\n\n'
                     f"const recomp_dispatch_entry_t {prefix}recomp_dispatch_table[] = {{\n"
                     + "".join(f"    {{ 0x{a:08X}u, sub_{a:08X} }},\n" for a in sorted(entries))
                     + "};\n" + f"const uint32_t {prefix}recomp_dispatch_count = {len(entries)};\n")
    write_if_changed(os.path.join(outdir, f"{prefix}recomp_imports.c"),
                     "/* AUTO-GENERATED: IAT slot -> import name, for diagnostics. */\n#include <stdint.h>\n"
                     "typedef struct { uint32_t slot; const char* name; } nfs_import_name_t;\n"
                     f"const nfs_import_name_t {prefix}nfs_import_names[] = {{\n"
                     + "".join(f'    {{ 0x{slot:08X}u, "{dll}!{nm}" }},\n' for slot, (dll, nm) in sorted(iat.items()))
                     + "    { 0, 0 } };\n")
    for fn in stale:   # files of a previous lift that this one no longer writes
        os.remove(os.path.join(outdir, fn))
    out = {"functions": len(entries), "files": idx, "c_lines": lines_total,
           "lift_failures": len(failed), "failures": failed[:200],
           "unimplemented_total": sum(unimpl.values()),
           "unimplemented": dict(unimpl.most_common()),
           "unimplemented_sites": dict(unimpl_sites),
           "overridden": stats["overridden"], "excluded": stats["excluded"],
           "icall_sites": stats["icall_sites"], "itail_sites": stats["itail_sites"],
           "setjmp_rewrites": stats["setjmp_rewrites"],
           "seconds": round(time.time() - t0)}
    json.dump(out, open(os.path.join(outdir, f"{prefix}lift_stats.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in out.items() if k not in ("failures", "unimplemented_sites")}, indent=1))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "catalog":
        cmd_catalog(a[1], a[2], a[3])
    elif a and a[0] == "lift":
        split = int(a[a.index("--split") + 1]) if "--split" in a else 400
        names = a[a.index("--names") + 1] if "--names" in a else None
        prefix = a[a.index("--prefix") + 1] if "--prefix" in a else ""
        cmd_lift(a[1], a[2], a[3], split, names, prefix)
    else:
        sys.exit(__doc__)
