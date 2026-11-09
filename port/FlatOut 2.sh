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

if [ ! -f gamedata/FlatOut2.exe ] && [ ! -f gamedata/flatout2.exe ]; then
  echo "Game files not found: copy the PC game into $GAMEDIR/gamedata/"
  [ -n "$CUR_TTY" ] && printf "\033c\nFlatOut 2: copy the PC game files into\n%s/gamedata/\n" "$GAMEDIR" > "$CUR_TTY"
  sleep 5
  exit 1
fi
# The port is built from one exact executable: another one cannot run.
EXE=gamedata/FlatOut2.exe; [ -f "$EXE" ] || EXE=gamedata/flatout2.exe
if [ "$(md5sum "$EXE" | cut -d' ' -f1)" != "1f8cae2b47276ca134750ba438aa35ca" ]; then
  echo "Unsupported FlatOut2.exe: this port needs the DRM-free 1.2 executable (see README)."
  [ -n "$CUR_TTY" ] && printf "\033c\nFlatOut 2: this FlatOut2.exe is not supported.\nSee the README for the right version.\n" > "$CUR_TTY"
  sleep 6
  exit 1
fi
mkdir -p gamedata/Savegame

# Per console: video driver, render size and frame rate limit.
case "$(grep -m1 '^Hardware' /proc/cpuinfo 2>/dev/null; cat /proc/device-tree/model 2>/dev/null)" in
  *Pixel2*) DEFAULT_DRIVER=wayland; DEFAULT_RENDER=640x480; DEFAULT_CAP=30   # ROCKNIX under Sway, GKD Pixel II (Cortex-A35, Mali-G31)
           export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/0-runtime-dir}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
           export FO2_SWAP_ABXY="${FO2_SWAP_ABXY:-1}"                       # its mapping has A/B and X/Y the other way round
           export FO2_VIDEO_THREADS="${FO2_VIDEO_THREADS:-3}" FO2_VIDEO_FAST="${FO2_VIDEO_FAST:-1}" ;;
  *RG351*) DEFAULT_DRIVER=KMSDRM; DEFAULT_RENDER=480x320; DEFAULT_CAP=30   # dArkOS, RK3326, 480x320 panel mounted rotated
           export FO2_SWAP_ABXY="${FO2_SWAP_ABXY:-1}"                       # its mapping has A/B and X/Y the other way round
           export FO2_VIDEO_THREADS="${FO2_VIDEO_THREADS:-3}" FO2_VIDEO_FAST="${FO2_VIDEO_FAST:-1}" ;;   # lighter video decoding
  *RG34XX*) DEFAULT_DRIVER=mali;  DEFAULT_RENDER=648x432; DEFAULT_CAP=60 ;;   # Knulli, RG34XX-SP
  *)  # A console nobody has tested: SDL picks the video driver (mali when there is no DRM device), the render size
      # follows the screen with at most 480 lines, the frame rate limit is the prudent one.
      DEFAULT_DRIVER=""; [ -e /dev/dri/card0 ] || DEFAULT_DRIVER=mali
      SW=${DISPLAY_WIDTH:-640}; SH=${DISPLAY_HEIGHT:-480}
      [ "$SW" -lt "$SH" ] && { T=$SW; SW=$SH; SH=$T; }
      if [ "$SH" -gt 480 ]; then SW=$(( SW * 480 / SH )); SH=480; fi
      DEFAULT_RENDER="$(( SW / 2 * 2 ))x$(( SH / 2 * 2 ))"; DEFAULT_CAP=30
      export FO2_VIDEO_THREADS="${FO2_VIDEO_THREADS:-3}" FO2_VIDEO_FAST="${FO2_VIDEO_FAST:-1}" ;;
esac
# Two launchers ship with the port and differ only by their name: "... 30 FPS" and "... FPS Unlock".
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

cleanup() {
  kill "$WATCH_PID" 2>/dev/null
  for entry in $CLOCK_STATE; do wsys "${entry%%=*}" "${entry#*=}"; done
  sync
}
trap cleanup EXIT

chmod +x ./fo2.aarch64
# FO2_CAP (fo2.env): the frame rate limit. 60 lets frames alternate between one and two panel refreshes when the game
# cannot hold 60, felt as stutter; 30 gives even frames.
./fo2.aarch64 --cap="${FO2_CAP:-$DEFAULT_CAP}"
echo "exit code $?"