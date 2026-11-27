#!/usr/bin/env bash
# Linux counterpart of recomp/qa/qa_record.ps1: records a play session for automated replays
# (recomp/qa/README.md). The game keeps running between recordings; start and stop are messages
# to the running game through WII_COMPILED_QA_CONTROL_DIR. Needs a QA build (build_prime_runtime.sh --qa).
#   tools/linux_diag/qa_record.sh --launch            start the game ready to record (nothing recorded yet)
#   tools/linux_diag/qa_record.sh NAME [NOTES]        start recording (at the title or the menus)
#   tools/linux_diag/qa_record.sh --stop              stop the current recording; the game keeps running
#   tools/linux_diag/qa_record.sh --status            what the game reports (idle / recording / stopped / refused)
# The build and its UserData (the saved game a replay starts from): $MP3_BUILD_DIR.
# Recordings: $QA_RECORDINGS (default: the Windows tree's recomp/qa/recordings, the folder qa_replay.sh reads).
set -euo pipefail
build=${MP3_BUILD_DIR:-/mnt/mp3port/builds/RM3E01_00_runtime_linux}
iso=${MP3_ISO:-"/mnt/mp3port/iso/Metroid Prime 3 - Corruption (USA).rvz"}
recordings=${QA_RECORDINGS:-/media/javb92/jav_extra_one/gamecube_roms/metroid3_wii/recomp/qa/recordings}
control=${QA_CONTROL_DIR:-/mnt/mp3port/builds/qa/record_control}
exe="$build/Corruption_3 PC"
mkdir -p "$control"
running() { pgrep -f '(^|/)Corruption_3 PC --' >/dev/null; }

launch() {
    grep -aq MKW_PRIME_QA_TOOLS_ENABLED_BUILD "$exe" ||
        { echo "$exe was built without MKW_PRIME_QA_TOOLS (build_prime_runtime.sh --qa)" >&2; exit 1; }
    rm -f "$control"/*.txt
    cd "$build"
    # The same A taps through the strap screen and the intro videos as the replays (qa_replay.sh), so
    # the front end is reached on the same frame in both.
    WII_COMPILED_QA_CONTROL_DIR=$control WII_COMPILED_PRIME_KEYBOARD_MOUSE=1 WII_COMPILED_QA_TAP_INTRO=1 \
        nohup ./"Corruption_3 PC" --iso "$iso" > "$control/stdout.log" 2>&1 &
    echo "Game started (pid $!), ready to record. Wait for the title screen."
}

case "${1:-}" in
    --status)
        cat "$control/status.txt" 2>/dev/null || echo 'no status (game not started with qa_record.sh)'
        exit 0 ;;
    --stop)
        running || { echo 'The game is not running.' >&2; exit 1; }
        echo stop > "$control/stop.txt"; sleep 2
        cat "$control/status.txt" 2>/dev/null || true
        exit 0 ;;
    --launch)
        running && { echo 'The game is already running.' >&2; exit 1; }
        launch; exit 0 ;;
    ''|-*)
        echo 'usage: qa_record.sh --launch | NAME [NOTES] | --stop | --status' >&2; exit 2 ;;
esac

name=$1; notes=${2:-}
[[ $name =~ ^[A-Za-z0-9_-]+$ ]] || { echo 'NAME: letters, digits, _ and - only' >&2; exit 2; }
rec=$recordings/$name
[[ -e $rec ]] && { echo "Recording '$name' exists; pick another name or delete it." >&2; exit 1; }
if ! running; then
    launch
    echo 'When the title screen is up, run this command again with the same NAME to start recording.'
    exit 0
fi
# Starting state for replays: the saved game on disk and the configuration, as they are right now.
mkdir -p "$rec"
cp -r "$build/UserData/NAND" "$rec/NAND"
cp "$build/UserData/Config.toml" "$rec/Config.toml"
python3 -I - "$rec/meta.json" "$name" "$notes" "$(sha256sum "$exe" | cut -d' ' -f1)" <<'EOF'
import datetime, json, sys
path, name, notes, sha = sys.argv[1:]
json.dump({'name': name, 'recorded_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
           'notes': notes, 'exe_sha256': sha, 'host': 'linux', 'tap_intro': True}, open(path, 'w'), indent=2)
EOF
echo "$rec/inputs.txt" > "$control/start.txt"; sleep 2
reported=$(cat "$control/status.txt" 2>/dev/null || true)
echo "$reported"
if [[ $reported == refused* ]]; then rm -rf "$rec"; else echo 'Play the tour, then: qa_record.sh --stop   (the game keeps running)'; fi
