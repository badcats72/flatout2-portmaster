# Technical notes

The record of the FlatOut 2 port to the RG34XX-SP (Knulli) by static recompilation, on the runtime of the
Alien Shooter 2 port (`../Alien Shooter 2 - Reloaded (Recomp)`, itself from the NFSU2 recompilation).
Dates: November 2026.

## 1. The game files (Zoom Platform release)

| File | What |
|---|---|
| `FlatOut2.exe` | 3,004,832 bytes, MD5 `1f8cae2b47276ca134750ba438aa35ca`, built 2006-09-29. PE32, image base 0x400000, keeps its `.reloc`. Sections `.text` 2.3 MB, `.data` 2.5 MB (mostly BSS), plus `TORQ_CX_` / `WILK_DX_` (tiny, low entropy: not a packer) |
| `fo2_zoom.dll` | Zoom's launcher shim (2024): one export `ZOOM_Init`, which talks to `steam_api.dll`. Replaced by a stub |
| `steam_api.dll`, `steamclient.dll`, `steam_32.dll`, `steam_api.ini` | a Steam API emulator used by the shim. Not used |
| `d3d9.dll`, `dxwrapper.dll/.ini` | dxwrapper (a Windows compatibility wrapper). Not used: the runtime is the D3D9 |
| `d3dx9_30.dll` | Microsoft D3DX (April 2006). The exe imports `D3DXCreateEffect`, `D3DXLoadSurfaceFromMemory`, `D3DXSaveSurfaceToFileA` |
| `data.bfs` (2.47 GB), `langpack.bfs`, `zoom_platform.bfs` | the game's archives (`filesystem` lists them). Over 2 GB: file offsets must stay unsigned |

**Data actually used: the CD release** (folder "FlatOut 2 No-Steam (Windows Version)"): `fo2a.bfs` (1.86 GB),
`fo2b.bfs` (1.21 GB), `fo2c.bfs`, `patch1.bfs` (patch 1.2) and their `filesystem` / `patch` lists, with
`FlatOut2.exe` and `d3dx9_30.dll` from above: the CD's own exe cannot be lifted, and this exe is
the same build without the copy protection. Nothing else of the Zoom release is used; the port's package
holds only the exe, D3DX and the CD's files. With these archives the boot reaches the same point
(`D3DXCreateEffect`).

## 2. What the exe needs

- **Graphics:** D3D9 (`Direct3DCreate9`) with **D3DX effects**: `data/shader/*` + `shaderlib_emu.ini` /
  `shaderlib_pro.ini`. Effects are compiled from HLSL at run time by D3DX, so `d3dx9_30.dll` is lifted as a
  second module (section 3).
- **Sound:** **FMOD 3.75** linked statically (`\Library\fmod375_win32\...`), with its own Ogg Vorbis
  (libvorbis again: `mdct_backward` / floor native overrides as in AS2). Output through DirectSound
  (`LoadLibrary("dsound.dll")`, `DirectSoundCreate`), or WinMM `waveOut*` as fallback. ACM (`msacm32`) for
  compressed WAV.
- **Input:** DirectInput 8 (`LoadLibrary("dinput8.dll")`), keyboard and joystick. The NFSU2 runtime already
  offers the pad as a DirectInput joystick (analog steering): the closest match of the three games.
- **Other:** WinSock + GameSpy (online/LAN: stubbed), keyboard hooks (`SetWindowsHookExA`), registry.
- Import coverage by the AS2 runtime: 167 of 242 imports. Missing ones are mostly trivial (waveIn, extra
  WinSock, hooks, time).

## 3. d3dx9_30.dll as a lifted module

- Its preferred base is 0x400000, the exe's. It is relocated to **0x00A00000** (`scripts/prepare_module.py`,
  from the NFSU2 project), between the exe image (ends 0x941000) and the first heap arena (0x10000000).
- Seeds: entry, exports, and every relocated pointer into code. Lifted with `run_lift.py --prefix d3dx_`.
- The runtime maps the original file at 0xA00000 with its relocations applied (`guest_modules.inc`), runs
  DllMain, and points the exe's IAT slots at the export VAs (`bindGuestModuleImports`).
- It imports **msvcrt.dll** (72 functions; FlatOut2.exe links its CRT statically): `runtime/guest/fo2_msvcrt.inc`.
  `_CI*` intrinsics work on the x87 stack; double results are pushed to ST0; `_adjust_fdiv` is a data import.

## 4. Runtime changes from AS2

- `RECOMP_ENTER` no longer hooks 0x005BF2E0 (NFSU2's GetRacingResolution): other code in this exe.
- Removed NFSU2's 0x0075bcc7 redirect; AS2's native overrides (libvorbis, longjmp) disabled until
  FlatOut2.exe's own addresses are found.

## 5. The lift

- Seeds: vtables, RTTI, switch jump tables (3-4 passes), and every relocated pointer into `.text`
  (`scripts/reloc_seeds.py`: the exe has relocations, so this catches callbacks the scans miss).
- Catalog: 16,430 functions after 4 passes (6,435 seeds). Lift: 5.4 M lines of C, 0 failures.
- Unimplemented instructions at first: 277 sites, nearly all data decoded as code (`int`, `iretd`, `bound`,
  `arpl`, `daa`...). The real ones were **`fcomi` / `fcmovcc`** at 0x562DA0 (min/max of a triangle's vertices
  in the collision code). pcrecomp's lift32 now lifts `fcomi`/`fucomi` (and `fcomip` against its actual
  ST(i), not always ST(1)) and the eight `fcmovcc` through the matching jcc condition. 193 sites left, all
  data.
