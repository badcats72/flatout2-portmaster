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

**Render thread and GPU, second pass (2026-11-12, RG34XX-SP; not installed yet: test binary `fo2_test.aarch64` on the console).**
Method: automatic race with FO2_AUTORESET=8, "heavy zone" = PERF blocks with at least 350 draws (the pack at the start). Races differ
a lot (412 to 541 draws on average in that zone), so only repeated A/B pairs count. Before: 37.9 / 39.6 FPS in the heavy zone, 43-44
over the race. After everything below: 50.2 to 54.3 in the heavy zone, 53 to 56 over the race.
- `FO2_RT_PROF=1` (d3d9_render_thread.inc) prints the wall time of the render thread's queued commands by kind (op/method, per
  frame, with the calls over 0.3 ms). It showed the thread blocked in the driver: SetRenderTarget (method 37) 3 to 8 ms a frame for
  22 calls, buffer updates (op 7) 3.6 to 4.4 ms, Clear (method 43) up to 4 ms.
- **One framebuffer object per target** (bindRenderTarget, m_fboCache; FO2_FBO_CACHE=0: the single re-attached object). The Mali
  flushes, and can wait for, the pending rendering of an object whose attachments change. SetRenderTarget: 1.5 ms a frame. The cache
  is dropped whenever a texture or renderbuffer is deleted (glsc::attachmentGeneration).
- **Tiler hints** (FO2_TILER_HINTS=0 off): a Clear of the whole target goes without the scissor test; Present invalidates the back
  buffer's depth and stencil; the buffer the next frame starts in is cleared whole (colour only in a race: menus and loading screens
  redraw parts of the picture). The game itself clears only depth (flags=2) on the back buffer, so the GPU loaded the old picture
  back every frame. That last clear gave black frames in races with two presenter buffers (user report; 7 of 90 framebuffer captures,
  none with the hints off): the buffer was cleared right after its presentation, before the presenter context's copy to the screen
  had run. Now three buffers (NFSU2_PRESENT_SLOTS, default 3) and a buffer is released one presentation later (presenter::heldSlot),
  at once when nothing else is queued; with two buffers that clear is not done. 270 captures in three races: no black frame.
- **Lazy storage for large dynamic buffers** (GlesBufferUse::lazy, FO2_LAZY_BUFFERS=0 off; dynamic, 64 KB or more): an orphaned
  buffer gets empty storage instead of the whole CPU copy (a 1 MB ring, twice a frame), updates only mark 1 KB pages invalid, and each
  draw uploads the pages it reads that the storage lacks (unsynchronized map). Indexed draws from such a buffer take their vertex
  range from the indices themselves (noteBufferReads), not from the game's MinVertexIndex / NumVertices. Buffer updates: 0.6 to 0.9 ms.
- **No `discard` when the alpha test is off** (PS variant key bit 20, kAlphaTestBit; FO2_NO_DISCARD=0 off). Every pixel shader ended
  with the alpha test block, which keeps a tile-based GPU from writing depth early and killing hidden pixels. Two A/B pairs: heavy
  zone 50.3 / 52.2 FPS without it, 41.2 / 40.4 with it; GPU wait 16-18 ms against 19-24. The GPU is fill-bound: at 480x320 it falls
  under the refresh period in the heavy zone.
- **Dead targets** (m_deadTargets, FO2_DEAD_TARGETS=0 off): the bloom chain still runs with the bloom off (two 512x432 passes, a
  256x256 ping-pong of 5 to 7 passes, composites); its programs do not link, so its draws are dropped, but its 5 clears a frame were
  render passes. A Clear is skipped on an off-screen target whose last draw had no program. Clear: 0.4 ms a frame.
- No gain, left alone: the cars' shadows (8 targets of 256x256, one per car; FO2_SHADOW_EVERY=100000: 49.5 FPS against 53.1, within
  the noise). `FO2_EXP_NODRAW=1/2` (diagnostics) skips the GL draw call or the whole draw.
- The "presenter waited for the GPU" figure has a floor of 13-14 ms at 60 FPS even in menus with 11 draws: it includes the wait for
  the panel's refresh, so it only means GPU time above that.
- **Command ring prefetch** (rt::push and rt::loop): the 5 MB ring is far larger than the caches, so each slot was a miss; the
  slots four calls ahead are now prefetched, for writing on the game thread and reading on the render thread. Ptrace sampler,
  2,500 samples each in the heavy zone: push 6.3% -> 1.6% of the game thread, loop 2.3% -> 1.3% of the render thread.
