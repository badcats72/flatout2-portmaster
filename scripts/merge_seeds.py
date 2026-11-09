#!/usr/bin/env python3
"""merge_seeds.py <lift dir>: vtable, RTTI, jump-table and relocated-pointer seeds -> seeds_all.json (prints the count)."""
import json, sys

work = sys.argv[1]
seen, out = set(), []
for name in ("vtable_seeds.json", "rtti_seeds.json", "jumptable_seeds.json", "reloc_seeds.json"):
    try:
        entries = json.load(open(f"{work}/{name}"))
    except FileNotFoundError:
        continue
    for e in entries:
        if e["address"] not in seen:
            seen.add(e["address"])
            out.append(e)
json.dump(out, open(f"{work}/seeds_all.json", "w"))
print(len(out))
