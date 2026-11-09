#!/usr/bin/env python3
"""Single-precision local x87 window for hot functions (see runtime/guest/arm64_guest.h, NFS_LW_T).

The lift keeps the x87 stack in doubles, so every `fld dword` and `fstp dword` costs a conversion, and each
arithmetic operand another one. In the physics, whose values are all floats in memory, those conversions are a
large part of the cost on a small core. For the functions of the local window (scripts/lw) whose address falls in
a range of config/x87_float.txt, this script makes the local stack float and drops the conversions.
A function that reads or writes 64-bit floats in memory is left alone.

    x87float.py <lifted C dir> <nfs_lw_info.json> <x87_float.txt>

Run by lw.ps1, after the local window is annotated."""
import glob
import json
import re
import sys

gen, info_path, ranges_path = sys.argv[1:4]
info = json.load(open(info_path))
ranges = []
for line in open(ranges_path):
    line = line.split("#")[0].split()
    if len(line) >= 2:
        ranges.append((int(line[0], 16), int(line[1], 16), len(line) > 2 and line[2] == "strict"))   # the first range that matches applies

FUNC = re.compile(r"^void (sub_[0-9A-F]{8})\(void\) \{", re.M)
XCH = ("do { %s _t = _st[0]; _st[0] = _st[i]; _st[i] = _t; "
       "int64_t _ti = g_st_i64[0]; g_st_i64[0] = g_st_i64[i]; g_st_i64[i] = _ti; } while (0)")
ENTER = ("#undef NFS_LW_T\n#define NFS_LW_T float\n#undef _st\n#define _st (l_lw + l_fp_sp)\n"
         "#undef fp_xch\n#define fp_xch(i) " + XCH % "float" + "\n")
LEAVE = ("\n#undef NFS_LW_T\n#define NFS_LW_T double\n#undef _st\n#define _st (l_lwon ? l_lw + l_fp_sp : l_stw + l_fp_sp)\n"
         "#undef fp_xch\n#define fp_xch(i) " + XCH % "double" + "\n\n")

done = skipped = 0
for path in sorted(glob.glob(gen + "/recomp_[0-9]*.c")):
    text = open(path, newline="\n").read()
    if "#define NFS_LW_T float" in text:
        continue
    matches = list(FUNC.finditer(text))
    out, last, changed = [], 0, False
    for i, m in enumerate(matches):
        name = m.group(1)
        va = int(name[4:], 16)
        where = next((r for r in ranges if r[0] <= va < r[1]), None)
        if not where or not (info.get(name) or {}).get("lw"):
            continue
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[m.start():end]
        # strict ranges: no integer load or store either (a timer or a counter beyond 2^24 does not fit a float)
        if "double*)" in body or (where[2] and (": fild" in body or ": fist" in body)):
            skipped += 1
            continue
        body = body.replace("(double)*(float*)", "*(float*)").replace("{ double _v = fp_pop();", "{ float _v = fp_pop();")
        body = re.sub(r"\bsqrt\(_st\[", "sqrtf(_st[", body)
        out += [text[last:m.start()], ENTER, body.rstrip("\n"), LEAVE]
        last, changed = end, True
        done += 1
    if changed:
        out.append(text[last:])
        with open(path, "w", newline="\n") as fh:
            fh.write("".join(out))
print("x87float: %d functions in float, %d left in double" % (done, skipped), file=sys.stderr)