- Game thread profile in the heavy zone (same sampler): the game's render layer (0x58-0x5C) 18%, game logic 19%, engine
  (0x52-0x56) 12%, physics 8.5%, d3dx9 effects 6%, the bridge about 14% before the prefetch, waiting 9%. No hot function (the
  hottest lifted one is under 2%). The trigonometry seen in gdb stacks was noise (libm and compiler_rt: 0.5% in all).
- Installed on the RG34XX-SP on 2026-11-12 (MD5 bf1b19939839c2ddb533c991b0a15b07); v1.0.0 kept beside it as fo2.aarch64.v100.
  Last automatic races: 52.4 and 56.5 FPS in the heavy zone, 51 to 56 over the race.
- Where it stands in the heavy zone: game thread 17-21 ms of CPU (now the limit), render thread 15-18 ms (9-11 ms in ~450
  DrawIndexedPrimitive, of which ~15 us a draw inside the driver), GPU about 16 ms. The PERF line's "game-thread CPU" is really the
  presenting (render) thread's. Never attach gdb in a race
  (it froze the game for 15 s): use the ptrace sampler. Sparks to compare by eye against the installed binary (FO2_LAZY_BUFFERS=0 to rule the buffers out).

**Game thread: what does not help (2026-11-13, RG34XX-SP).** The game thread is the limit in the heavy zone (16-17 ms of CPU for a
16.7 ms budget; wall 18.6 to 20.7 ms). Races differ by about 8% in game-thread time for the same binary (15.7 to 17.4 ms), so only
gains of 10% or more can be measured (`sum2.sh` in the session scratchpad: CPU of both threads per 100 draws). Tried, no measurable gain:
- State blocks: the game captures and applies one about 400 times a frame (D3DX effects). FO2_EXP_NOBLOCK=1 (diagnostics, render
  thread only, wrong picture possible) drops them all: both threads stay at 17 ms for ~540 draws.
- Hot functions packed together: the code a race runs is 819 functions, 2.05 MB of 62.9 MB (ptrace sampler, 44,000 samples of
  both threads). `build_guest.ps1 -ExtraFlags -ffunction-sections` and `build_host.ps1 -OrderFile <linker script>` (zig refuses
  --symbol-ordering-file; a script with `SECTIONS { .text.fo2hot : { *(.text.<function>) ... } } INSERT BEFORE .text;` works) bring
  them from 845 to 535 pages of 4 KB. Two A/B pairs: 16.3 / 17.4 ms without, 17.3 / 15.7 with. Not kept.
- -O3: zig cc gives the same binary as -O2 (same MD5).
- Huge pages: the Knulli kernel (4.9.170) has neither transparent huge pages nor hugetlbfs.
- Calling convention (each lifted call writes and reloads 11 registers and the flags): a build with -DNFS_COUNT_CALLS counts
  18,000 to 25,500 lifted calls a frame in a race, about 0.6 ms at most.
- Trigonometry: 0.5% of the samples.
Left with the size needed: profile-guided optimisation of the lifted code (zig ships neither the profile runtime nor
llvm-profdata: it needs an LLVM download), and, for the GPU (about 16 ms in the heavy zone, fill-bound), a render scale applied in
the bridge when frames run late. The installed binary is still the one of 2026-11-12 (bf1b1993...).

**Music stutter and a crash without a sound device (GitHub issue 1, R50S, ArkOS4Clone, 2026-11-15).** The user plays with the
music at 0%, and so did every test until now: nobody had listened to the music. FMOD mixes everything (music and sounds) into one
looping DirectSound buffer of 16,384 bytes (93 ms); its mixer thread (entry 0x61DB85) and its stream thread (0x617A65) are guest
threads, so they only run when the game thread lets the guest machine go. When they are late the output played the ring's old
contents again ("stream stretches replayed"). RG34XX-SP, music at 80% (`MUSIC=80` in the session's raceopt.sh sets both volumes
for a run and puts the user's file back): 6,633 of 45,094 granules replayed before the race (menus and above all the loading
screen), none in the race.
- **Queue between FMOD and the output** (PcmVoice::fifo, audio_mixer.h; FO2_AUDIO_FIFO=0 off, FO2_AUDIO_DELAY_MS start depth,
  default 20): the blocks the guest publishes while the ring plays are queued and played in order; a dry queue gives silence, not
  old sound, then waits for the target depth, which grows to the length of the dry spell (spells up to 150 ms; longer ones are
  loading screens). Each 30 s without a dry spell, if more than the start depth plus 10 ms was always left, the queue is played 1%
  faster down to that. Result: no replay at all; in a race no dry spell even with every frame 60 ms longer
  (NFSU2_SLOW_RACE_MS=60), queue back at 44-48 ms; before the race 38-46 dry spells remain (loading): silence instead of repeats.
