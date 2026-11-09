# Compile the host side (SDL2 host, guest runtime, D3D9 -> GLES 2 layer, MojoShader) with zig and link it
# with the guest objects into fo2.aarch64. The flags are the NFSU2 port's (ports/r36s/build_cross.sh).
#   build_host.ps1 -Zig <zig.exe> -Guest <guest object dir> -DevLibs <dir with the device's libSDL2-2.0.so.0>
#                  -Out <dir> [-Jobs N]
# libGLESv2 and libEGL are linked against stubs with the device libraries' sonames (the real ones are the
# Mali driver's, on the device).
param(
    [Parameter(Mandatory)] [string]$Zig,
    [Parameter(Mandatory)] [string]$Guest,
    [Parameter(Mandatory)] [string]$DevLibs,
    [Parameter(Mandatory)] [string]$Out,
    [int]$Jobs = [Environment]::ProcessorCount
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$rt = "$root\runtime"
$sdk = "$rt\sdk\include"
$dxvk = "$sdk\dxvk\native"
$mojo = "$root\third_party\mojoshader"
New-Item -ItemType Directory -Force "$Out\obj", "$Out\stubs" | Out-Null

$target = @('-target', 'aarch64-linux-gnu.2.28', '-mcpu=cortex_a53')
$opt = @('-O2', '-g', '-DNDEBUG', '-fno-strict-aliasing', '-ffp-contract=off')
$mojoDef = @('-DSUPPORT_PROFILE_ARB1=0', '-DSUPPORT_PROFILE_ARB1_NV=0', '-DSUPPORT_PROFILE_HLSL=0', '-DSUPPORT_PROFILE_METAL=0',
    '-DSUPPORT_PROFILE_D3D=0', '-DSUPPORT_PROFILE_BYTECODE=0', '-DSUPPORT_PROFILE_SPIRV=0', '-DSUPPORT_PROFILE_GLSPIRV=0',
    '-DMOJOSHADER_DEPTH_CLIPPING', '-DMOJOSHADER_FLIP_RENDERTARGET')
$sdlInc = @("-I$sdk", "-I$sdk\SDL2")
$glesInc = @("-I$rt\d3d9_gles", "-I$mojo", "-I$sdk\dxvk", "-I$dxvk", "-I$dxvk\windows", "-I$dxvk\directx") + $sdlInc
$appInc = @("-I$sdk\dxvk", "-I$rt\host", "-I$rt\d3d9_gles", "-I$rt\guest", "-I$rt\native", "-I$sdk\recomp32", "-I$sdk\win32hle") + $sdlInc +
    @('-isystem', $dxvk, '-isystem', "$dxvk\windows", '-isystem', "$dxvk\directx")
$appDef = @('-DRECOMP_TLS=__thread', '-DRECOMP_TLS_STRUCT', '-DNFS_PARALLEL_GUEST', '-DNFS_FIXED_GUEST_BASE=0x1000000000ULL',
    '-DNFS_R36S', '-DNFS_GLES', '-DNFS_D3D9_BACKEND', '-DNFS_SDL_AUDIO', '-DRECOMP_STRICT_ICALL', '-DRECOMP_X87_WINDOW',
    '-DFO2_D3DX_LIFTED') + $mojoDef

$units = @(
    @{ src = "$rt\d3d9_gles\d3d9_gles.cpp"; cmd = 'c++'; args = @('-std=c++17') + $opt + $mojoDef + @('-DNFS_R36S', '-DNFS_GLES') + $glesInc }
    @{ src = "$rt\host\main.cpp"; cmd = 'c++'; args = @('-std=c++17') + $opt + $appDef + $appInc }
    @{ src = "$rt\guest\guest_runtime.cpp"; cmd = 'c++'; args = @('-std=c++17') + $opt + $appDef + $appInc }
    @{ src = "$rt\guest\d3d9_bridge.cpp"; cmd = 'c++'; args = @('-std=c++17') + $opt + $appDef + $appInc }
    @{ src = "$rt\native\as2_native.c"; cmd = 'cc'; args = $opt + $appDef + $appInc }
    @{ src = "$rt\native\fo2_video.c"; cmd = 'cc'; args = $opt + $appDef + $appInc }
)
foreach ($c in 'mojoshader.c', 'mojoshader_common.c', 'profiles\mojoshader_profile_common.c', 'profiles\mojoshader_profile_glsl.c') {
    $units += @{ src = "$mojo\$c"; cmd = 'cc'; args = $opt + $mojoDef + @('-DNFS_R36S', '-DNFS_GLES', "-I$mojo") }
}

$procs = @()
foreach ($u in $units) {
    while (@($procs | Where-Object { -not $_.HasExited }).Count -ge $Jobs) { Start-Sleep -Milliseconds 300 }
    $name = [IO.Path]::GetFileNameWithoutExtension($u.src)
    $obj = "$Out\obj\$name.o"
    $log = "$Out\obj\$name.log"
    $a = @($u.cmd, '-c', $u.src, '-o', $obj) + $target + $u.args
    $p = Start-Process -FilePath $Zig -ArgumentList ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) `
        -NoNewWindow -PassThru -RedirectStandardError $log
    $null = $p.Handle
    $p | Add-Member -NotePropertyName Unit -NotePropertyValue $name
    $procs += $p
}
$failed = 0
foreach ($p in $procs) {
    $p.WaitForExit()
    $log = "$Out\obj\$($p.Unit).log"
    $errors = @(Select-String -Path $log -Pattern 'error:' -ErrorAction SilentlyContinue)
    if ($p.ExitCode -ne 0) {
        $failed++
        Write-Output "  FAILED $($p.Unit) (exit $($p.ExitCode)), $($errors.Count) errors:"
        $errors | Select-Object -First 15 | ForEach-Object { Write-Output "    $($_.Line.Trim())" }
    } else { Write-Output "  CC $($p.Unit)" }
}
if ($failed) { Write-Output "host: $failed failed"; exit 1 }

# GLES / EGL stubs with the device sonames, one empty function per entry point of GLES2/gl2.h and EGL/egl.h
# (the device's libGLESv2/libEGL forward to the Mali driver; at run time the real ones are loaded)
$ErrorActionPreference = 'Continue'
$gl = [regex]::Matches((Get-Content -Raw "$sdk\GLES2\gl2.h"), 'GL_APIENTRY\s+(gl[A-Za-z0-9_]+)\s*\(') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$egl = [regex]::Matches((Get-Content -Raw "$sdk\EGL\egl.h"), 'EGLAPIENTRY\s+(egl[A-Za-z0-9_]+)\s*\(') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
($gl | ForEach-Object { "void $_(void){}" }) -join "`n" | Set-Content -Encoding ascii "$Out\stubs\stub_gles.c"
($egl | ForEach-Object { "void $_(void){}" }) -join "`n" | Set-Content -Encoding ascii "$Out\stubs\stub_egl.c"
& $Zig cc -shared -fPIC @target -o "$Out\stubs\libGLESv2.so.2" "-Wl,-soname,libGLESv2.so.2" "$Out\stubs\stub_gles.c"
& $Zig cc -shared -fPIC @target -o "$Out\stubs\libEGL.so.1" "-Wl,-soname,libEGL.so.1" "$Out\stubs\stub_egl.c"

$objs = @(Get-ChildItem "$Out\obj\*.o" | ForEach-Object FullName) + @(Get-ChildItem "$Guest\*.o" | ForEach-Object FullName)
# (the shared libraries by path: zig's driver does not pass -l:<file> through)
$link = @('c++', '-o', "$Out\fo2.aarch64") + $target + $objs + @("$DevLibs\libSDL2-2.0.so.0",
    "$Out\stubs\libGLESv2.so.2", "$Out\stubs\libEGL.so.1", '-lpthread', '-ldl', '-lm', '-rdynamic', '-no-pie',
    '-Wl,--allow-shlib-undefined')
$result = & $Zig @link 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    $undef = [regex]::Matches($result, 'undefined symbol: (\S+)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    Write-Output "link failed$(if ($undef) { ", undefined: $($undef -join ' ')" })"
    Write-Output ($result.Substring(0, [Math]::Min(4000, $result.Length)))
    exit 1
}
Write-Output "built $Out\fo2.aarch64 ($((Get-Item "$Out\fo2.aarch64").Length) bytes); stubs: $($gl.Count) gl, $($egl.Count) egl"