- The config files must be UTF-8 without BOM (`run_lift.py` reads the first VA as `\ufeff...`).
- **Computed jumps into a sled** (`scripts/sled_seeds.py`): AMD's memcpy at 0x560F3C ends with
  `neg ecx; add ecx, 0x5610B4; jmp ecx` into 16 one-byte `movsd`. Each entry of the run is now a function.
- **Code pointers in data dropped by discovery** (`scripts/data_pointer_rescue.py`): a switch table at
  0x5AAB84 was decoded as code across 0x5AABA0, a function reached only through a table in `.rdata`.
  Every relocated pointer stored outside `.text` that targets code and is not a function start is now
  decoded on its own (6 functions).
- d3dx9_30.dll seeds: Microsoft merges read-only data into `.text`, so most relocated pointers there are
  data; `prepare_module.py` keeps only pointers at a plausible function start (7,578 -> 1,978).

## 6. Boot, step by step

| Stop | Cause | Fix |
|---|---|---|
| "Overlapped CreateFile requires asynchronous file service" | the game reads `filesystem` and the `.bfs` archives with `FILE_FLAG_OVERLAPPED` | overlapped requests served synchronously with `pread`/`pwrite` at the OVERLAPPED's 64-bit offset (data.bfs is 2.47 GB), completed on return (event signalled; `GetOverlappedResult` reports the count) |
| ITAIL unresolved 0x005610B1 | the memcpy sled above | `sled_seeds.py` |
| Segfault copying a string from NULL (0x44F62B) | FMOD's `FSOUND_GetDriverName(0)` returned NULL: no driver, because `LoadLibrary("dsound.dll")` failed (only DLLs of the import table had handles) | `dsound.dll` / `dinput8.dll` get a handle when loaded; `DirectSoundEnumerateA` reports one driver |
| ICALL unresolved 0x005AABA0 | function only reached through an `.rdata` table | `data_pointer_rescue.py` |
| `D3DXCreateEffect` | expected: needs the lifted d3dx9_30.dll | d3dx9_30.dll lifted, after `scripts/reachable_filter.py` (below) |
| ITAIL unresolved 0x00B2891B | d3dx switch case reached only through its jump table | the filter follows `jumptable_seeds.json` |
| Guest call stack imbalance at 0x00AFB326 | d3dx's `qsort` comparator is cdecl, called as stdcall | `callGuest(compare, {a, b}, 8)` |
| `kernel32.dll!GlobalAlloc` | FMOD's init | `GlobalAlloc` / `GlobalFree` on the process heap (no `GlobalLock` imported) |
| Segfault in 0x616C24 (FMOD, NULL list link) | FMOD gave up its output: the primary buffer is asked with `DSBCAPS_CTRL3D` (0x11) and was refused, so its lists were never set up | CTRL3D accepted on the primary buffer only; FMOD then mixes into its 16 KB secondary buffer |
| ICALL unresolved 0x006439E0 | a function registered by `push offset`, right after a jump table, decoded as part of the function before | `data_pointer_rescue.py` also takes pointers stored in `.text`, minus jump-table entries, and only to a plausible start (not a table, after padding / ret / a table): 9 functions. Without that test 116 came in, mostly switch index tables, and one decoded a `loope` into the middle of an instruction (Clang: undeclared label) |

| "press START button" answers nothing | FlatOut 2 reads the keyboard only through a `WH_KEYBOARD` hook (`SetWindowsHookExA(2, 0x55AAD0)`); it releases DirectInput once it has looked for joysticks. The hook was accepted and never called | PeekMessage / GetMessage call the hook procedure for each key message taken from the queue (HC_ACTION, virtual key, keystroke flags), as Windows does |
| (same screen, with a pad) | a DirectInput pad gets the keyboard's codes as its bindings (`Savegame/am<guid>.dat`: 20 actions x {action, code, type 0 key / 1 axis / 2 button, axis words}) and answers no button | the pad is not offered to the game (`FO2_DIRECTINPUT_PAD=1` offers it); the host turns its buttons into keys: d-pad/stick arrows, A Enter, B Ctrl, X Esc, Y R, R1 Space, L1 C, R2/L2 A/Z, Start Esc |

Also fixed on the way: `ShowWindow` / the first `SetFocus` now send `WM_ACTIVATEAPP` + `WM_ACTIVATE` as Windows
does (the game keeps the flag). The boot movies (0x520BB0) were skipped for a while; they play again since the native video decoder (section 8).

| Enter sometimes validated two screens | the keyboard hook was also called for `WM_CHAR` (0x102): TranslateMessage turns Enter into WM_CHAR 13, read as a second VK_RETURN | only WM_KEYDOWN/UP and WM_SYSKEYDOWN/UP reach the hook, as on Windows |
| "Phantom" Enter / Space every 4 s in the user's sessions | not the game: the test scripts restarted EmulationStation from a shell that exported FO2_AUTOPRESS (the runtime's test presses), and every game launched from it inherited it | the scripts restart EmulationStation with `env -u` of every test variable |
| 540x360: segfault in 0x5A59D0 (window / mode set-up) | the adapter's mode list (0x5B06A0, after EnumAdapterModes) rejects a mode narrower than 512 or lower than 384: an empty list, a null mode in the device table (0x450E78..) | any size from 512x384 up works: 576x384 is the smallest 3:2 one. Below that (the RG351P's 480x320) the two compares at 0x5B06FA / 0x5B0708 need a hook |

## 8. Performance (first race, 720x480, "very low" details)

About 3 FPS. Game thread 250-300 ms of CPU per frame, render thread idle ~220 ms: the guest code is the
bottleneck, not the GPU. A gdb sampling of the game thread (30 stacks) put more than half of it under
0x56C850 (0x5692B0, 0x570040, 0x571080, 0x572FB0, 0x573780: the physics) and a second group under 0x42B440
(0x42BCC0, 0x5639E0). All of these sit in sources over 8 MB (recomp_0022.c, recomp_0002.c), which the build
compiled at -O1 like NFSU2 did: `build_guest.ps1 -BigOpt` now defaults to -O2. About 13 synchronous D3D
calls per frame (device methods 38, 40, 34) also cost ~67 ms.

