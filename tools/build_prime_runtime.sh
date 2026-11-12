#!/usr/bin/env bash
# Linux counterpart of build_prime_runtime.ps1: regenerates the RM3E01_00 RSO
# translations, DOL switch repairs, runtime hooks and build shards, then builds
# the native executable. Same steps and checks as the Windows script.
#
# Prerequisites (see docs/linux_port.md):
#   source /mnt/mp3port/env.sh
#   recomp/tools/build_nod_ffi.sh                      # pinned nod FFI archive
#   private build inputs extracted from the player's disc image with
#   nod_extract_build_inputs into recomp/.tools/disc-extraction-rm3e01 and
#   decomp/orig/RM3E01_00 (build-time only; the game reads the image at run time)
#   generated/RM3E01_00_conservative base translation
#
# Usage: recomp/tools/build_prime_runtime.sh [-j JOBS] [--build-dir DIR] [--qa | --public]
#   --qa      test build with the private QA tools (F5/F7 draw dumps, F8 stack profile, F9 panel),
#             like the Windows test builds; never package it (package_release.ps1 refuses it).
#   --public  build without them. Without either flag the build directory keeps its current choice.
set -euo pipefail

jobs=$(nproc)
qa_tools=
task_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${MP3_BUILD_DIR:-$task_root/recomp/build/RM3E01_00_runtime_linux}
while [[ $# -gt 0 ]]; do
    case "$1" in
        -j|--jobs) jobs=$2; shift 2 ;;
        --build-dir) build_dir=$2; shift 2 ;;
        --qa) qa_tools=ON; shift ;;
        --public) qa_tools=OFF; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
cd "$task_root"

die() { echo "build_prime_runtime.sh: $*" >&2; exit 1; }
step() { echo "== $*"; }

# --- pinned nod archive -------------------------------------------------------
task_nod=${NOD_DIR:-${MP3PORT:-$task_root/recomp/.tools}/tools/nod-source}
task_nod_library="$task_nod/target/x86_64-unknown-linux-gnu/release/libnod.a"
[[ -f "$task_nod_library" ]] || die "Pinned nod archive missing; run recomp/tools/build_nod_ffi.sh first."
# An archive without compressors links fine and then fails on .rvz, .wia and .gcz images.
grep -q compress-zstd "$(dirname "$task_nod_library")/libnod.features.txt" ||
    die 'The nod archive has no compressors; run recomp/tools/build_nod_ffi.sh.'
[[ "$(git -C "$task_nod" rev-parse HEAD)" == 6a54b418e541ce5eeb364399116afd6e47b817bb ]] || die 'Unexpected nod source revision'
[[ -z "$(git -C "$task_nod" status --porcelain)" ]] || die 'Pinned nod source is modified'

# --- private build inputs -----------------------------------------------------
extraction=recomp/.tools/disc-extraction-rm3e01
[[ -f "$extraction/sys/main.dol" && -d "$extraction/files/RSO/Rev2 Production" ]] ||
    die "Missing $extraction; extract it from the disc image with nod_extract_build_inputs."
[[ -f decomp/orig/RM3E01_00/sys/main.dol ]] || die 'Missing decomp/orig/RM3E01_00 (link it to the extraction).'
echo '3e81f9b09dfb1e7c02e22b9c6c48a4a1390224b2  decomp/orig/RM3E01_00/sys/main.dol' | sha1sum -c --quiet ||
    die 'main.dol is not RM3E01_00'
[[ -f recomp/generated/RM3E01_00_conservative/base_translation_output.json ]] ||
    die 'Missing generated/RM3E01_00_conservative base translation.'

# --- translators --------------------------------------------------------------
step 'translators'
dotnet build recomp/third_party/WiiCompiled/translator/src/Translator.Cli/Translator.Cli.csproj -c Release --nologo --no-incremental -v quiet ||
    die 'Prime shard translator build failed'
dotnet build recomp/tools/RsoRuntimeTranslator/RsoRuntimeTranslator.csproj -c Release --nologo --no-incremental -v quiet ||
    die 'Prime RSO translator build failed'
