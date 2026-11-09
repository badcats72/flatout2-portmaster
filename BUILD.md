# Building from source

Most players do not need this: the [Release](https://github.com/badcats72/flatout2-portmaster/releases) is ready to use. These steps are for changing the port.

This repository holds no game files and no generated code. The build turns **your own** `FlatOut2.exe` and `d3dx9_30.dll` into C, then compiles it. The scripts are for Windows PowerShell, and they have only been run on one machine, so expect to adjust things.

## You need

- The GOG / Zoom Platform `FlatOut2.exe` (MD5 `1f8cae2b47276ca134750ba438aa35ca`) and its `d3dx9_30.dll`
- [pcrecomp](https://github.com/sp00nznet/pcrecomp), the recompiler
- [Zig](https://ziglang.org/), used as the cross compiler for aarch64
- Python 3 with `capstone` and `pefile`
- `libSDL2-2.0.so.0` from a target console, in a folder of its own (`DevLibs` below)

## Steps

Set these to your own folders, in PowerShell:

```powershell
$repo = "<this repository>"
$py   = "<python.exe>"
$zig  = "<zig.exe>"
$pc   = "<pcrecomp folder>"
$libs = "<folder with libSDL2-2.0.so.0>"
$work = "$repo\work\lift"     # put FlatOut2.exe and d3dx9_30.dll here
$gen  = "$repo\work\gen"
```

1. Find the functions and lift the exe, then the DLL:

```powershell
& "$repo\scripts\catalog.ps1" -Python $py -Pcrecomp $pc -Work $work -Gen $gen
& "$repo\scripts\catalog_module.ps1" -Python $py -Pcrecomp $pc -Dll "$work\d3dx9_30.dll" -Base 0x00A00000 -Work $work -Gen $gen -Prefix d3dx_
```

2. Prepare the lifted code. Run this after every lift:

```powershell
& "$repo\scripts\lw\lw.ps1" -Python $py -Gen $gen
```

3. Compile the lifted code, then the runtime and the final binary:

```powershell
& "$repo\scripts\build_guest.ps1" -Zig $zig -Gen $gen -Out "$repo\work\guest-obj" -Pcrecomp $pc -Runtime "$repo\runtime\guest"
& "$repo\scripts\build_host.ps1" -Zig $zig -Guest "$repo\work\guest-obj" -DevLibs $libs -Out "$repo\work\host-build"
```

The result is `work\host-build\fo2.aarch64`. Copy it into `ports/flatout2/` next to `port/FlatOut 2.sh`.

After changing only the runtime (`runtime/`), running `build_host.ps1` again is enough. After changing `config/hooks.txt`, `config/overrides.txt` or `config/skippable.txt`, lift again with `scripts/run_lift.py lift`, then repeat steps 2 and 3.

`docs/NOTES.md` is the project's working journal: how the game was analysed, what was tried, and what failed.