After -O2: ~5.8 FPS; game thread 116 ms CPU + 54 ms in ~12 synchronous D3D calls a frame (GetRenderTarget 1.8,
GetDepthStencilSurface 1.2, StretchRect 0.6, vertex buffer Lock/Unlock 1.2 each, texture GetLevelCount 0.9).
Made asynchronous (d3d9_render_thread.inc): GetRenderTarget / GetDepthStencilSurface answered from the surfaces
the game bound (or a first synchronous answer), with one more guest reference (FO2_ASYNC_TARGETS=0 turns these
off, with GetLevelCount); StretchRect queued; a static buffer locked a second time gets the dynamic buffers'
permanent guest copy (FO2_ASYNC_RELOCKS=0). GetRenderTarget stayed synchronous at first: the game Releases the
surface at once, the wrapper was retired and the next query made a new one; surfaces learnt from a query now keep
one bridge reference (released before Reset).

**Physics spiral.** After a long pause (gdb sampling: a 130 s frame) every race frame took 20-25 s and never
recovered: the game steps its physics at a fixed rate and catches up with all the time the last frame took, so a
slow frame makes the next one slower. The guest clock (QueryPerformanceCounter, timeGetTime, GetTickCount) now
loses, at each Present, whatever part of the frame went beyond FO2_MAX_FRAME_MS (default 50 ms; 0 = real time):
slow motion while the handheld cannot keep up, never a collapse. Race then: ~5-6 FPS, no synchronous D3D call
left, game thread 165-195 ms of CPU a frame.

