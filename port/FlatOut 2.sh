#!/bin/bash
# PortMaster launch script: FlatOut 2 (PC), statically recompiled to ARM64.
# Made for the Anbernic RG34XX-SP (Allwinner H700, Mali-G31, 720x480) on Knulli.
#
# Game files: copy from the PC game folder into ports/flatout2/gamedata/:
#   FlatOut2.exe, d3dx9_30.dll, filesystem, data.bfs, langpack.bfs (and zoom_platform.bfs if present)

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi
if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
  get_controls
fi
# Without PortMaster's control.txt (or with one that leaves them empty): sudo for the sysfs writes when not root,
# and the first console for messages.
[ -z "$ESUDO" ] && [ "$(id -u)" != "0" ] && command -v sudo > /dev/null 2>&1 && ESUDO=sudo
[ -z "$CUR_TTY" ] && [ -w /dev/tty1 ] && CUR_TTY=/dev/tty1

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
if [ -n "$directory" ] && [ -d "/$directory/ports/flatout2" ]; then
  GAMEDIR="/$directory/ports/flatout2"
else
  GAMEDIR="$SCRIPT_DIR/flatout2"
fi
cd "$GAMEDIR" || exit 1

# One log per run (the previous one is kept), flushed to the card every 10 s with a memory reading,
# so a run that hangs the console still leaves its last seconds behind.
[ -f log.txt ] && mv -f log.txt log.prev.txt
[ -f mem.txt ] && mv -f mem.txt mem.prev.txt
exec > >(tee "$GAMEDIR/log.txt") 2>&1
echo "FlatOut 2 (recompiled), $(date)"
uname -a

# The game's files, from any PC release: FlatOut2.exe in any case, at the top of gamedata/ or in a folder copied
# inside it (the game's own folder, or a GOG installer's "app"), in which case that folder's content is moved up.
findexe() { find "$1" -maxdepth "$2" -type f -iname flatout2.exe 2>/dev/null | head -1; }
EXE=$(findexe gamedata 1)
if [ -z "$EXE" ]; then
  EXE=$(findexe gamedata 4)
  if [ -n "$EXE" ]; then
    SRC=$(dirname "$EXE")
    echo "Game files found in $SRC: moved up into gamedata/"
    ( cd "$SRC" && for f in * .[!.]*; do [ -e "$f" ] || continue; [ -e "$GAMEDIR/gamedata/$f" ] && continue; mv -f "$f" "$GAMEDIR/gamedata/" 2>/dev/null; done )
    EXE=$(findexe gamedata 1)
  fi
fi
if [ -z "$EXE" ]; then
  echo "Game files not found: copy the PC game into $GAMEDIR/gamedata/"
  [ -n "$CUR_TTY" ] && printf "\033c\nFlatOut 2: copy the PC game files into\n%s/gamedata/\n" "$GAMEDIR" > "$CUR_TTY"
  sleep 5
  exit 1
fi
# Tested with the GOG / Zoom exe (v2.1.0.9). Other copies of the same build (GOG, Steam, retail patch 1.2) run too: the
# runtime compares their data with the tested exe's, shows a notice in the game, and refuses only another build.
EXE_MD5=$(md5sum "$EXE" | cut -d' ' -f1)
if [ "$EXE_MD5" = "1f8cae2b47276ca134750ba438aa35ca" ]; then
  echo "FlatOut2.exe: the tested version (GOG / Zoom v2.1.0.9)"
else
  echo "FlatOut2.exe: MD5 $EXE_MD5, $(stat -c %s "$EXE") bytes: not the tested version (1f8cae2b47276ca134750ba438aa35ca), trying it"
