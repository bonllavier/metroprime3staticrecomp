#!/usr/bin/env bash
# Linux counterpart of recomp/qa/qa_replay.ps1: replays a recorded QA session unattended on a private
# profile (the recording's save and settings; the shader caches shared with the build's profile through a
# symlink) and prints the per-window frame rate. Needs a QA build (build_prime_runtime.sh --qa).
#   tools/linux_diag/qa_replay.sh NAME [LABEL] [VAR=value | cfg:section.key=value ...]
# cfg: arguments override the recording's Config.toml (e.g. cfg:video.frame_interpolation_fps=120).
# Recordings: $QA_RECORDINGS (default: the Windows tree's recomp/qa/recordings, private, never published).
set -euo pipefail
script_dir=$(dirname "$(readlink -f "$0")")
name=${1:?recording name}; label=${2:-run}; shift $(( $# >= 2 ? 2 : 1 ))
build=${MP3_BUILD_DIR:-/mnt/mp3port/builds/RM3E01_00_runtime_linux}
iso=${MP3_ISO:-"/mnt/mp3port/iso/Metroid Prime 3 - Corruption (USA).rvz"}
recordings=${QA_RECORDINGS:-/media/javb92/jav_extra_one/gamecube_roms/metroid3_wii/recomp/qa/recordings}
rec=$recordings/$name
[[ -f $rec/inputs.txt ]] || { echo "no recording $rec" >&2; exit 1; }
# The game renames its main thread, so match the executable path rather than the process name.
if pgrep -f '(^|/)Corruption_3 PC --' >/dev/null; then echo "close the game first (the replay shares the shader cache)" >&2; exit 1; fi
qa_root=${QA_ROOT:-/mnt/mp3port/builds/qa}
profile=$qa_root/profile/UserData
rm -rf "$qa_root/profile"
mkdir -p "$profile"
cp -r "$rec/NAND" "$profile/NAND"
# The recording's settings, without its Windows disc path (the image is passed with --iso).
grep -v '^\s*iso_path\s*=' "$rec/Config.toml" > "$profile/Config.toml"
ln -s "$build/UserData/Cache" "$profile/Cache"
run_dir=$qa_root/runs/$name/$(date +%Y%m%d_%H%M%S)_$label
mkdir -p "$run_dir"
export WII_COMPILED_QA_USER_DATA=$profile WII_COMPILED_QA_REPLAY=$rec/inputs.txt WII_COMPILED_QA_REPLAY_EXIT=10 \
       WII_COMPILED_QA_TAP_INTRO=1 WII_COMPILED_PRIME_KEYBOARD_MOUSE=1 WII_COMPILED_PRIME_PERFORMANCE_TRACE=1
for pair in "$@"; do
    if [[ $pair == cfg:* ]]; then python3 -I "$script_dir/set_config.py" "$profile/Config.toml" "${pair#cfg:}"
    else export "$pair"; fi
done
cd "$build"
./"Corruption_3 PC" --iso "$iso" > "$run_dir/stdout.log" 2>&1 &
pid=$!
# Same memory guard as the other heavy tests.
"$script_dir/watchdog.sh" "$pid" "$run_dir/memory.log" &
timeout_s=$(( ${QA_TIMEOUT_MINUTES:-30} * 60 )); waited=0
while kill -0 "$pid" 2>/dev/null && (( waited < timeout_s )); do sleep 2; waited=$((waited + 2)); done
kill -0 "$pid" 2>/dev/null && { kill "$pid"; echo "timed out" >&2; }
sleep 2
log=$(ls -td "$profile/Logs/"*"pid$pid" 2>/dev/null | head -1)/console.log
cp "$log" "$run_dir/console.log" 2>/dev/null || true
fps=$(grep -oE '\[prime-performance\] presents=[0-9]+ window_samples=[0-9]+ fps=[0-9.]+' "$run_dir/console.log" | sed 's/.*fps=//' | tail -n +2 | tr '\n' ' ')
mean=$(echo "$fps" | awk '{s=0; for(i=1;i<=NF;i++) s+=$i; if (NF) printf "%.2f", s/NF; else print 0}')
low=$(echo "$fps" | awk '{n=0; for(i=1;i<=NF;i++) if ($i<57) n++; print n "/" NF}')
finished=$(grep -c '\[qa-replay\] finished' "$run_dir/console.log" || true)
echo "recording=$name label=$label finished=$finished mean_fps=$mean windows_below_57=$low"
echo "fps: $fps"
# Images actually presented (frame interpolation adds inserted ones): present-latency lines, every 10 s.
presented=$(grep -oE '\[present-latency\] frames=[0-9]+' "$run_dir/console.log" | sed 's/.*frames=//' | tail -n +2 | awk '{printf "%.0f ", $1/10}')
echo "presented_per_s: $presented"
echo "run: $run_dir"