**Local x87 window** (NFSU2's NFS_X87_LOCAL + NFS_X87_LW, scripts/lw from its ports/r36s/lw, globs widened to the
d3dx sources): analyze.py proves a constant x87 stack depth per function; those functions keep their FPU stack in
a local array whose slots GCC/Clang resolve to registers. 4,841 of the 5,041 functions using the x87 qualify; 7,496
call sites annotated. `scripts/lw/lw.ps1` must run after every lift, before build_guest.ps1. Result in a race:
game thread ~45-50 ms of CPU a frame (from 165-195), ~8.3 FPS, now limited by the render side (render thread never
idle, ~450 draws and ~2,500 queued commands a frame, the Mali driver's worker ~57 ms a frame).

**Render side.** The render thread often sat in the Mali driver's osup_sync_object_wait: every buffer Unlock was a
glBufferSubData, whatever the D3D lock flags, so writing into a buffer the GPU may still read made the driver wait
or copy it. Unlock now honours the flags (d3d9_gles_resources.h glesUploadLockedRange): DISCARD gives the buffer
fresh storage, NOOVERWRITE writes unsynchronized through glMapBufferRange (looked up with SDL_GL_GetProcAddress:
the devlibs' GLES3 header and stub library lack it). FO2_GL_BUFFER_FLAGS=0: as before. The GPU had no
"performance" governor (only simple_ondemand, 240-696 MHz, at 420 MHz in races): the launcher raises its
min_freq to the top frequency instead (pinning it gave 8.1 -> 8.5 FPS: not the main limit). The CPU is capped at
1.512 GHz of 1.704 (Knulli's overclock gate): left alone. Result: **~24 FPS in a race** (from ~8.3), the driver's
worker thread gone; wall ~42 ms a frame, game thread 24-29 ms of CPU, render thread ~30 ms and never idle (~430
draws a frame): the render thread is the next limit. (gdb sampling cannot profile it: attaching stops every
thread, so it is nearly always caught waiting for work.)

**Native pad, second try (after the keyboard hook fix).** Controller = 1 with the pad's GUID, the pad offered
(FO2_DIRECTINPUT_PAD=1), no keys from the host (FO2_PAD_KEYS=0), and am<guid>.dat rewritten with type-2
(button) bindings: first Enter -> A / Escape -> B, then every one of the 20 actions on button (action % 10).
The game polls the pad (GetDeviceState) but the title screen still answers no button, Start included. Not
pursued: the pad stays mapped to keys. 648x432 (3:2, 90%) works and is the default render size.

**Pad layout (keyboard keys, Xbox-like).** Three buttons mean different keys in a race and in the menus, so the
host needs to know which: `config/hooks.txt` puts a RECOMP_HOOK on 0x56C850 (the physics step, which runs only
in a race, not in its pause menu), recomp_hook() stamps the time, fo2InRace() = stepped in the last 300 ms.
Buttons by position (RG34XX-SP labels in brackets): bottom [B] Enter / accelerate (Up), right [A] Escape /
handbrake (Space), left [Y] - / reverse (Down), top [X] reset (R) in a race, R1 nitro (Left Ctrl), Select camera
(C), Start Escape, d-pad and left stick arrows (stick up/down only in menus), R2/L2 gear up/down (A/Z). The
mapping is re-applied when the state flips with buttons held (SdlGamepad::refresh each main-loop pass).

**Graphics back to "Very High" at every launch.** At boot the game loads Savegame/device.cfg (0x44F340) and
checks it (0x44F580): the stored adapter GUID, then the stored display mode (width/height/bits at bytes
120/124/128) among the adapter's modes. The runtime lists only FO2_RENDER, and the auto-configuration
(0x450AA0: on a capable adapter TextureQuality 4, Visibility 100, mode 1280x1024x32) stored a mode that never
matched, so it ran again at every boot and rewrote options.cfg with the highest settings. The launcher now
writes the render size into device.cfg before each launch: the check passes and the saved settings stay.

**Videos (blocky, "bad DivX").** The AVI player (0x4C8F00 / 0x4C8520) feeds a codec at 0x63A000-0x64F000
whose IDCT table (0x63FFF3) is filled with SSE2 routines (0x649360, 0x649860) unconditionally. Ruled out so far:
the async texture/surface lock paths (same blocks with AS2_ASYNC_SURFACE_LOCKS=0 NFSU2_ASYNC_TEX=0); the SSE2
lifting (scratchpad difftest of simd32.py against real SSE2 intrinsics, 20 forms x 20,000 random inputs: all
exact); fild/fistp qword copies (none in the codec). Masking SSE2 in CPUID (FO2_CPUID_EDX_OFF=4000000) changes
nothing; masking SSE stops the game at boot (it requires SSE). (History: the native decoder below replaced all this.) Videos were then turned off: config/hooks.txt puts a hook
at 0x451386, right after the game sets its video system's "on" flag 0x8DA61C (read only by the AVI player
0x4C8F00), and recomp_hook clears it, so every open takes the player's own "no video" path (FO2_VIDEOS=1 keeps
them). A still image then covered the black menu background; it was removed once the videos worked. Note: an earlier count of "5,255 silently dropped
xmm instructions" was wrong (simd32.py puts the code and the comment on separate lines).

**Shaders that never drew (graphics glitches).** Two kinds, both fixed in the runtime (third_party/mojoshader is
left as it is: its CLAUDE.md asks that no AI be used in contributions to it):
- 6 vertex shaders refused by MojoShader, "relative addressing unsupported without a CTAB": D3DX effects strip
  the constant table, and c[a0.x + n] needs one. d3d9_gles_shaders.h parses such a shader again with a minimal
  CTAB (one float4 array c0..c255) inserted after the version token of a copy.
- 7-9 programs failing to link, "Fragment shader uses a varying io_5_0 that has not been declared in the vertex
  shader": the game's pixel shaders with the runtime's fixed-function vertex shader (full-screen passes). D3D9
  allows reading an interpolator nobody wrote; GLSL does not. On that failure the program is linked again with a
  vertex shader declaring the missing varyings, set at the end of main(): io_5_N = v_tcN, io_10_0/1 =
  v_diffuse/v_specular, else zero. (Zero for the texture coordinates made one pass cover the whole race view with
  a single texel.) They are the game's bloom, ~8 ms a race frame; the user prefers the picture and the speed
  without it, so this is off by default (FO2_FIX_VARYINGS=1 turns it on). The CTAB fix did not change the debris
  glitches either.

**Clocks and memory.** The launcher now also sets the CPU policies to performance at their hardware top
(1.704 GHz instead of Knulli's 1.512 cap) while the game runs, restored on exit (FO2_CPU_BOOST=0 leaves them):
~20 -> ~23.5 FPS in the same race, 52-56 C. Memory: no limit applies (no rlimit, no cgroup cap, zram swap unused);
the game uses ~540 MB because that is what it loads, 1.26 GB stay available. Earlier note: the x87 code of the physics (a global FPU stack in memory:
every fld/fstp is a load/store of g_st_win and of the stack top).

**StretchRect on the GPU (~20 -> 26-39 FPS in a race).** gdb samples of the render thread (scratchpad
sample.sh: `gdb -p <render tid> -batch -ex bt`, my own race) showed it half idle, and StretchRect in
readSurfaceRGBA: the game copies the back buffer into two textures (512x432, 512x512) every race frame, and the
CPU path's glReadPixels waited for the GPU to finish the frame so far, then re-uploaded the pixels. Now
glBlitFramebuffer between a read and a draw framebuffer when both sides are in GL (the back buffer bottom-up,
textures in D3D row order: reversed Y ranges flip). Same picture; game thread ~20 ms, GPU ~17-19 ms a frame
now (FO2_GPU_STRETCH=0: the CPU path).

**Audio (first look).** FMOD 3.75 mixes in software into one 16 KB (93 ms) looping DirectSound buffer
(caps report no hardware buffers; the 3D primary is accepted, 3D secondaries refused). audio_mixer.h now
counts stream stretches played again without new data ([AS2 AUDIO] "stream stretches replayed"): none in a
race with SDL callbacks of 1024 or 512 frames (FO2_AUDIO_SAMPLES), so FMOD keeps up with the cursor. The
game's audio settings are ordinary (Settings.Audio.* in options.cfg). The user hears debris sounds cut off
half way: FMOD's software channels being stolen. FSOUND_Init is 0x61A3A9, called at 0x4207AA with (mixrate, 64,
0x2000) (found from the arguments on the guest stack at DirectSoundCreate, AS2_SOUND_TRACE; return addresses
read dead0000 in lifted code); a hook there (config/hooks.txt) raises the channels to 128 (FO2_FMOD_CHANNELS,
0 = the game's 64). Not confirmed by ear yet.

**Native video decoding (idea from a Reddit reader).** The AVI player only accepts `vp31` / `VP31` streams (the
check is at 0x4C90E1): On2 VP3.1, not DivX as first assumed, from 320x240 (menus) to 640x480 (attract demo).
0x4C8890 (eax = the player) takes the next queued compressed frame and writes a 24-bit DIB (+0x2DC, width +0x2B8,
height +0x2BC); it is replaced (config/overrides.txt, runtime/native/fo2_video.c) by the system's FFmpeg, opened
with dlopen (libavcodec.so.61 down to .58; no build dependency, nothing shipped), and a C YUV to BGR loop. Picture
clean, no blocks, right orientation and colours at the first try; menus 43-60 FPS, attract demo ~22 FPS. A hook at
0x4C8320 frees the decoder when the player closes. The videos now default to on when FFmpeg loads (the hook
at 0x451386 keeps the flag), off without it; FO2_VIDEOS=0 / 1 forces them. The boot movies (0x520BB0: nvidia,
empire, bugbear, intro) play again (their override is gone) and the still menu image is removed (code and file).

**HUD at half rate (same reader): not worth it, measured.** The HUD is 0x477EC0 -> 0x4B88D0: 17 draws of ~470 in a
race, 1.1 ms a frame on the game thread (measured with two temporary hooks, 0x477ED9 and 0x477EF6). The NFSU2 way (hud_half.c: run it every other frame,
replay its D3D calls in between) would need replaying its Lock/memcpy/Unlock batches and copes badly with the
game's own state cache (0x8DA7AC...: it skips SetStreamSource when it believes it is already set). Before building
that, the HUD was removed altogether in a race (the wrapper's object pointer cleared by those hooks), the upper bound of any gain. Four auto races: with HUD
38.7 and 35.8 FPS, without 38.3 and 36.2: no measurable difference (the game thread is not the limit in those
frames; GPU and render thread are). Not implemented.

**Debris sounds cut right after they start (investigation).** FO2_FMOD_TRACE=1 logs the FMOD entries the
game's sound layer (0x411000-0x422000) calls most (hooks in config/hooks.txt): 0x616483 PlaySoundEx(FREE,
sample, 0, paused), 0x615830 SetPaused, 0x615B77 SetVolume (clamped 0-255), 0x6155A8 SetFrequency,
0x61AB73 (channel, position*, NULL: 3D attributes, no velocity), 0x615B31 (channel only: `ret 4`, a channel
method through vtable +0x1B8, every frame), 0x6154FD (stop?), 0x61598D. Measured: 468 of 521 sounds started in a race are
never stopped by the game and get no further volume change; start volumes range over 0-255 as expected.
Samples are plain PCM (FSOUND_SAMPLE after its 256-byte name: length, loop points, mode 0x8131/0x8132 =
LOADMEMORY|SIGNED|MONO|16BITS|LOOP_OFF/NORMAL, 44.1 kHz, 0.2-2 s): no decoder involved. So FMOD itself ends
them early (cause not found). A Doppler theory came from reading 0x615B31 as 3D_SetAttributes(ch, pos,
vel): it takes the channel only, and the "vectors" were the caller's stack. Writing a pointer into that
stack (an FO2_FMOD_NOVEL test, removed) crashed the game at the profile screen: the trace hook only reads.
Seen in passing: in the menus "stream stretches replayed" reaches 183-427 of 1327 (FMOD feeds its stream
late there; 0 in races).

**HUD torn after StretchRect went to the GPU.** User A/B tests through `flatout2/fo2.env` (the launcher
exports its VAR=value lines): FO2_GPU_STRETCH=0 -> no tearing (~20 FPS); FO2_GL_BUFFER_FLAGS=0 -> no tearing
(~10 FPS); FO2_CPUID_EDX_OFF=800000 (FMOD without its MMX mixer) -> sound unchanged. Without the per-frame
glReadPixels the GPU runs a frame behind, and the HUD, rewritten in place each frame with NOOVERWRITE, was
written unsynchronized under the previous frame's draws. Now the first NOOVERWRITE update of a buffer in a
frame orphans it (glBufferData with the CPU copy, like DISCARD); later ones in that frame stay unsynchronized
(d3d9_gles_resources.h glesUploadLockedRange, orphanFrame). That alone did not stop the tearing (user test):
the HUD is also rewritten in place within a frame, and on the Mali (a tiler) a frame's draws run when it is
flushed, after such a write. So each buffer now records the bytes this frame's draws have read
(GlesBufferUse::noteDraw, from DrawPrimitive / DrawIndexedPrimitive), and a NOOVERWRITE update overlapping
them orphans the buffer too. Orphanings per frame are in the "[NFSU2 PERF] buffer updates" line.

**The HUD smear was the particle fix (final).** The user noticed it only when physics objects moved; fast
framebuffer captures (racetest.ps1 -Sleep 0.12) showed the HUD smeared horizontally. With FO2_PARTICLE_FIX=0
FO2_DRAW_RESYNC=0 the HUD was intact; each half alone too. The game-thread version (edit the guest copy,
clear it at DISCARD, resend changed bytes before each draw) is gone. The repair now runs on the render thread
in GL order (d3d9_gles.cpp patchParticleStrip, before prepareDraw in DrawPrimitive): perspective draws only
(c3 from the real constants), zero vertices and jumps of more than FO2_PARTICLE_JUMP (4) world units to a
vertex that is not doubled (the game joins particles with doubled vertices) become a copy of the previous
vertex, sent as a NOOVERWRITE update; the vertex buffer clears the particle ranges at its next DISCARD
(IDirect3DVertexBuffer9GLES::Lock). "[FO2 PARTICLES]" lines count strips and patched vertices (up to ~1,500
vertices a frame in a crash). Result: HUD intact, no streak from a spark to the camera, sparks like the PC's.
The bridge keeps only FO2_SPARKS=0 (drop those draws). The NOOVERWRITE orphaning above stays (it guards a
real hazard and costs ~2 orphanings a frame).

**HUD torn after the particle fix (first guess, superseded).** Clearing the whole 1 MB guest copy at DISCARD erased HUD quads the game
draws again without rewriting them. Now only the ranges particle strips drew from since the previous DISCARD
are cleared (BufferShadow::particleRanges).

**Streaks and white flashes when the body scrapes (the "debris" glitch).** Long bright lines from a contact
point across the screen, sometimes a white screen, and stretched pieces. Ruled out one by one in auto races
(scratchpad race2.sh / racetest.ps1: FO2_AUTOPRESS=a holds the throttle in a race, framebuffer every 0.3 s):
the buffer lock-flag paths (FO2_GL_BUFFER_FLAGS=0 FO2_ASYNC_RELOCKS=0), SSE2 (FO2_CPUID_EDX_OFF=4000000, which
now also hides it from IsProcessorFeaturePresent(10), so the CRT takes its x87 maths), the local x87 window
(a whole build without NFS_X87_LW: `build_guest.ps1 -NoLW`), rdtsc (only random seeds), MojoShader's depth
correction and vertex precision (highp). MojoShader's `vs_a0.x = int(x)` truncates where D3D rounds to the
nearest: rewritten as `int(floor(x + 0.5))` (d3d9_gles_shaders.h roundAddressRegister, FO2_A0_ROUND=0 off),
correct but not the cause. A frame trace (touch /tmp/as2_trace; the trace now prints each DrawPrimitive's
positions: tracePositions) found it: the particles (sparks, dust) are triangle strips of 24-byte vertices
(position, colour, uv) from a 1 MB dynamic buffer, drawn with a plain position x c0..c3 shader and ONE/ONE
blending, one fixed-size slot per particle; the end of some slots is never written by the lifted code (24 zero
bytes: a position at the origin, i.e. at the camera, or older vertices: other particles, HUD quads), so the
strip runs from the spark to there. The guest copy holds the same bytes (not a transfer bug: d3d9_render_thread
resyncDrawRange found nothing to send), so the cause is in the lifted particle code and was not found. Worked
around in the bridge (FO2_PARTICLE_FIX=0 off): a DISCARD lock of a dynamic buffer of 512 KB or more clears its
guest copy (D3D keeps no contents across DISCARD), and before a triangle-strip draw of 24-byte vertices from
such a buffer every zero vertex becomes a copy of the one before it (degenerate triangles); the changed bytes
are queued before the draw. Sparks then look like the PC game's (short rays from the contact point).
FO2_SPARKS=0 drops these draws altogether (no streak at all, no sparks or dust either).

Hunt for the writer, not concluded. `FO2_VB_LOCK_LOG=1` + gdb `break fo2_lock_mark` / `bt` names the lifted callers
of the big buffer's lock (the ring 0x5B0F50): the generic mesh draw 0x5AD450 (most of them; it copies
`vertices x stride` bytes from the CPU array at mesh+0x2C and draws with mesh+0x14 type, mesh+0x1A primitives),
HUD 0x5AB5D0, billboards 0x59ED70, post-processing 0x5A7F80. Read and found correct: the billboards (four whole
vertices per particle, `rep movsd`) and the tyre marks (0x58BE00, a ring of 16 strips, not in that buffer).
The code filling the strips' CPU array was not identified. A stricter FO2_PARTICLE_JUMP=1.5 did not remove the
last long ribbons in captures, so the default stays 4.

**Back buffer copies (StretchRect).** They feed only the bloom, off by default. Skipping them, four A/B race pairs:
51.7/57.2, 42.4/48.6, 47.6/48.5, 39.4/49.8 FPS (+6 on average, noise up to 9). Now skipped unless the bloom is
on (FO2_SKIP_STRETCH=0 keeps them).

**576x384 against 648x432 (21% fewer pixels): about 5% in the heavy zone.** Four A/B race pairs at the launcher's low
settings, end of race: 576x384 44.5, 55.4, 54.8, 54.8 FPS; 648x432 41.3, 57.9, 57.4, 57.0 (near the 60 FPS ceiling,
so no difference shows). Heavy zone (PERF lines with at least 350 draws, the pack at the start of a race), three
races each: 576x384 40.0 / 39.4 / 45.0 FPS at 449 / 455 / 406 draws; 648x432 36.7 / 36.4 / 42.4 at 466 / 477 / 405.
The draw count varies between races, so compare frame time per draw: 54.7-55.8 us against 57.7-58.5, ranges that do
not overlap. The default stays 648x432 (the user's choice: 2-3 FPS do not justify the softer picture). A test at a size the game refuses makes its auto-configuration write the highest settings into
options.cfg: test scripts must apply the launcher's sed and device.cfg step (four first A/B runs were invalid
because of it).

**Draw-call profile (heavy zone).** 590-835 draws/frame at the start of a race give 30-36 FPS, against ~210 draws at 58
FPS in calm scenes. Render thread about 8.5 ms + 21.6 us/draw, GL driver about 15.5 us/draw, game thread 19-22 ms, GPU
21-28 ms: the three are close, so cutting one alone gains little. The state cache already drops 22,727 requested GL
state changes to 2,072 per frame. Traced frame: 520 draws, of which 40 to the 256x256 shadow texture.

**glDrawElementsBaseVertex (FO2_DRAW_PATH): no clear gain, default stays 0.** 1 is glDrawElementsBaseVertex, 2 is
glDrawRangeElementsBaseVertex, both instead of moving the attribute pointers for each base vertex. Frame time per draw in
the heavy zone: path 0 57.2-63.8 us (six races), path 1 53.9-62.3 (four), path 2 56.5-58.6 (three). The ranges overlap.
Path 2 also trusts the game's NumVertices, which D3D9 does not enforce.

**Unattended races.** FO2_AUTOPRESS only holds the throttle, so the car ends against a wall and reports a parked car's
frame rate. FO2_AUTORESET=8 taps the reset-car button every 8 s of race. It does not steer: the car loses the pack, so
the heavy zone is still only the start of the race.

**Sounds of debris cut short: not found.** FO2_FMOD_WATCH traces of FMOD 3.75's software channels: the game neither
stops, pauses nor mutes the channels; 317 one-shots tracked played 90-100% of their expected length but one;
the samples are complete PCM with a natural decay; rolloff is 1 and only 7 of 250 one-shots lose 70% of
their 3D attenuation within 400 ms. Not the output stream (no replays), the voice count (64 or 128) nor the MMX
mixer. Hardware voices would change nothing: the samples are created without FSOUND_HW3D/HW2D (mode 0x8131/0x8132),
so FMOD mixes them in software whatever the caps say. Left: FO2_FMOD_ROLLOFF by ear, or a native mixer.

**Sounds of debris cut short: found and fixed.** The user's ears settled it: FO2_FMOD_ROLLOFF=0 removes the cuts (and all
localisation: 0x61D375 then uses a linear law over min..max, with max 1e9), 0.3 brings them back. Keeping each
one-shot at its starting distance (tried, removed) did not help, because the cut is not a fade. FMOD's PlaySound
(0x616054) puts a new sound on the listener with gain 255; the game then sends its position (0x61AB73), but the gain
follows only at the next FSOUND_Update, and the mixer thread can run in between (FMOD mixes before it locks the
output, so Lock is too late to correct it). The first 23 ms block of a distant sound was mixed up to 20 times too
loud, then dropped: heard as an attack cut off. Measured on the first look at each one-shot: 56 of 130 attenuated
sounds at full gain. The earlier traces missed it because they skipped each sound's first block.
A hook at 0x61AB73 (config/hooks.txt, fmodStartGain in guest_audio.inc) applies the gain 0x61D375 will compute, for
one-shots not yet past their first block, only ever lowering it. FO2_FMOD_STARTGAIN=0 disables. After it, the only
sounds still at full gain (33 of 249) have channel flag 0x200, set while PlaySound runs. Confirmed by ear: no more cuts,
localisation natural. Loops are left alone: a loop's position comes back under 2048 at every turn.
FO2_FMOD_WATCH=1 now also logs [FMOD3DCH] (attenuation inputs), [FMODFIRST] (first look, at Lock and Unlock) and [FMODLOOP].