fi
# d3dx9_30.dll (DirectX 9, June 2006) is needed: some releases install it with DirectX instead of in the game's folder.
if [ -z "$(find gamedata -maxdepth 1 -type f -iname d3dx9_30.dll | head -1)" ]; then
  D3DX=$(find gamedata -maxdepth 4 -type f -iname d3dx9_30.dll 2>/dev/null | head -1)
  if [ -n "$D3DX" ]; then
    cp -f "$D3DX" gamedata/d3dx9_30.dll
  else
    echo "d3dx9_30.dll not found: copy it from a Windows PC (C:\\Windows\\SysWOW64\\d3dx9_30.dll) into $GAMEDIR/gamedata/"
    [ -n "$CUR_TTY" ] && printf "\033c\nFlatOut 2: d3dx9_30.dll is missing.\nCopy it from a Windows PC\n(C:\\\\Windows\\\\SysWOW64\\\\d3dx9_30.dll)\ninto %s/gamedata/\n" "$GAMEDIR" > "$CUR_TTY"
    sleep 8
    exit 1
  fi
fi
mkdir -p gamedata/Savegame

# Per console: video driver, render size and frame rate limit.
# The console: /proc/cpuinfo, the device tree's model and compatible strings, and PortMaster's own detection.
MODEL="$(grep -m1 '^Hardware' /proc/cpuinfo 2>/dev/null; tr '\000' ' ' < /proc/device-tree/model 2>/dev/null; tr '\000' ' ' < /proc/device-tree/compatible 2>/dev/null) ${DEVICE_NAME:-}"
echo "Console: $MODEL"
KMS_VT=0
case "$MODEL" in
  *R36S*|*r36s*)   # RK3326, 640x480. Community fix by Coreilabs (github.com/coreilabs/flatout2-fix-r36s-darkosEN, MIT),
           # tested on an R36S with dArkOSen, where the game otherwise stayed on a black screen.
           DEFAULT_DRIVER=KMSDRM; DEFAULT_RENDER=640x480; DEFAULT_CAP=60; KMS_VT=1
           export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}" ALSOFT_DRIVERS="${ALSOFT_DRIVERS:-alsa}"
           export SDL_VIDEO_GL_DRIVER="${SDL_VIDEO_GL_DRIVER:-libGLESv2.so}" SDL_VIDEO_EGL_DRIVER="${SDL_VIDEO_EGL_DRIVER:-libEGL.so}"
           export FO2_CPU_BOOST="${FO2_CPU_BOOST:-0}"                       # as tested: the system's CPU maximum
           export FO2_VIDEO_THREADS="${FO2_VIDEO_THREADS:-3}" FO2_VIDEO_FAST="${FO2_VIDEO_FAST:-1}" ;;
  *Pixel2*) DEFAULT_DRIVER=wayland; DEFAULT_RENDER=640x480; DEFAULT_CAP=60   # ROCKNIX under Sway, GKD Pixel II (Cortex-A35, Mali-G31)
           export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/0-runtime-dir}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
           export FO2_SWAP_ABXY="${FO2_SWAP_ABXY:-1}"                       # its mapping has A/B and X/Y the other way round
           export FO2_VIDEO_THREADS="${FO2_VIDEO_THREADS:-3}" FO2_VIDEO_FAST="${FO2_VIDEO_FAST:-1}" ;;
  *RG351*) DEFAULT_DRIVER=KMSDRM; DEFAULT_RENDER=480x320; DEFAULT_CAP=60   # dArkOS, RK3326, 480x320 panel mounted rotated
           case "$MODEL" in *RG351MP*|*RG351V*) DEFAULT_RENDER=640x480 ;; esac   # their panel is 640x480
           export FO2_SWAP_ABXY="${FO2_SWAP_ABXY:-1}"                       # its mapping has A/B and X/Y the other way round
           export FO2_VIDEO_THREADS="${FO2_VIDEO_THREADS:-3}" FO2_VIDEO_FAST="${FO2_VIDEO_FAST:-1}" ;;   # lighter video decoding
  *RG34XX*) DEFAULT_DRIVER=mali;  DEFAULT_RENDER=648x432; DEFAULT_CAP=60 ;;   # Knulli, RG34XX-SP
  *)  # A console nobody has tested: SDL picks the video driver (mali when there is no DRM device), the render size
      # follows the screen with at most 480 lines, the frame rate limit is the prudent one.
      DEFAULT_DRIVER=""; [ -e /dev/dri/card0 ] || DEFAULT_DRIVER=mali
      SW=${DISPLAY_WIDTH:-640}; SH=${DISPLAY_HEIGHT:-480}
      [ "$SW" -lt "$SH" ] && { T=$SW; SW=$SH; SH=$T; }
      if [ "$SH" -gt 480 ]; then SW=$(( SW * 480 / SH )); SH=480; fi
      DEFAULT_RENDER="$(( SW / 2 * 2 ))x$(( SH / 2 * 2 ))"; DEFAULT_CAP=60
      export FO2_VIDEO_THREADS="${FO2_VIDEO_THREADS:-3}" FO2_VIDEO_FAST="${FO2_VIDEO_FAST:-1}" ;;