- **Silent output when the device cannot be opened** (startSoundStream): the attached log shows "ALSA: Couldn't open audio device",
  DirectSoundCreate failing, then a segmentation fault. SDL's dummy driver now takes over (the cursors keep moving) and the log
  says "NO SOUND". Tested with AUDIODEV=hw:7,0: the race runs.
- **The real fix is to run FMOD's two threads in parallel**: `NFSU2_PARALLEL_THREADS=61db85,617a65` (the runtime's NFSU2
  mechanism, compiled in since v1.0.0, default list empty for this game). Replayed before the race: 45 of 41,083. But one start in
  about thirty ended with "Guest thread 3: Unbalanced LeaveCriticalSection" (LeaveCriticalSection now logs the section, count and
  owner). Not the default until that is understood; usable from fo2.env. Suspects: InitializeCriticalSection replacing the mutex
  of a section another thread is entering; the deadlock resolver in enterCritical reading another thread's wait/handle pair.
- Yielding the machine while holding a guest critical section (FO2_YIELD_WITH_LOCKS=1, test switch) changes nothing: 6,419 of 45,063.

**Profile-guided optimisation of the lifted code (2026-11-15, kept; installed on the RG34XX-SP, MD5 e608d870...).** zig 0.17 is
clang 22.1.8 and takes `-fprofile-generate` / `-fprofile-use`, but ships neither the profile runtime nor llvm-profdata. Both are in
`work/pgo/` (not in the repository): `tools/llvm-profdata.exe` taken out of LLVM-22.1.8-win64.exe with 7-Zip, `profile-rt/` =
compiler-rt/lib/profile and include/profile at tag llvmorg-22.1.8. Steps:
1. Runtime: each InstrProfiling*.c except the Darwin / AIX / Fuchsia / Windows / Other platform files, plus InstrProfilingRuntime.cpp, with
   `zig cc -target aarch64-linux-gnu.2.28 -O2 -fno-pie -I<parent of profile-rt as "profile"> -DCOMPILER_RT_HAS_ATOMICS=1
   -DCOMPILER_RT_HAS_FCNTL_LCK=1 -DCOMPILER_RT_HAS_UNAME=1`; put the objects in the instrumented guest object folder (build_host links every .o there).
2. `build_guest.ps1 -Out work\guest-obj-pgogen -ExtraFlags '-fprofile-generate','-mllvm','-enable-name-compression=false'` (the Windows
   llvm-profdata has no zlib: compressed names cannot be merged), then build_host.ps1 with that folder.
3. On the console: `LLVM_PROFILE_FILE=/tmp/fo2.profraw FO2_PROF_DUMP_SEC=170` (d3d9_gles.cpp writes the counters that many seconds
   after the first frame, since test runs end with SIGKILL). The instrumented game is only about 10% slower.
4. `llvm-profdata merge -o fo2.profdata fo2.profraw`, then `build_guest.ps1 -Out work\guest-obj-pgo -ExtraFlags
   "-fprofile-use=<fo2.profdata>",'-Wno-profile-instr-out-of-date','-Wno-profile-instr-unprofiled'` and build_host.ps1.
The profile used (`work/pgo/fo2-rg34xxsp-autorace.profdata`: boot, menus, loading and one automatic race, 21,354 functions) must be
taken again after any new lift. Result, automatic races in turn with the build without it: game thread 14.88, 14.48, 13.45, 13.75,
13.60, 13.75 ms (mean 13.99, 395 draws) against 14.93, 16.59, 17.54, 14.84, 15.62 (mean 15.90, 427 draws), about 6 to 9% at equal
load; race 55.8 FPS on average against 53.5. The installed binary is built from work/guest-obj-pgo.

