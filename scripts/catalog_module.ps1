# Seeds, catalog and lift of a game DLL lifted next to the exe (d3dx9_30.dll), at its fixed guest base.
#   catalog_module.ps1 -Python <python.exe> -Pcrecomp <dir> -Dll <original dll> -Base 0x00A00000 -Work <lift dir> -Gen <lifted C dir> -Prefix d3dx_
# The analysis copy (relocated to -Base) and its seeds (entry, exports, relocated code pointers) come from
# prepare_module.py; the jump-table passes are those of catalog.ps1.
param(
    [Parameter(Mandatory)] [string]$Python,
    [Parameter(Mandatory)] [string]$Pcrecomp,
    [Parameter(Mandatory)] [string]$Dll,
    [Parameter(Mandatory)] [string]$Base,
    [Parameter(Mandatory)] [string]$Work,
    [Parameter(Mandatory)] [string]$Gen,
    [Parameter(Mandatory)] [string]$Prefix
)
$ErrorActionPreference = 'Continue'
$scripts = $PSScriptRoot
$env:PCRECOMP = $Pcrecomp
New-Item -ItemType Directory -Force $Work | Out-Null
$copy = Join-Path $Work (Split-Path $Dll -Leaf)
& $Python -I "$scripts\prepare_module.py" $Dll $Base $copy "$Work\reloc_seeds.json"
$previous = -1
for ($pass = 1; $pass -le 4; $pass++) {
    $count = [int](& $Python -I "$scripts\merge_seeds.py" $Work)
    Write-Output "pass $pass : $count seeds"
    if ($count -eq $previous) { break }
    $previous = $count
    & $Python -I "$scripts\run_lift.py" catalog $copy "$Work\seeds_all.json" "$Work\catalog.json" | Select-Object -Last 1
    & $Python -I "$scripts\jump_table_seeds.py" $copy "$Work\catalog.json" "$Work\jumptable_seeds.json"
}
& $Python -I "$scripts\run_lift.py" lift $copy "$Work\catalog.json" $Gen --prefix $Prefix | Select-String '"functions"|lift_failures|unimplemented_total|seconds'
