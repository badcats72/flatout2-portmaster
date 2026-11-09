# Seeds and function catalog of FlatOut2.exe, then the lift to C.
#   catalog.ps1 -Python <python.exe> -Pcrecomp <dir> -Work <lift dir holding FlatOut2.exe> -Gen <lifted C dir>
# Seeds: pcrecomp's vtable and RTTI scans plus the targets of the switch jump tables, recomputed on each new
# catalog until a pass adds no table target (a table found inside newly decoded code can lead to more).
param(
    [Parameter(Mandatory)] [string]$Python,
    [Parameter(Mandatory)] [string]$Pcrecomp,
    [Parameter(Mandatory)] [string]$Work,
    [Parameter(Mandatory)] [string]$Gen
)
$ErrorActionPreference = 'Continue'
$scripts = $PSScriptRoot
$exe = "$Work\FlatOut2.exe"
$env:PCRECOMP = $Pcrecomp
if (-not (Test-Path "$Work\vtable_seeds.json")) {
    & $Python -I "$Pcrecomp\tools\cpp\vtable_scan.py" $exe --seeds "$Work\vtable_seeds.json" | Select-Object -Last 1
    & $Python -I "$Pcrecomp\tools\cpp\rtti.py" $exe --seeds "$Work\rtti_seeds.json" | Select-Object -Last 1
}
$previous = -1
for ($pass = 1; $pass -le 4; $pass++) {
    $count = [int](& $Python -I "$scripts\merge_seeds.py" $Work)
    Write-Output "pass $pass : $count seeds"
    if ($count -eq $previous) { break }
    $previous = $count
    & $Python -I "$scripts\run_lift.py" catalog $exe "$Work\seeds_all.json" "$Work\catalog.json" | Select-Object -Last 1
    & $Python -I "$scripts\jump_table_seeds.py" $exe "$Work\catalog.json" "$Work\jumptable_seeds.json"
}
& $Python -I "$scripts\sled_seeds.py" $exe "$Work\catalog.json"   # computed jumps into memcpy's movsd run
& $Python -I "$scripts\data_pointer_rescue.py" $exe "$Work\catalog.json"   # code pointers in vtables/tables dropped by discovery
& $Python -I "$scripts\run_lift.py" lift $exe "$Work\catalog.json" $Gen | Select-String '"functions"|lift_failures|unimplemented_total|seconds'
