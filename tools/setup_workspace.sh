#!/usr/bin/env bash
# Prepares a fresh workspace for building (Linux). Run from anywhere:
#   recomp/tools/setup_workspace.sh [extraction-dir]
# 1. Copies the support files of recomp/workspace into the workspace root (never overwrites).
# 2. With an extraction made by nod_extract_build_inputs (sys/main.dol, files/RSO/...), links it where
#    the scripts expect it and copies main.dol into recomp/input/RM3E01_00 after checking its hash.
set -euo pipefail
recomp=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
root=$(cd "$recomp/.." && pwd)
cd "$recomp/workspace"
find . -type f ! -name README.md | while read -r file; do
    target="$root/${file#./}"
    if [[ -e $target ]]; then echo "kept     $target"; else mkdir -p "$(dirname "$target")"; cp "$file" "$target"; echo "copied   $target"; fi
done
extraction=${1:-}
[[ -z $extraction ]] && { echo "No extraction given: skipping the disc inputs."; exit 0; }
extraction=$(cd "$extraction" && pwd)
[[ -f $extraction/sys/main.dol ]] || { echo "$extraction/sys/main.dol not found" >&2; exit 1; }
echo '6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104  '"$extraction/sys/main.dol" | sha256sum -c --quiet ||
    { echo "main.dol is not Metroid Prime 3 Corruption USA (RM3E01) revision 0" >&2; exit 1; }
mkdir -p "$recomp/input/RM3E01_00" "$recomp/.tools" "$root/decomp/orig"
cp -n "$extraction/sys/main.dol" "$recomp/input/RM3E01_00/main.dol"
ln -sfn "$extraction" "$recomp/.tools/disc-extraction-rm3e01"
ln -sfn "$extraction" "$root/decomp/orig/RM3E01_00"
echo "linked   $recomp/.tools/disc-extraction-rm3e01 and decomp/orig/RM3E01_00 -> $extraction"