**GKD Pixel II with the 2026-11-15 build (PGO, render-thread and GPU changes, audio queue), 2026-11-16.** Installed (MD5 e608d870...,
v1.0.0 kept as fo2.aarch64.v100). Automatic races through the launcher, 150 s, PERF blocks with 200 draws or more (session scripts
gkd.ps1 / gkdsum.sh): v1.0.0 at the launcher's 30 FPS cap 26.5 FPS (game thread 28.8 ms, render thread 24.2), with FO2_CAP=60
30.8 FPS (slowest block 54.8 ms); new build 28.3 FPS at the cap (game 23.6 ms, render 18.5) and 40.0 FPS with FO2_CAP=60 (slowest
block 35.2 ms, game 20.9, render 18.8). Audio: 3,599 and 1,952 of 53,070 granules replayed with v1.0.0, none with the queue (6 to 8
dry spells in 80 s, loading included). Picture not captured there (no grim on ROCKNIX): `touch /tmp/fo2_shot` now saves the next
frame as screenshot_N.ppm (in the source, not yet in the installed binary); the user checks it by eye.

**Shader hitches after the no-discard variants (user report on the GKD Pixel II, 2026-11-16).** The user felt "bigger drops" in
destruction. Their log: frames of 0.13 to 0.89 s in a race with `compile 1/765.4 ms` or `link 1/108.7 ms`, and 16 s at the race
start with `link 22/15444.5 ms`. Two faults of the program cache, on every console:
- A program in the binary cache still had both its shaders compiled at every run, at their first use. Now shaders of the game's
  draws are only created with their source (compileGLShader's `lazy`, FO2_LAZY_SHADERS=0 off) and compiled when a program really has to be
  linked (ensureShaderCompiled in getProgram). RG34XX-SP, compiles inside hitches: 19 with the build before, 8 at the first run, 0 after.
- Programs that cannot link (the bloom chain) were linked again at every run. An empty `<key>.fail` file now remembers them
  (ProgramBinaryCache::knownFailure / saveFailure; not used with FO2_FIX_VARYINGS=1). RG34XX-SP: 7 remembered, then 7 skipped.
The new pixel shader variants themselves are compiled and linked once per console, then come from the cache: the first race after
an update still stutters. Installed on the RG34XX-SP (MD5 9acebea6..., PGO guest objects). The GKD still has e608d870...: it dropped
off the network during the copy of this build (fo2.new may be left there, partial).
Also from that log: 301 dry spells of the audio queue in 282 two-second reports, in menus and races: on the RK3326 the queue turns
constant lateness into constant short silences, so the queue is not enough there. Next: FMOD's threads in parallel
(NFSU2_PARALLEL_THREADS=61db85,617a65) on that console, and the cause of its rare "Unbalanced LeaveCriticalSection".

**FMOD's threads in parallel by default (2026-11-16).** NFS_SOUND_THREAD_ENTRY is now "61db85,617a65" (mixer and stream threads;
`NFSU2_PARALLEL_THREADS=` empty puts them back under the machine). GKD Pixel II, music at 80%, 30 FPS cap, one automatic race each:
under the machine 104 dry spells of the audio queue, 60 frames over 50 ms in the race, 26.8 FPS, slowest block 51.6 ms; in parallel 2
dry spells, 1 frame over 50 ms, 29.9 FPS, slowest block 34.1 ms. The stream thread (Ogg decoding) was taking the machine from the
game thread: the "machine 74.5" hitches of the R50S report. RG34XX-SP with music: 1 dry spell in a session against 39.
The rare start-up error ("Unbalanced LeaveCriticalSection" in thread 3): 1 in 78 starts, then 1 in 150 after initCritical and
deleteCritical were made safe against a thread that has taken the mutex but not yet written its count (both now decide with the
mutex; a section initialised again keeps its mutex). The one capture: section 105e55b0 (a heap object), count 0, owner 0, lock
count -1, no re-initialisation or deletion logged. Cause not found. LeaveCriticalSection on a section nobody holds is now ignored,
as on Windows, and logged with the last operations on that section (csNote / csDump: E entered, L left, R / S the deadlock
resolver, I, D, U). Installed on the RG34XX-SP (MD5 9192018b...). The GKD still has 9acebea6... (threads under the machine): it was
off the network when this build was ready. To send a build there: gzip it (30 MB instead of 95) and `scp -l 6000`.

**Second RG351P (dArkOS, Debian 13; the first one no longer powers on).** The user copied the old
"RG351P - pret a jouer" folder there (CD archives with the GOG / Zoom exe, a build from before v1.0.0, kept as fo2.aarch64.old; old
launcher kept as flatout2/launcher.old). Installed: build 9192018b... and the current launcher. Automatic races, music at 80%, 170 s
(session scripts r351.ps1 / run351.sh / consum.sh): old build at the 30 FPS cap 26.3 FPS, 65 frames over 50 ms in the race (its
shader cache was being filled), 17,111 of 108,748 audio granules replayed; new build 28.8 FPS, 4 frames over 50 ms, nothing
replayed, 2 dry spells; with FO2_CAP=60 37.9 FPS, slowest block 34.5 ms. Picture and sound to be judged by the user.

