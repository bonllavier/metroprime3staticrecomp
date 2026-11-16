#!/usr/bin/env bash
# Packages the public Linux release of Corruption_3 PC: two archives of the same version, as on Windows
# (package_release.ps1; decision of 16/11/2026, recomp/docs/publicacion_github.md):
#   dist/Corruption_3-PC-vX.Y.Z-linux-x86_64.tar.gz                everything for Metroid Prime 3: Corruption (USA)
#   dist/Corruption_3-PC-vX.Y.Z-Trilogy-addon-linux-x86_64.tar.gz  only the two Metroid Prime Trilogy (USA) binaries,
#                                                                  extracted by the player into the main folder
# Each with a .sha256. tar keeps the executable bit, which a zip would lose.
#
#   source /mnt/mp3port/env.sh
#   recomp/tools/package_release_linux.sh [--build-dir DIR] [--out DIR] [--layout-test]
#
# The build must be public (build_prime_runtime.sh without --qa; Trilogy: build_trilogy.py --public, which
# installs both binaries next to "Corruption_3 PC"). --layout-test packages the local test build only to
# check the archives' layout: it skips the QA and diagnostics checks and its archives are not releases.
# Written on Windows on 16/11/2026 and not run on Linux yet (see recomp/docs/linux_port.md).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${MP3_BUILD_DIR:-${MP3PORT:-/mnt/mp3port}/builds/RM3E01_00_runtime_linux}
out=$root/dist
layout_test=0
while [[ $# -gt 0 ]]; do
    case $1 in
        --build-dir) build_dir=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --layout-test) layout_test=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
die() { echo "package_release_linux: $*" >&2; exit 1; }

header=$root/recomp/third_party/WiiCompiled/runtime/include/prime_port_version.h
version=$(sed -n 's/^#define PRIME_PORT_VERSION "\([^"]*\)".*/\1/p' "$header")
[[ -n $version ]] || die "PRIME_PORT_VERSION not found in $header"
publish=$root/publish
main_name=Corruption_3-PC-v$version-linux-x86_64
addon_name=Corruption_3-PC-v$version-Trilogy-addon-linux-x86_64
main_stage=$out/$main_name
addon_stage=$out/$addon_name
rm -rf "$main_stage" "$addon_stage"
mkdir -p "$main_stage/docs" "$addon_stage"

# The three binaries: checks as on Windows (no QA tools, no diagnostics), debug sections stripped.
strip_tool=$(command -v llvm-strip || command -v strip) || die 'llvm-strip or strip is needed'
check_binary() {
    local file=$1 name=$2
    [[ -f $file ]] || die "missing $file (Trilogy: run recomp/tools/trilogy/build_trilogy.py --public)"
    [[ -x $file ]] || die "$file is not executable"
    if (( ! layout_test )); then
        grep -q MKW_PRIME_QA_TOOLS_ENABLED_BUILD "$file" && die "$name was built with the QA tools; rebuild without --qa"
        for marker in '[host-stall]' 'others_over_ms' '[gpu-time]' '[post-ssao]' 'Game performance [F9]' \
                      'WII_COMPILED_PRIME_PERFORMANCE_TRACE' 'WII_COMPILED_GPU_TIMING' 'capture_%Y%m%d'; do
            grep -qF -- "$marker" "$file" && die "$name still carries diagnostics: $marker"
        done
        grep -qF "$version" "$file" || die "$name does not carry version $version; rebuild after bumping the version"
    fi
    return 0
}
copy_binary() {
    local name=$1 destination=$2
    check_binary "$build_dir/$name" "$name"
    cp -f "$build_dir/$name" "$destination/$name"
    "$strip_tool" --strip-debug "$destination/$name"
    chmod 755 "$destination/$name"
}
copy_binary 'Corruption_3 PC' "$main_stage"
copy_binary 'Corruption_3 Trilogy Menu' "$addon_stage"
copy_binary 'Corruption_3 Trilogy MP3' "$addon_stage"

