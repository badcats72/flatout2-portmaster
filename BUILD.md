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

The runtime's profile of the exe (page hashes, import slots) and the switch tables it restores over a copy whose code differs are generated from the lifted exe and the lifted code. They are in the repository; generate them again after a new lift:

```powershell
& $py "$repo\scripts\make_exe_profile.py" "$work\FlatOut2.exe" "$repo\runtime\native\fo2_exe_profile.h"
& $py "$repo\scripts\make_text_tables.py" "$work\FlatOut2.exe" $gen "$repo\runtime\native\fo2_text_tables.h"
```

3. Compile the lifted code, then the runtime and the final binary:

```powershell
& "$repo\scripts\build_guest.ps1" -Zig $zig -Gen $gen -Out "$repo\work\guest-obj" -Pcrecomp $pc -Runtime "$repo\runtime\guest"
& "$repo\scripts\build_host.ps1" -Zig $zig -Guest "$repo\work\guest-obj" -DevLibs $libs -Out "$repo\work\host-build"
```

The result is `work\host-build\fo2.aarch64`. Copy it into `ports/flatout2/` next to `port/FlatOut 2.sh`.

After changing only the runtime (`runtime/`), running `build_host.ps1` again is enough. After changing `config/hooks.txt`, `config/overrides.txt` or `config/skippable.txt`, lift again with `scripts/run_lift.py lift`, then repeat steps 2 and 3.

## Optional: profile-guided optimisation

The binary in the Release is built this way; it makes the game's own code about 6 to 9% faster. Zig compiles with `-fprofile-generate` and `-fprofile-use`, but ships neither the profile runtime nor `llvm-profdata`: take both from the LLVM release matching `zig cc --version` (`llvm-profdata.exe` from the Windows installer, `compiler-rt/lib/profile` and `compiler-rt/include/profile` from the sources).

1. Compile the profile runtime (every `InstrProfiling*.c` except the Darwin, AIX, Fuchsia, Windows and Other platform files, plus `InstrProfilingRuntime.cpp`) for `aarch64-linux-gnu.2.28` with `-DCOMPILER_RT_HAS_ATOMICS=1 -DCOMPILER_RT_HAS_FCNTL_LCK=1 -DCOMPILER_RT_HAS_UNAME=1`.
2. Build an instrumented game, with those objects copied into its object folder:

```powershell
& "$repo\scripts\build_guest.ps1" -Zig $zig -Gen $gen -Out "$repo\work\guest-obj-pgogen" -Pcrecomp $pc -Runtime "$repo\runtime\guest" -ExtraFlags '-fprofile-generate','-mllvm','-enable-name-compression=false'
& "$repo\scripts\build_host.ps1" -Zig $zig -Guest "$repo\work\guest-obj-pgogen" -DevLibs $libs -Out "$repo\work\host-build"
```

3. Play a race on a console with `LLVM_PROFILE_FILE=/tmp/fo2.profraw` and `FO2_PROF_DUMP_SEC=170` in `fo2.env`: the counters are written 170 seconds after the first frame.
4. Merge the profile and build with it:

```powershell
llvm-profdata merge -o fo2.profdata fo2.profraw
& "$repo\scripts\build_guest.ps1" -Zig $zig -Gen $gen -Out "$repo\work\guest-obj-pgo" -Pcrecomp $pc -Runtime "$repo\runtime\guest" -ExtraFlags "-fprofile-use=<path>\fo2.profdata",'-Wno-profile-instr-out-of-date','-Wno-profile-instr-unprofiled'
& "$repo\scripts\build_host.ps1" -Zig $zig -Guest "$repo\work\guest-obj-pgo" -DevLibs $libs -Out "$repo\work\host-build"
```

The profile belongs to one lift: take it again after lifting again.

`docs/NOTES.md` is the project's working journal: how the game was analysed, what was tried, and what failed.