rso_translator=recomp/tools/RsoRuntimeTranslator/bin/Release/net8.0/RsoRuntimeTranslator.dll
dotnet "$rso_translator" --self-test-relocations || die 'Prime RSO relocated instruction tests failed'

# --- RSO modules --------------------------------------------------------------
step 'RSO modules'
mapfile -t task_modules < <(python3 -I -c 'import json,sys; [print(m["name"]+","+m["symbol"]) for m in json.load(open(sys.argv[1]))["modules"]]' recomp/config/RM3E01_00/rso_modules.json)
rso_dir="$extraction/files/RSO/Rev2 Production"
mkdir -p recomp/.tools/rso-rel recomp/.tools/rso-runtime
for spec in "${task_modules[@]}"; do
    module=${spec%%,*}
    python3 -B recomp/tools/convert_rso_to_rel_probe.py "$rso_dir/RSO_$module.rso" "recomp/.tools/rso-rel/$module.synthetic.rel" >/dev/null ||
        die "Prime $module analysis image conversion failed"
    python3 -B recomp/tools/prepare_prime_rso_translation.py --module "$module" >/dev/null ||
        die "Prime $module manifest validation failed"
    dotnet "$rso_translator" "recomp/.tools/rso-runtime/$module/input.json" "recomp/.tools/rso-runtime/$module/functions" >/dev/null ||
        die "Prime $module translation failed"
    python3 -B recomp/tools/verify_rso_rel_probe.py "$rso_dir/RSO_$module.rso" \
        "recomp/.tools/rso-rel/$module.synthetic.rel" "recomp/.tools/rso-runtime/$module/functions/relocated_module.bin" \
        --output "recomp/.tools/rso-runtime/$module/nominal_relocations.json" >/dev/null ||
        die "Prime $module independent relocation verification failed"
    python3 -B recomp/tools/emit_prime_rso_registry.py --module "$module" >/dev/null ||
        die "Prime $module registry validation failed"
done
echo "${#task_modules[@]} modules translated and verified"
# CMake links every module listed here (name,descriptor symbol).
{
    echo 'set(MKW_PRIME_RSO_MODULE_SPECS'
    for spec in "${task_modules[@]}"; do echo "    \"$spec\""; done
    echo ')'
} > recomp/.tools/rso-runtime/modules.cmake.new
cmp -s recomp/.tools/rso-runtime/modules.cmake.new recomp/.tools/rso-runtime/modules.cmake 2>/dev/null &&
    rm recomp/.tools/rso-runtime/modules.cmake.new ||
    mv recomp/.tools/rso-runtime/modules.cmake.new recomp/.tools/rso-runtime/modules.cmake
python3 -B recomp/tools/audit_prime_rso_address_taken.py --output recomp/.tools/rso-runtime/address_taken_coverage.json >/dev/null ||
    die 'Prime RSO address-taken entry coverage failed'

# --- DOL switch repairs -------------------------------------------------------
step 'DOL switch repairs'
mapfile -t extra_repairs < <(python3 -I -c 'import json,sys; [print(a) for a in json.load(open(sys.argv[1]))["repair_functions"]]' recomp/config/RM3E01_00/dol_jump_tables.json)
for repair in 800E01F0 80420444 8019804C 8042B530 80423190 80423EF8 \
        8040D9D8 8040F98C 80414E20 80415CB8 804167CC 80419848 \
        80425B10 80427A34 804280AC "${extra_repairs[@]}"; do
    dotnet "$rso_translator" --dol-repair . "$repair" recomp/.tools/dol-runtime-repairs >/dev/null ||
        die "Prime DOL switch repair $repair translation failed"
done

# --- hooks, pipeline seed, shards ---------------------------------------------
step 'runtime hooks'
python3 -B recomp/tools/prepare_prime_runtime_hooks.py || die 'Runtime overlay verification failed'
# Fold pipelines this machine has recorded into the shipped seed (portable recipes only).
python3 -B recomp/tools/prime_pipeline_seed.py merge --cache "$build_dir/UserData/Cache/pipeline_cache.db" ||
    die 'Pipeline seed merge failed'