**"Slow motion" in races is frame pacing, not game speed.** The user feels short slowdowns while the counter stays above
30 FPS. FO2_FRAME_LOG=1 logs each frame's time and physics steps ([FF], 10 ms of game time a step; automatic race at
the launcher's clocks and --cap60). Game time follows real time exactly (speed 1.000; lowest half second 0.91): nothing
slows down. Frames alternate: 80-84% take one panel refresh, 16-19% two, physics runs 1 to 4 steps a frame.
Limited to 30 FPS: 99.8-100% of frames at two refreshes, frame-to-frame change over 10 ms in 0.1-0.5% of frames (9-13% at 60),
always 3 or 4 steps, lowest half second 0.99. Vsync off (FO2_SWAP_INTERVAL=0, accepted by the driver) changes nothing
measurable. FO2_CAP in fo2.env sets the launcher's limit. The default stays 60: after trying 30 the user preferred the higher
frame rate, despite the more even pacing.
Shadow pass (FO2_SHADOW_EVERY=N, default 1; 40 draws, 8,368 of 129,000 triangles): frames over 25 ms and FPS, two races
each in mirrored order: every frame 17.9% / 17.9%, 48.5 / 48.4 FPS; one frame in 2 15.7% / 17.2%, 49.4 / 48.8; practically
never 14.4% / 12.0%, 50.0 / 51.7. Halving it is within the noise and even removing it gains only ~2.4 FPS, so it stays on.
Game thread profile (ptrace sampler, 3,000 samples from the start of a race): no hot spot. The hottest lifted function is 1.9%
(sub_00570040), the top 5 are 7.6% of all samples, the top 20 19.2%, the top 100 44.1%. Lifted code is 62%. About 22% of
the samples sit at one address in a shared library, almost certainly a blocking wait (the callers were not resolved), so
in those frames the game thread is not the limit. The D3D queue (rt::push 3.9%, cachedObject, filterState, tryDefer,
the call shortcut) is about 7%. Rewriting functions natively would have to cover dozens to gain a little: not worth it.
FO2_MAX_FRAME_MS (the "physics spiral" guard above) is dead code in this mode: guestFramePresented sits in
dispatchGuestD3D9, which a deferred Present never reaches. No spiral has been seen in a race since the render thread.