esac
# One launcher since v1.1.0, up to 60 FPS everywhere (FO2_CAP=30 in fo2.env for a steady 30). A copy named "... 30 FPS" still caps at 30.
case "$(basename "$0")" in
  *Unlock*)   DEFAULT_CAP=60 ;;
  *"30 FPS"*) DEFAULT_CAP=30 ;;
esac
# Sysfs writes: the launcher is not always root (dArkOS runs it as ark, PortMaster's control.txt sets ESUDO).
wsys() { echo "$2" | $ESUDO tee "$1" > /dev/null 2>&1; }

[ -n "${SDL_VIDEODRIVER:-$DEFAULT_DRIVER}" ] && export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-$DEFAULT_DRIVER}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run}"   # PipeWire's socket on Knulli
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export NFSU2_SHOW_FPS="${FO2_SHOW_FPS:-0}"              # FPS counter, hidden at launch (L2 + R2 or Select + R3 toggles it)
export FO2_RENDER="${FO2_RENDER:-$DEFAULT_RENDER}"     # RG34XX-SP: 3:2 at 90%, scaled to the full screen; 720x480 is native but costs more
                                                # (576x384 gains about 5% in the heaviest scenes)
# fo2.env next to the binary: optional VAR=value lines (# comments), exported before the launch, to try the
# runtime's switches without editing this script (e.g. FO2_GPU_STRETCH=0, FO2_CPUID_EDX_OFF=800000).
if [ -f fo2.env ]; then
  while IFS= read -r line || [ -n "$line" ]; do
    line="${line%%#*}"; line="$(echo "$line" | tr -d '\r' | sed 's/^ *//;s/ *$//')"
    case "$line" in *=*) export "$line";; esac
  done < fo2.env
fi

