# Compile the lifted game code (guest objects) for the handheld with zig cc, in parallel.
# The flags are the NFSU2 port's (ports/r36s/build_guest_vm.sh) without its optional x87
# window annotations; sources over 8 MB get -BigOpt (NFSU2 used -O1 for its few enormous functions).
#   build_guest.ps1 -Zig <zig.exe> -Gen <lifted C dir> -Out <object dir> -Pcrecomp <dir> -Runtime <android cpp dir> [-Jobs N]
# An object newer than its source is kept, so the script can be run again after a failure.
param(
    [Parameter(Mandatory)] [string]$Zig,
    [Parameter(Mandatory)] [string]$Gen,
    [Parameter(Mandatory)] [string]$Out,
    [Parameter(Mandatory)] [string]$Pcrecomp,
    [Parameter(Mandatory)] [string]$Runtime,
    [int]$Jobs = [Environment]::ProcessorCount,
    [string]$Opt = '-O2',   # optimization of the sources up to 8 MB
    # optimization of the bigger ones: FlatOut 2's physics (0x56C850..0x574030, recomp_0022.c) and the
    # 0x42B440 tree (recomp_0002.c) are in them and take most of a race frame, so -O2 there too
    [string]$BigOpt = '-O2',
    # without the local x87 window (NFS_X87_LW): the annotations stay inert, every function uses the global stack
    [switch]$NoLW
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$common = @('-target', 'aarch64-linux-gnu.2.28', '-mcpu=cortex_a53', '-fno-pie',
    '-DNFS_FIXED_GUEST_BASE=0x1000000000ULL', '-DRECOMP_TLS=__thread', '-DRECOMP_TLS_STRUCT',
    '-ftls-model=local-exec', '-fno-math-errno', '-fno-stack-protector',
    '-w', '-fwrapv', '-fno-strict-aliasing', '-fexceptions', '-ffp-contract=off', '-g0', '-fvisibility=hidden',
    '-include', "$Runtime\arm64_guest.h",
    '-DRECOMP_STRICT_ICALL', '-DRECOMP_LOCAL_REGS', '-DRECOMP_X87_WINDOW', '-DRECOMP_MEM_QUAL=',
    # x87 stack position in locals, and the local x87 window of the functions scripts/lw/lw.ps1 proved
    # (needs $Gen/nfs_lw.h and the annotated call sites: run lw.ps1 after each lift)
    '-DNFS_X87_LOCAL', '-DNFS_X87_LW',
    "-I$Pcrecomp\runtime\recomp32", "-I$Gen")
if ($NoLW) { $common = @($common | Where-Object { $_ -ne '-DNFS_X87_LW' }) }

# FlatOut2.exe (recomp_*) and d3dx9_30.dll (d3dx_recomp_*)
$todo = Get-ChildItem "$Gen\*recomp_*.c" | Sort-Object Length -Descending | Where-Object {
    $o = Join-Path $Out ($_.BaseName + '.o')
    -not ((Test-Path $o) -and (Get-Item $o).LastWriteTime -gt $_.LastWriteTime)
}
Write-Output "$($todo.Count) sources to compile, $Jobs at a time"
$running = @()
$failed = @()
foreach ($src in $todo) {
    while (@($running | Where-Object { -not $_.HasExited }).Count -ge $Jobs) { Start-Sleep -Milliseconds 500 }
    $opt = if ($src.Length -gt 8MB) { $BigOpt } else { $Opt }
    $obj = Join-Path $Out ($src.BaseName + '.o')
    $log = Join-Path $Out ($src.BaseName + '.log')
    $args = @('cc', '-c', $src.FullName, '-o', "$obj.tmp", $opt) + $common
    $p = Start-Process -FilePath $Zig -ArgumentList ($args | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) `
        -NoNewWindow -PassThru -RedirectStandardError $log
    $null = $p.Handle   # keeps the process handle, without which ExitCode reads back empty
    $p | Add-Member -NotePropertyName Src -NotePropertyValue $src
    $p | Add-Member -NotePropertyName Obj -NotePropertyValue $obj
    $running += $p
}
foreach ($p in $running) {
    $p.WaitForExit()
    if ($p.ExitCode -eq 0 -and (Test-Path "$($p.Obj).tmp")) {
        Move-Item -Force "$($p.Obj).tmp" $p.Obj
        Write-Output "  CC $($p.Src.Name)"
    } else {
        $failed += $p.Src.Name
        Write-Output "  FAILED $($p.Src.Name) (exit $($p.ExitCode)): $(Get-Content (Join-Path $Out ($p.Src.BaseName + '.log')) -TotalCount 5)"
    }
}
if ($failed.Count) { Write-Output "guest objects: $($failed.Count) failed"; exit 1 }
Write-Output "guest objects complete ($Out)"