**RG351P (dArkOS, Debian 13, RK3326: 4 x Cortex-A35 1.296 GHz, Mali-G31 520 MHz, 895 MB, zram swap).** The same binary boots
and races at the first attempt, launched by the same script (KMSDRM, 480x320, cap 30, sysfs through $ESUDO: the launcher
picks them from /proc/cpuinfo's Hardware line). The mode filter (0x5B06FA / 0x5B0708) is passed by two hooks. Automatic race, low
settings: 25.5 FPS mean, 81% of frames at two refreshes, 15% at three, 3% at six (about 100 ms), game speed 0.999. First
launch runs at the auto-configuration's highest settings (19-23 FPS); the launcher lowers them from the second launch on.
FO2_SWAP_ABXY=1 (launcher, this console) swaps A/B and X/Y; FO2_VIDEO_THREADS=3 and FO2_VIDEO_FAST=1 (no deblocking), with
a NEON YUV conversion, made the boot movies smooth by the user's report.
Slowdowns to 17-20 FPS last about 5 s and are physics: frames at 54 ms against 35, draws unchanged (~320), 5-6 physics steps of
10 ms a frame instead of 3.3, game time still exact. Profile by one-second slices (slow against normal, ms of a frame):
sub_00571080 5.2 against 1.1 (leaf, 4 KB, 319 x87 instructions, no calls), sub_00570040 3.5 against 0.8, the rest spread
over the physics range 0x562000-0x575000. Slow frames cost more steps, and each step costs more: load feeds on itself.
FO2_PHYSICS_HALF=1 (tried, rejected by the user, off everywhere: the game feels in slow motion and a stopped car jitters, though
the frame times below are better): the 100 Hz tick is hard-wired (0.01 at dozens of sites in the cars' logic),
but the physics world step 0x56C850 takes its time step as an argument and computes forces inside it, so it now runs one
tick in two with 0.02 (config/skippable.txt, g_recomp_skip_function). Two races each, mirrored order: 100 Hz 27.6 / 28.0 FPS, frames
over 50 ms 4.8% / 3.6%, 6 / 3 seconds under 20 FPS; 50 Hz 29.8 / 29.1 FPS, 0.2% / 1.3%, 0 / 1 second. Game thread 20-27 ms against
23-45.

**Physics x87 in single precision (scripts/x87float.py, config/x87_float.txt).** The lift keeps the x87 stack in doubles, so each
`fld dword`, `fstp dword` and arithmetic operand costs a float/double conversion; the physics' data are 32-bit floats. For the
143 local-window functions in 0x562000-0x575000 the local stack is float (NFS_LW_T, per function) and the conversions are gone,
without touching their logic (run by lw.ps1 after each lift). RG351P, automatic races: 29.1 / 28.6 / 29.6 FPS against 27.6 / 28.0,
frames over 50 ms 0.2 / 1.5 / 0.7% against 4.8 / 3.6%, seconds under 20 FPS 0 / 1 / 0 against 6 / 3. Applies to every console (same binary).
Later runs of the same build vary (27.9-28.6 FPS, 0 and 4 seconds under 20): the gain is real but the noise between races is large.
Tried after it on the RG351P, no gain: float over the whole game code (1,605 functions: 29.1 / 28.9 / 28.8 FPS), all four cores
(FO2_CPU_ALL=1: 27.8 / 27.6 FPS against 28.6 / 27.9 on cores 1-3, more jitter). Render thread at the start of a race (750-784 draws, 39 ms): 55% in
libmali, about 20% in the bridge spread over many functions, so only fewer draw calls would help it. L2 + R2 restarts the FPS counter's statistics.

**The player's car sometimes silent (user report, both consoles): not reproduced.** Four automatic races on the RG351P with
FO2_FMOD_WATCH=1: in each one the player's loops (distance 0-8, channels 12-18 and 21) have full 3D gain and non-zero volumes (up to 130-229).
The "Language: Spanish" of the log is NFSU2's default written to a registry value (guest_kernel.inc); not checked whether FlatOut 2 reads it.

**GKD Pixel II (ROCKNIX under Sway, Cortex-A35 1.512 GHz, Mali-G31 520 MHz, 975 MB, panel 480x640 turned by Sway).** The same binary runs at the first
attempt, through Wayland (the launcher picks wayland, 640x480, cap 30 from /proc/device-tree/model). Automatic race: 28.7 FPS, 94% of frames at two refreshes,
over 50 ms 0.5%, no second under 20 FPS, game thread 20-34 ms, GPU load 39%, audio through PulseAudio, FFmpeg 60. A large SSH copy killed the first Wi-Fi adapter: the
game data went by SD card.

**Camera 4 (FO2_CAM4=0 disables).** Cameras are data: each car's camera.ini (inside the .bfs, plain Lua, `Cameras={[1]=...}`, five entries for the player's cars). A hook at
0x524276, in the Lua chunk reader, appends a script at the end of those files: a copy of camera 1 with height +0.30, distance x0.96 and its target 0.35 higher, so it looks less down at the road (tuned with the user), inserted as camera 4 (the old 4 and 5
become 5 and 6). The game's files are not changed. The frame counter is hidden at launch; L2 + R2 shows it (fresh statistics) or hides it.
The archives also hold LANCreateGame.bed, LANLobby.bed, LANGame.bed and LANResults.bed: the LAN mode exists in the data.

**GOG / Zoom data only since 2026-11-09.** The Zoom release's first boot movie, `data/video/nvidia.avi` (1280x720), carries its logo; a hook at 0x4C8F00 (config/skippable.txt) skips the file, and the NVIDIA logo goes with it (FO2_SKIP_LOGO=0 keeps it).

**LAN (not working yet).** fo2_imports.inc now implements the overlapped UDP calls (WSASendTo, WSARecvFrom, WSAWaitForMultipleEvents, WSAGetOverlappedResult), WSAStringToAddressA,
GetAdaptersInfo and the ordinals #11 #12 #103 #108 #151. A session can be created (ports 23756 and 23757), broadcasts leave, but a console that receives a packet crashes:
thread g6 (0x4E58B0), sub_005070C0, reading [ebp+0x1128] with ebp = 0 (0x5076E7 or 0x507791), just after the packet handler 0x506160. The thread's argument, ebp's value, is still on the stack
(0x6D9C18 at [esp+0x5C]), so a callee did not give it back. Whether a saved ebp was overwritten is not established: the capture does not reach that far below esp.

**The d3dx9_30.dll filter.** Lifting all 17,876 catalogued functions gave 5 GB of C: four sources Clang
refused ("ran out of source locations"), nine with branches to missing labels. Most of it is data decoded as
code: Microsoft merges `.rdata` into `.text`, and the HLSL compiler's string tables ("texCUBE", "tex3Dproj"...)
made thousands of overlapping pseudo-functions. `reachable_filter.py` keeps what the exe can reach: roots are
DllMain, the 3 imported exports and every relocated pointer to a plausible function start (prepare_module's
prologue rule); edges are direct calls and branches, relocated pointers inside kept code (and the vtable runs
they point to) and switch cases. 4,872 functions, 57 MB of C, 2,463 unimplemented sites.

By then: CRT, archives, FMOD, DirectInput and the D3D9 device (720x480 on the Mali, GLES 3.2 context) are up,
and the game has written `Savegame/options.cfg` and `device.cfg`.