**The player's car silent, its jet flames missing (about one race in three for the user): found and fixed, 2026-11-16.**
Not reproduced by 41 automatic races (career, single race, camera changes): the engine loops were always audible. The user's test:
Single Race, Canal, Water Canal 1, car "Rocket" (two presses to the left in the car list); the flames show at once whether it is a
good start. `FO2_AUTOPRESS` takes a list, one press every 4 s: six `a`, `dpright`, then `a` reaches a single race on Timberlands 1.
In a bad race (FO2_FMOD_WATCH=1 in fo2.env, [FMODLOOP] lines): the player's seven engine loops (the channels at distance 8.0 from
the listener) are playing, all at volume 0; in a good one a band is audible (153 at idle). gdb on the running game (stopped, not
racing): the volumes come from 0x423D70 -> 0x419F60 -> 0x41A7F0, called once per car about every 50 to 110 ms with eax = the
car's engine sound state and [esp+4] = the milliseconds since the last update. That function smooths the engine speed (+0x64,
towards +0x24, clamped to +0x34..+0x38), the load (+0x68, towards +0x28, clamped to 0..1) and a start-up wobble (+0xA0, +0xA4); a
NaN passes every clamp (unordered compares) and stays for the race. The guard in recomp_hook (fo2EngineSoundGuard,
config/hooks.txt 0x41A7F0, FO2_ENGINE_GUARD=0 off, 2 log only) puts non-finite values in +0x60..+0xC0 back to rest and replaces a
time step that is not positive by 1 ms. The user's ten starts after it: ten good (sound and flames). The log of that session shows
it at work on most cars: "+68 = -nan" and "+b8 = -nan" with time steps of 100 to 110 ms, once "load -1.70141e+38": the load the
function is given is not initialised yet at the first updates of a race (the cause upstream, in the caller, was not looked for).
The flames come back with it, so they follow this state. Tools: scratchpad `nanscan` (a process's guest memory through /proc/pid/mem,
no stop: default NaNs by address, or a dump of words), `carch2.sh` / `camtl.sh` (loops per channel, per second). The hook was added
by hand to work/gen/recomp_0001.c as the lifter would (`RECOMP_HOOK(0x0041A7F0u);` after the entry label); only that file was
compiled again (guest-obj and guest-obj-pgo). Installed on the RG34XX-SP: MD5 4cbb1e7d...

**State before the user's v1.1.0 measurements (2026-11-16).** Build 4cbb1e7d... (PGO guest objects, engine sound guard) is on the
three consoles: RG34XX-SP, GKD Pixel II, second RG351P. Control races with music at the old
30 FPS cap: GKD 30.0 FPS, 2 frames over 50 ms, 1 dry spell; RG351P 29.1 FPS, 3 frames over 50 ms, none. One launcher from now on:
`port/FlatOut 2.sh` defaults to 60 FPS on every console (FO2_CAP=30 in fo2.env for a steady 30; a copy named "... 30 FPS" still
caps at 30), pushed to the three consoles (previous one kept as flatout2/launcher.prev). The release package, README and BUILD.md
are still those of v1.0.0 (two launchers, no PGO step, old numbers).

**v1.1.0 published, 2026-11-16.** The user's runs, one lap, same track and car as for v1.0.0 (average / minimum / 1% low): RG34XX-SP
52 / 40 / 20 (was 39 / 28 / 23), RG351P 33 / 20 / 18 (was 26 / 15 / 15, on the other console of that model), GKD Pixel II 36 / 23 / 17
(was 25 / 12 / 12). Repository commit 6d009c2 (README with the before / after table and screenshots, BUILD.md with the PGO steps,
sources), release v1.1.0 with FlatOut2-ARM64-port.zip (MD5 ddf4c158f280172178bbd4c56cf1f195, checked after download: one launcher,
binary 4cbb1e7d...), and a reply on issue 1. The published docs/NOTES.md has the CD-exe section and the LAN addresses taken out.

