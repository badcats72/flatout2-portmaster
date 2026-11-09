# Local x87 window data for the lifted C (arm64_guest.h NFS_X87_LW), from the NFSU2 port (ports/r36s/lw):
# strip old annotations, analyse the x87 stack depth of every function, annotate the call sites, write nfs_lw.h.
# Run after every lift (run_lift.py rewrites the sources), before build_guest.ps1.
#   lw.ps1 -Python <python.exe> -Gen <lifted C dir>
param(
    [Parameter(Mandatory)] [string]$Python,
    [Parameter(Mandatory)] [string]$Gen
)
$ErrorActionPreference = 'Continue'   # the tools report progress on stderr
$info = Join-Path $Gen 'nfs_lw_info.json'
$steps = @(
    @('strip', $Gen),
    @('ANALYZE', $Gen, $info),
    @('apply', $Gen, $info),
    @('header', $Gen, $info, (Join-Path $Gen 'nfs_lw.h')))
foreach ($s in $steps) {
    if ($s[0] -eq 'ANALYZE') { & $Python -I "$PSScriptRoot\analyze.py" $s[1] $s[2] 2>&1 | Select-Object -Last 2 | ForEach-Object { "$_" } }
    else { & $Python -I "$PSScriptRoot\annotate.py" @s 2>&1 | ForEach-Object { "$_" } }
    if ($LASTEXITCODE) { throw "lw: step $($s[0]) failed ($LASTEXITCODE)" }
}
# Hot functions in single precision (config/x87_float.txt)
& $Python -I "$PSScriptRoot\..\x87float.py" $Gen $info "$PSScriptRoot\..\..\config\x87_float.txt" 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE) { throw "lw: x87float failed ($LASTEXITCODE)" }