# Savegame/device.cfg keeps the display mode (width, height, bits at bytes 120/124/128). At boot the game
# looks for it among the adapter's modes; the runtime offers only FO2_RENDER, so the 1280x1024x32 its
# auto-configuration stores never matched, and the auto-configuration (highest details, 0x450AA0) ran again
# at every launch. The mode is set to the render size before each launch.
DEVICE="$GAMEDIR/gamedata/Savegame/device.cfg"
putdw() {  # little-endian dword $2 at byte $1 of device.cfg
  printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(($2 & 255)) $((($2 >> 8) & 255)) $((($2 >> 16) & 255)) $((($2 >> 24) & 255)))" \
    | dd of="$DEVICE" bs=1 seek="$1" conv=notrunc status=none
}
RW=${FO2_RENDER%x*}; RH=${FO2_RENDER#*x}
if [ -f "$DEVICE" ] && [ "$(stat -c %s "$DEVICE")" -ge 132 ] && [ "$RW" -gt 0 ] 2>/dev/null && [ "$RH" -gt 0 ] 2>/dev/null; then
  putdw 120 "$RW"; putdw 124 "$RH"; putdw 128 32
fi

# Lowest visual settings at every launch (the handheld needs them all): textures, track detail and
# view distance at their minimum, no sun flare, no motion blur. Alpha blending stays (foliage, glass).
# FO2_KEEP_SETTINGS=1 keeps whatever the game's options menu saved.
OPTIONS="$GAMEDIR/gamedata/Savegame/options.cfg"
if [ "${FO2_KEEP_SETTINGS:-0}" != "1" ] && [ -f "$OPTIONS" ]; then
  sed -i -e 's/^\(Settings\.Visual\.TextureQuality = \)[0-9]*/\10/' \
         -e 's/^\(Settings\.Visual\.TrackDetail = \)[0-9]*/\10/' \
         -e 's/^\(Settings\.Visual\.Visibility = \)[0-9]*/\10/' \
         -e 's/^\(Settings\.Visual\.PS2MotionBlurIngame = \)[0-9]*/\10/' \
         -e 's/^\(Settings\.Visual\.SunFlare = \)[A-Z]*/\1FALSE/' "$OPTIONS"
fi

# Native gamepad (since 2.0.0): the console is offered to the game as an Xbox 360 pad and the game is set to use it,
# with the steering on the stick's X axis (analog; the d-pad drives that axis on consoles without a stick). The pad's
# bindings (pad/am<guid>.dat) are installed once: a remap made later in the game's Controls menu is kept.
# FO2_DIRECTINPUT_PAD=0 goes back to the keyboard of 1.x (digital steering).
PAD_GUID=444956444950000000000000028e045e
if [ "${FO2_DIRECTINPUT_PAD:-1}" != "0" ] && [ -f "$OPTIONS" ]; then
  sed -i -e 's/^\(Settings\.Control\.Controller *= *\)[0-9]*/\11/' \
         -e "s/^\(Settings\.Control\.ControllerGuid *= *\).*/\1\"$PAD_GUID\"/" "$OPTIONS"
  BINDINGS="$GAMEDIR/gamedata/Savegame/am$PAD_GUID.dat"
  if [ -f "$GAMEDIR/pad/am$PAD_GUID.dat" ] && [ ! -f "$GAMEDIR/gamedata/Savegame/.pad_bindings_v1" ]; then
    [ -f "$BINDINGS" ] && cp -f "$BINDINGS" "$BINDINGS.before-2.0"
    cp -f "$GAMEDIR/pad/am$PAD_GUID.dat" "$BINDINGS" && touch "$GAMEDIR/gamedata/Savegame/.pad_bindings_v1"
  fi
elif [ -f "$OPTIONS" ]; then
  sed -i 's/^\(Settings\.Control\.Controller *= *\)[0-9]*/\10/' "$OPTIONS"
fi

# GPU at its top clock while the game runs (restored on exit). The RG34XX-SP's GPU offers only
# simple_ondemand (no "performance" governor): its minimum frequency is raised to the highest one instead.
CLOCK_STATE=""
for dev in /sys/class/devfreq/*; do
  gov="$dev/governor"; [ -f "$gov" ] || continue
  old=$(cat "$gov" 2>/dev/null)
  if grep -qw performance "$dev/available_governors" 2>/dev/null && wsys "$gov" performance; then
    CLOCK_STATE="$CLOCK_STATE $gov=$old"
  elif [ -f "$dev/min_freq" ] && [ -f "$dev/available_frequencies" ]; then
    top=$(tr ' ' '\n' < "$dev/available_frequencies" | sort -n | tail -1)
    oldmin=$(cat "$dev/min_freq")
    [ -n "$top" ] && wsys "$dev/min_freq" "$top" && CLOCK_STATE="$CLOCK_STATE $dev/min_freq=$oldmin"
  fi
done
# CPU at its top frequency (Knulli caps the RG34XX-SP at 1.512 GHz of its 1.704: the "overclock" gate),
# with the performance governor, while the game runs; restored on exit. FO2_CPU_BOOST=0 leaves the CPU alone.
if [ "${FO2_CPU_BOOST:-1}" != "0" ]; then
  for pol in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -f "$pol/scaling_max_freq" ] || continue
    hw=$(cat "$pol/cpuinfo_max_freq")
    CLOCK_STATE="$CLOCK_STATE $pol/scaling_max_freq=$(cat "$pol/scaling_max_freq") $pol/scaling_governor=$(cat "$pol/scaling_governor")"
    wsys "$pol/scaling_max_freq" "$hw"
    wsys "$pol/scaling_governor" performance
  done
fi

(
  while sleep 10; do
    { date +%T; free -m | sed -n 2,3p; } >> "$GAMEDIR/mem.txt"
    sync
  done
) &
WATCH_PID=$!

VT_STATE=""
cleanup() {
  kill "$WATCH_PID" 2>/dev/null
  for entry in $CLOCK_STATE; do wsys "${entry%%=*}" "${entry#*=}"; done
  for entry in $VT_STATE; do wsys "${entry%%=*}" "${entry#*=}"; done
  [ "$KMS_VT" = "1" ] && [ -w "$CUR_TTY" ] && printf '\033c\033[?25h' > "$CUR_TTY"
  for ifc in $WIFI_STATE; do $ESUDO iw dev "$ifc" set power_save on 2>/dev/null; done
  sync
}
# LAN (on unless FO2_LAN=0): the Wi-Fi's power saving delays packets by up to a few hundred milliseconds (measured 300-600 ms on
# the RG351P and GKD Pixel II's USB adapters while the game ran), long enough for the game to drop a player. It is
# turned off while the game runs and put back on exit. FO2_WIFI_AWAKE=0 leaves it alone.
WIFI_STATE=""
if [ "${FO2_LAN:-1}" != "0" ] && [ "${FO2_WIFI_AWAKE:-1}" != "0" ]; then
  for dev in /sys/class/net/wlan*; do
    [ -e "$dev" ] || continue; ifc=${dev##*/}
    if command -v iw > /dev/null 2>&1; then
      ps=$(iw dev "$ifc" get power_save 2>/dev/null | awk '{print $NF}')
      [ "$ps" = "on" ] && $ESUDO iw dev "$ifc" set power_save off 2>/dev/null && WIFI_STATE="$WIFI_STATE $ifc"
    fi
  done
  for prm in /sys/module/*/parameters/rtw_power_mgnt; do   # Realtek out-of-tree drivers (8188eu, 8821cs): read at the next connection
    [ -e "$prm" ] || continue; old=$(cat "$prm")
    [ "$old" != "0" ] && wsys "$prm" 0 && CLOCK_STATE="$CLOCK_STATE $prm=$old"
  done
  echo "LAN: Wi-Fi power saving off on:${WIFI_STATE:- (iw not available)}"
fi
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# R36S profile (Coreilabs' fix): the text console released for KMSDRM while the game runs (put back on exit), and the
# game's input read from the first console.
if [ "$KMS_VT" = "1" ] && [ "${FO2_KMS_VT:-1}" != "0" ]; then
  [ -w "$CUR_TTY" ] && printf '\033c\033[?25l' > "$CUR_TTY"
  for bind in /sys/class/vtconsole/vtcon*/bind; do
    [ -r "$bind" ] || continue
    oldbind=$(cat "$bind")
    wsys "$bind" 0 && VT_STATE="$VT_STATE $bind=$oldbind"
  done
  [ -r "$CUR_TTY" ] && exec < "$CUR_TTY"
fi

chmod +x ./fo2.aarch64
# FO2_CAP (fo2.env): the frame rate limit. 60 lets frames alternate between one and two panel refreshes when the game
# cannot hold 60, felt as stutter; 30 gives even frames.
./fo2.aarch64 --cap="${FO2_CAP:-$DEFAULT_CAP}"
echo "exit code $?"
if grep -q "unsupported FlatOut2.exe" "$GAMEDIR/log.txt" 2>/dev/null; then
  [ -n "$CUR_TTY" ] && printf "\033c\nFlatOut 2: this FlatOut2.exe is another build\nthan the one the port was made from.\nPlease open an issue on GitHub with\nports/flatout2/log.txt:\ngithub.com/badcats72/flatout2-portmaster\n" > "$CUR_TTY"
  sleep 10
fi