**LAN works (2026-11-17).** The crash of a console receiving a LAN packet (0x5070C0 reading [ebp+0x1128] with ebp = 0) was the
runtime's own: a WSARecvFrom left pending (WSA_IO_PENDING) kept the game's WSABUF array, "received" and "flags" pointers, all on
the network thread's stack, and when the packet came wsaov::receive wrote 0 through the flags pointer, by then a saved ebp. The
array is now copied at the call and nothing is written through those pointers afterwards (Windows does not write them either once
the call has returned pending). Found with a crash report now built into the runtime (nfsInstallCrashHandler, guest_runtime.cpp:
signal, faulting address, guest registers, host call chain by frame pointers, resolved with the binary's symbols: the chain was
sub_005070C0 <- sub_004E58B0 <- callGuest <- guestThreadEntry; the probes put around the packet handler 0x506160 never ran, so the
fault was before it). First LAN race, user's two consoles (RG34XX-SP hosting, RG351P joining, FO2_LAN=1 in fo2.env): both into the
race, position 2/2, packets both ways (22 to 109 bytes to the other console's port 23756, broadcasts of the host to 23757),
RG34XX-SP 59.5 FPS and RG351P 52.7 FPS on the grid with the cars still (no driving measured yet), no crash. The RG351P runs the CD
data, the RG34XX-SP the GOG / Zoom data: the boot logo screen differs, both sessions joined anyway. The LAN probes (FO2_LAN_TRACE,
RECOMP_HOOK lines added by hand to work/gen/recomp_0015.c) are still in the build. Still to do: a whole race driven on both,
leaving and starting a second race, the default (FO2_LAN is off unless set), then take the probes out.

**LAN with four consoles (2026-11-17).** A whole race was finished by three (RG34XX-SP hosting, the second RG351P, the GKD Pixel
II). The first RG351P is killed at every LAN race start by the kernel's OOM killer (dmesg: fo2.aarch64 anon-rss 452 MB), not by a
crash of the port: none of the dArkOS installs has swap on (zram-swap.service installed but disabled), and on that console
EmulationStation, which stays resident while a port runs, holds 342 MB (131 MB on the other RG351P). Not changed: enabling zram is the
user's choice. Disconnections: from the PC, pings to the two USB-Realtek consoles (r8188eu) reach 300-600 ms while the game runs,
against 13-15 ms to the others; the launcher now turns the Wi-Fi's power saving off while a LAN game runs (iw, and the Realtek
module's rtw_power_mgnt, both put back on exit; FO2_WIFI_AWAKE=0 leaves them), and the runtime logs every 5 s, per peer, the packets
received and the longest silence ([FO2 NET] lines). Pause in a LAN race: the game keeps running, so the port's pad mapping stays on
"race" and the confirm button sends accelerate: the quit confirmation cannot be validated. Needs the game's "menu open" flag.

**LAN with a PC (2026-11-17).** The Windows game (GOG / Zoom FlatOut2.exe, the same MD5 as the lifted one) hosted a session that
the four consoles joined. It did not work at first, with the firewall rule allowed: the PC had a Radmin VPN adapter with interface
metric 1 (Ethernet 25), so its LAN traffic and broadcasts went to the VPN; with Radmin VPN quit, PC and consoles see each other.
The second RG351P's [FO2 NET] lines then showed about 24 packets a second from the PC and from each console, longest silences
67-157 ms, no send error. The zram swap the user enabled on both RG351P let the first one race (it was OOM-killed before).

**Pause menu in a LAN race.** The user's clue: on "No", the confirm button moved to "Yes" and quit; on "Yes" it did nothing:
the button sent the race mapping's accelerate (Up). Found by memory snapshots: the scratchpad's `nanscan <pid> snap <va> <bytes>
<file>` (no stop) saved .data (0x681000, 0x282BE4 bytes) every 0.75 s of a single race whose automatic presses opened and closed
the pause twice (session menusnap.sh); 18 dwords hold one value whenever the menu is open and another whenever it is closed, and
switch exactly with it: 0x8DA4EC, 0x8DA4F8, 0x8DA514 (1 / 0), 0x8DA4F0 (0 / 0x10), 0x684B30 (0 / 0xCC), and in the menu layer
0x8DBE34 (1 / 0), alphas at 0x8DBE60... (0xFF / 0x00) and pointers. fo2InRace (the pad's race / menu choice) is now false while
0x8DBE34 or 0x8DA4EC is 1 (FO2_PAUSE_FLAG=0: physics only). Which of them a LAN pause sets is not known yet (user test).

**LAN finished (2026-11-17).** The user confirmed the pause menu in a LAN race (quit, Yes / No). LAN is now on by default
(fo2_imports.inc lanOn: FO2_LAN=0 turns it off), and the launcher keeps the Wi-Fi awake by default (FO2_WIFI_AWAKE=0 leaves it).
The LAN probes are out of work/gen/recomp_0015.c and the runtime; the crash report (nfsInstallCrashHandler) and the [FO2 NET]
statistics stay. The two pause flags, read four times a second through a whole automatic race: 0 throughout the driving, 0x8DA4EC
at 1 for 0.3 s at the race load (no effect). Build 94f546c7... on the four consoles, fo2.env test lines removed.

**Native gamepad (analog steering), investigation, 2026-11-17, not working yet.** With FO2_DIRECTINPUT_PAD=1 the game finds the
pad (offered as an Xbox 360 pad, 045E:028E, data format c_dfDIJoystick2: 272 bytes, 164 objects), the user can select it in
Options > Controls ("Android Gamepad": options.cfg Controller = 1, ControllerGuid = "444956444950000000000000028e045e"), and the
game polls it every frame: sub_00561610 (Poll, Acquire if needed, force feedback on/off, GetDeviceState into manager+0x2224 +
index * 0x110, previous state kept at +0x2460), called by sub_0055B490 for the two pad slots (present when [manager+0x4674+i*4]
and byte 0x8D8160+i is 0), which then copies the states (0x220 bytes) to manager+0. The analog values arrive (x from -10000 to
10000), yet nothing else reads them: hardware read watchpoints on the buttons (+0x30) and lX (+0) of both copies caught only
those copies, in the menus and in a race. With the host's keys off (FO2_PAD_KEYS=0) nothing answers, intro movies included.
Savegame/am<guid>.dat (16-byte entries: action, value, type, 0; header 0x00010002 and three settings) holds only keyboard
virtual-key codes (type 0); deleted, the game writes the same defaults back, and its remap screen changed nothing. The stick
no longer sends arrows in a race when the pad is offered (sdl_gamepad.h padOffered), so with the pad offered the stick does
nothing. Leads: sub_0055B4E0 (a switch on the controller type 0..9 calling 0x561A40 / 0x5619C0, device creation per type),
the second GetDeviceState site 0x5C3FEE (sub_005C3F90, a 0x110 / 0x114 state, another input system), and how Controller /
ControllerLayout select the reader. The RG34XX-SP is back to the keyboard setup (fo2.env removed, Controller = 0).

**Other releases of the game (after 2.0.0).** A user reported that "the GOG version does not work": the launcher refused
any FlatOut2.exe but the lifted one (MD5), and the runtime any file of another size. The retail 1.2 exe is the
same build as the Zoom one (link stamp 0x451D02BD, same IAT addresses): compared page by page, .rdata differs only in the
IAT thunks, the GameSpy -> OpenSpy host names, the debug directory and the original import tables, .data in 21 bytes
(host names); Zoom added one import, fo2_zoom.dll!ZOOM_Init, in an import table at the end of the image (nothing calls it).
So any copy of the 1.2 build can feed the lifted code. The runtime now (loadImage, runtime/native/fo2_exe_profile.h
generated by scripts/make_exe_profile.py: FNV-1a hashes of every 4 KB page but .reloc, and the import slots; no game
bytes) maps the file, compares its pages, and: the tested exe binds its own imports as before; another copy whose data
differs in at most 12 pages and code in at most 64 binds the imports from the profile (its own table can be missing or a
wrapper's) and shows a notice in the game for 25 s (d3d9SetNotice, FO2_NOTICE_SEC); anything else is refused with a
message (FO2_ANY_EXE=1 tries it). The code pages count: the lifted code reads .text at run time (switch tables): with
.text zeroed (an encrypted DRM wrapper) the game stops at once ("ITAIL unresolved 0"), with the .text of a protected retail copy (2,059
functions replaced by its protection) in the first race. Tests on the RG34XX-SP with variants built from the Zoom exe and
a retail 1.2 copy (scratchpad mksim.py): Zoom code + retail data and no import table (what a GOG / retail 1.2 exe should be): a whole
race at 58 FPS with the notice; protected code, zeroed code and scrambled data: refused. The launcher also finds the game in a
folder copied inside gamedata/ (moved up), any case of FlatOut2.exe, and d3dx9_30.dll in a subfolder, or tells where to
get it (C:\Windows\SysWOW64 of a Windows PC). Not tested with a real GOG or Steam exe (the user owns neither).
Then (the user asked to make refused copies work): the lifted code reads nothing of .text but the switch tables (347
"jmp [index*4 + table]" and 91 "movzx index, byte [value + table]" with a table in .text). scripts/make_text_tables.py
takes them from the lifted exe (code addresses and indices, 14 KB; bounds of index tables from the "cmp" before them,
dword tables from their entries that are cases of the switch or code addresses near the table, both directions: memcpy /
memmove index some from below, and the lifter's case list of memcpy's backward copy is wrong, so its entries go through
the dynamic dispatch) into runtime/native/fo2_text_tables.h, and loadImage writes them back when any code page differs.
The code pages no longer refuse a copy. Zeroed .text still stopped at start until the memcpy tables were complete: found
by zeroing page ranges of the real exe on the console (scratchpad bisect.sh: page 0x5FF000).

**Version and update check (2.2.0).** runtime/native/fo2_version.h (FO2_PORT_VERSION, to raise at each release) is shown
for 10 s (d3d9AddNotice: a list of notices with their colour and time) and printed by `fo2.aarch64 --version`. The launcher
fetches the latest release tag from api.github.com in the background while it prepares the game (curl or wget, 4 s at
most, FO2_UPDATE_CHECK=0 off) and passes a newer one as FO2_UPDATE_AVAILABLE (shown 15 s, in green). FO2_PORT_VERSION_TEST pretends another version (screenshots).

**R36S with dArkOSen (community fix).** Coreilabs (github.com/coreilabs/flatout2-fix-r36s-darkosEN, MIT) got the port out
of a black screen on an R36S with dArkOSen by changing the old two launchers: SDL KMSDRM, ALSA (SDL and OpenAL), explicit
libGLESv2.so / libEGL.so, 640x480, video decoding 3 threads + fast, no CPU boost, stdin from /dev/tty1, the vtconsole
bindings released while the game runs (restored on exit), sudo / tty1 fallbacks. Not isolated which change matters. Now a
profile of the single launcher, chosen when the device tree's model or compatible string, or PortMaster's DEVICE_NAME,
says R36S (the upstream kernel names it gameconsole,r36s); FO2_KMS_VT=0 keeps the console bound. Untested here (no R36S).
The sudo / tty1 fallbacks apply to every console without control.txt. The RG351 profile renders 640x480 on the RG351MP / V.

**Native gamepad: solved, default since 2.0.0.** The game reads the pad only through the bindings of am<guid>.dat, and an
entry can be an axis: type 1, value = axis index (0..7: X, Y, Z, RX, RY, RZ, U, V), extra = rest (low int16) and fully
pushed (high int16). Steering left (action 12) = axis 0 from 0 to -10000, right (13) = axis 0 from 0 to +10000; every other
action keeps its keyboard key (type 0), so the host still sends keys for the buttons. That file is port/pad/; the launcher
installs it once (marker Savegame/.pad_bindings_v1, the previous file kept as .before-2.0; a later remap is kept) and sets
options.cfg Controller = 1 with the pad's GUID at every launch. The pad is offered by default (FO2_DIRECTINPUT_PAD=0: the
keyboard of 1.x, Controller = 0). Host side (sdl_gamepad.h): in a race neither the stick nor the d-pad sends arrows; the
d-pad drives axis X when the stick rests (consoles without a stick: full lock, as before). Stick dead zone rescaled (no jump):
FO2_STICK_DEADZONE percent, 15 by default, full output from 95 % (cheap sticks rest off-centre and rarely reach the edge). The
game's own ControllerDeadzone (0), ControllerSaturation (90) and ControllerSensitivity (90) apply after it. User test on the
RG34XX-SP: "C'est progressif". The release zip (made by hand for 1.1.0) must now hold flatout2/pad/.

**The d3dx9_30.dll filter.** Lifting all 17,876 catalogued functions gave 5 GB of C: four sources Clang
refused ("ran out of source locations"), nine with branches to missing labels. Most of it is data decoded as
code: Microsoft merges `.rdata` into `.text`, and the HLSL compiler's string tables ("texCUBE", "tex3Dproj"...)
made thousands of overlapping pseudo-functions. `reachable_filter.py` keeps what the exe can reach: roots are
DllMain, the 3 imported exports and every relocated pointer to a plausible function start (prepare_module's
prologue rule); edges are direct calls and branches, relocated pointers inside kept code (and the vtable runs
they point to) and switch cases. 4,872 functions, 57 MB of C, 2,463 unimplemented sites.

By then: CRT, archives, FMOD, DirectInput and the D3D9 device (720x480 on the Mali, GLES 3.2 context) are up,
and the game has written `Savegame/options.cfg` and `device.cfg`.