# The section-blob assembly is host specific (ELF section flags, absolute .incbin
# paths): a copy generated on Windows (COFF, G:/...) does not assemble here.
# The blobs and data_sections_init.cpp it writes are byte-identical across hosts.
step 'data section blobs'
dotnet recomp/third_party/WiiCompiled/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll generate-data-init \
    --project recomp/config/RM3E01_00/project_full_conservative.yml >/dev/null || die 'Data section blob generation failed'
step 'build shards'
dotnet recomp/third_party/WiiCompiled/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll emit-build-shards \
    --project recomp/config/RM3E01_00/project_full_conservative.yml \
    --base-metadata recomp/generated/RM3E01_00_conservative/runtime_hooks/base_translation_output.json \
    --base-functions-dir recomp/generated/RM3E01_00_conservative \
    --native-source-dir recomp/third_party/WiiCompiled/runtime/src \
    --out recomp/generated/RM3E01_00_conservative/build_shards || die 'Runtime shard emission failed'

# --- native build ---------------------------------------------------------------
step 'configure'
cmake -S recomp/third_party/WiiCompiled/runtime -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER_TYPE=LLD \
    -DMKW_GAME_PROFILE_RM3E01_00=ON \
    "-DMKW_PRIME_NOD_SOURCE_DIR=$task_nod" \
    "-DMKW_PRIME_RSO_DIR=$task_root/recomp/.tools/rso-runtime/rhbm" \
    ${qa_tools:+"-DMKW_PRIME_QA_TOOLS=$qa_tools"} \
    '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG' || die 'Prime runtime graph configuration failed'
step 'build'
cmake --build "$build_dir" --target WiiCompiled --parallel "$jobs" || die 'Runtime build failed'

# The port's two start-up screens that replace the Wii Remote strap screen (same list
# as package_release.ps1: RM3E01 English/French/Spanish and Trilogy R3ME01 hashes).
# The executable installs them into UserData/texture_replacements on start-up.
step 'start-up screens'
strap_source=recomp/third_party/WiiCompiled/runtime/assets/texture_replacements/strap_screen
strap_target=$build_dir/texture_replacements/strap_screen
mkdir -p "$strap_target"
for image in tex1_832x456_0a2f3408cab0cbcc_14.png tex1_832x456_550dd77642afbb04_14.png \
             tex1_832x456_ad15d2f8f67a24d8_14.png tex1_832x456_daf65c9d00a79c85_14.png \
             tex1_832x456_ad8eb16a5f40d2e2_14.png tex1_832x456_0dd8e2ea1b2d67da_14.png \
             tex1_832x456_f6c8146f6a1af236_14.png tex1_832x456_9a1135bd0715a8fe_14.png; do
    [[ -f "$strap_source/$image" ]] || die "Start-up screen missing: $strap_source/$image"
    cp -f "$strap_source/$image" "$strap_target/"
done

# Release default configuration (keyboard/mouse, key and pad icons, renderer and effects),
# taken from the template in package_release.ps1 so both platforms ship the same defaults.
# The runtime copies it to UserData/Config.toml on the first run only. ReShade is not part
# of the Linux port, so its section is dropped.
step 'default configuration'
python3 -I - recomp/tools/package_release.ps1 "$build_dir/Config.default.toml" <<'PY' || die 'Default configuration extraction failed'
import re, sys
source = open(sys.argv[1], encoding='utf-8-sig').read().replace('\r\n', '\n')
match = re.search(r"@'\n(# Corruption_3 PC - default configuration.*?)\n'@ \| Set-Content[^\n]*Config\.default\.toml", source, re.S)
if not match:
    sys.exit('Config.default.toml template not found in package_release.ps1')
text = re.sub(r"\[reshade\]\n(?:[^\[\n][^\n]*\n|\n)*", "", match.group(1) + "\n")
if '[prime]' not in text or 'keyboard_icons = true' not in text or '[reshade]' in text:
    sys.exit('Unexpected Config.default.toml template')
open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write(text)
PY
echo "built: $(find "$build_dir" -maxdepth 1 -type f -name 'Corruption_3 PC' -print)"