# Shared libraries the build placed next to the binary (Dawn, SDL3, libpng...), runtime data and the
# start-up screens, as build_prime_runtime.sh leaves them.
shopt -s nullglob
for library in "$build_dir"/*.so "$build_dir"/*.so.*; do cp -fP "$library" "$main_stage/"; done
shopt -u nullglob
for asset in dsp_coef.bin initial_pipeline_cache.db Config.default.toml; do
    [[ -f $build_dir/$asset ]] || die "missing $build_dir/$asset"
    cp -f "$build_dir/$asset" "$main_stage/"
done
[[ -d $build_dir/wii_bootstrap ]] || die "missing $build_dir/wii_bootstrap"
cp -r "$build_dir/wii_bootstrap" "$main_stage/"
mkdir -p "$main_stage/texture_replacements"
cp -r "$build_dir/texture_replacements/strap_screen" "$main_stage/texture_replacements/"
echo 'UserData, Reports and caches are kept in this folder.' > "$main_stage/portable.txt"
mkdir -p "$main_stage/game"
echo 'Put your Metroid Prime 3: Corruption (USA) or Metroid Prime Trilogy (USA) disc image (.iso or .rvz) here.' \
    > "$main_stage/game/PUT_YOUR_DISC_IMAGE_HERE.txt"

# Every library the binary needs must come from the system or from the package itself.
missing=$(cd "$main_stage" && LD_LIBRARY_PATH=. ldd './Corruption_3 PC' | grep 'not found' || true)
[[ -z $missing ]] || die "unresolved libraries: $missing"

for doc in README.md CHANGELOG.md LICENSE THIRD-PARTY-NOTICES.md; do
    [[ -f $publish/$doc ]] || die "missing $publish/$doc"
    cp -f "$publish/$doc" "$main_stage/"
done
for doc in controls.md development.md troubleshooting.md; do cp -f "$publish/docs/$doc" "$main_stage/docs/"; done
if (( ! layout_test )); then
    grep -q "^## v$version\b" "$main_stage/CHANGELOG.md" || die "CHANGELOG.md has no '## v$version' section"
fi

cat > "$addon_stage/README - Trilogy add-on.txt" <<EOF
Corruption_3 PC v$version - Metroid Prime Trilogy add-on (Linux)

This archive adds Metroid Prime Trilogy (USA, game ID R3ME01) to Corruption_3 PC. Only the Metroid Prime 3
of that disc runs; Metroid Prime and Metroid Prime 2 are not part of the port.

1. Install Corruption_3 PC v$version first ($main_name.tar.gz).
2. Extract this archive into that same folder, next to "Corruption_3 PC":
     tar -xzf $addon_name.tar.gz -C <your Corruption_3 PC folder> --strip-components=1
   It contains "Corruption_3 Trilogy Menu" and "Corruption_3 Trilogy MP3" (both executable).
3. Start "Corruption_3 PC" and choose your Metroid Prime Trilogy (USA) disc image (.iso or .rvz),
   or change it in F1 > Diagnostics. The Trilogy menu opens, where saves are handled, and it starts
   Metroid Prime 3.

Both archives must be the same version: when you update Corruption_3 PC, download the add-on of the new
version too and extract it over the folder after the main archive. A mismatch is reported at start-up.
F1 > Info shows whether the add-on is installed. RetroAchievements are not available with the Trilogy disc.

No game data is included: the port reads your own disc image, which it never modifies.
Project page and documentation: https://github.com/bonllavier/Corruption_3-PC
EOF
cp -f "$publish/LICENSE" "$publish/THIRD-PARTY-NOTICES.md" "$addon_stage/"

# Nothing of the game or of the player, no credentials (the same rules as package_release.ps1).
bad=$(find "$main_stage" "$addon_stage" \( -name UserData -o -name save.bin -o -name banner.bin -o -name 'Config.toml' \
      -o -name retroachievements.dat -o -iname '*.iso' -o -iname '*.rvz' -o -iname '*.wbfs' -o -iname '*.dol' \
      -o -iname '*.rel' -o -iname '*.rso' -o -iname '*.pak' -o -name '*.log' \) -print)
[[ -z $bad ]] || die "game, player or private data in the staged folders: $bad"
addon_files=$(cd "$addon_stage" && find . -type f | sort | tr '\n' '|')
[[ $addon_files == './Corruption_3 Trilogy MP3|./Corruption_3 Trilogy Menu|./LICENSE|./README - Trilogy add-on.txt|./THIRD-PARTY-NOTICES.md|' ]] \
    || die "unexpected add-on contents: $addon_files"

write_tar() {
    local name=$1
    local archive=$out/$name.tar.gz
    rm -f "$archive" "$archive.sha256"
    tar -C "$out" --owner=0 --group=0 --numeric-owner -czf "$archive" "$name"
    (cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
    echo "$archive ($(du -m "$archive" | cut -f1) MB) sha256 $(cut -d' ' -f1 "$archive.sha256")"
}
echo "release: $(write_tar "$main_name")"
echo "addon:   $(write_tar "$addon_name")"
(( layout_test )) && echo 'LAYOUT TEST ONLY: built from the local test build; these archives are not releases.'
exit 0
