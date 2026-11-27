#!/usr/bin/env bash
# Plays the game by hand with one graphics configuration on a throwaway profile, for A/B of a scene the
# replays cannot reach (enemies differ between runs). The saved game and Config.toml are copied from the
# build's UserData at launch, the overrides applied, the shader caches shared; the performance trace is on.
# When the game closes, its console.log is kept in $MANUAL_ROOT/<label>_<time>/. Needs a QA build.
#   tools/linux_diag/play_variant.sh LABEL [section.key=value | VAR=value ...]
#   e.g. play_variant.sh no_contact video.contact_shadows=false
# section.key=value changes Config.toml; VAR=value (no dot in VAR) is an environment variable for the game.
# Also kept: nvidia-smi samples every 0.5 s (gpu.csv) and one F8 profile, taken by itself as soon as the
# presented rate falls below $PROFILE_BELOW_FPS (default 30) after the first minute.
# Saves made while playing a variant stay in the throwaway profile and are discarded.
# MP3_EXE picks another executable of the build directory (default "Corruption_3 PC"), e.g. a test copy
# "Corruption_3 PC.c14elide3"; it runs from the build directory, next to the real one.
set -euo pipefail
script_dir=$(dirname "$(readlink -f "$0")")
label=${1:?label}; shift
[[ $label =~ ^[A-Za-z0-9_-]+$ ]] || { echo 'LABEL: letters, digits, _ and - only' >&2; exit 2; }
build=${MP3_BUILD_DIR:-/mnt/mp3port/builds/RM3E01_00_runtime_linux}
iso=${MP3_ISO:-"/mnt/mp3port/iso/Metroid Prime 3 - Corruption (USA).rvz"}
root=${MANUAL_ROOT:-/mnt/mp3port/builds/qa/manual}
exe=${MP3_EXE:-Corruption_3 PC}
[[ -f $build/$exe ]] || { echo "no executable '$exe' in $build" >&2; exit 1; }
if pgrep -f '(^|/)Corruption_3 PC[^ ]* --' >/dev/null; then echo "close the game first (the variant shares the shader cache)" >&2; exit 1; fi
grep -aq MKW_PRIME_QA_TOOLS_ENABLED_BUILD "$build/$exe" ||
    { echo "$build/$exe was built without MKW_PRIME_QA_TOOLS" >&2; exit 1; }
profile=$root/profile/UserData
rm -rf "$root/profile"
mkdir -p "$profile"
cp -r "$build/UserData/NAND" "$profile/NAND"
grep -v '^\s*iso_path\s*=' "$build/UserData/Config.toml" > "$profile/Config.toml"
envs=()
for setting in "$@"; do
    name=${setting%%=*}
    if [[ $name == *.* ]]; then python3 -I "$script_dir/set_config.py" "$profile/Config.toml" "$setting"
    else envs+=("$setting"); fi
done
ln -s "$build/UserData/Cache" "$profile/Cache"
out=$root/${label}_$(date +%m%d_%H%M%S)
mkdir -p "$out"
printf '%s\n' "$@" > "$out/overrides.txt"
echo "$exe" > "$out/exe_name.txt"
cp "$profile/Config.toml" "$out/Config.toml"
gpu_logger=
if command -v nvidia-smi >/dev/null; then
    nvidia-smi --query-gpu=timestamp,utilization.gpu,clocks.gr,power.draw,pstate --format=csv -lms 500 > "$out/gpu.csv" 2>/dev/null &
    gpu_logger=$!
fi
cd "$build"
env WII_COMPILED_QA_USER_DATA="$profile" WII_COMPILED_PRIME_KEYBOARD_MOUSE=1 WII_COMPILED_PRIME_PERFORMANCE_TRACE=1 \
    WII_COMPILED_STACK_PROFILE_AT=60 WII_COMPILED_STACK_PROFILE_BELOW_FPS="${PROFILE_BELOW_FPS:-30}" "${envs[@]}" \
    ./"$exe" --iso "$iso" > "$out/stdout.log" 2>&1 || true
if [[ -n $gpu_logger ]]; then kill "$gpu_logger" 2>/dev/null || true; fi
# The session's folder (base_<epoch>_pid<PID>), not profiles/ (an F8 capture makes that the newest).
log=$(ls -td "$profile/Logs/"base_*/ 2>/dev/null | head -1 || true)
if [[ -n $log ]]; then cp "$log/console.log" "$out/console.log" || true; fi
cp "$profile/Logs/profiles/"* "$out/" 2>/dev/null || true
echo "kept: $out"
