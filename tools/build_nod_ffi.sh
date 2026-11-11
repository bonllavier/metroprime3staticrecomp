#!/usr/bin/env bash
# Linux counterpart of build_nod_ffi.ps1: builds the pinned nod FFI archive the
# runtime links. The compressors are not optional: without them .rvz, .wia and
# .gcz images fail when opened.
#
# Usage: source /mnt/mp3port/env.sh && recomp/tools/build_nod_ffi.sh
# Output: $NOD_DIR/target/x86_64-unknown-linux-gnu/release/libnod.a
set -euo pipefail

workspace=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
nod_dir=${NOD_DIR:-${MP3PORT:-$workspace/recomp/.tools}/tools/nod-source}
nod_commit=6a54b418e541ce5eeb364399116afd6e47b817bb
features=threading,compress-zstd-static,compress-zlib-static,compress-bzip2-static,compress-lzma-static
target=x86_64-unknown-linux-gnu
expected_rust='rustc 1.99.1 (58cf8f9c3 2026-10-13)'

if [[ ! -d "$nod_dir" ]]; then
    git clone --filter=blob:none --no-checkout https://github.com/encounter/nod.git "$nod_dir"
    git -C "$nod_dir" checkout -q --detach "$nod_commit"
fi
if [[ -n "$(git -C "$nod_dir" status --porcelain)" ]]; then
    echo 'The local nod source tree has modifications; refusing to build unpinned code.' >&2
    exit 1
fi
git -C "$nod_dir" checkout -q --detach "$nod_commit"
actual_commit=$(git -C "$nod_dir" rev-parse HEAD)
[[ "$actual_commit" == "$nod_commit" ]] || { echo "Unexpected nod commit: $actual_commit" >&2; exit 1; }

rust_version=$(rustc --version)
[[ "$rust_version" == "$expected_rust" ]] || { echo "Unexpected Rust toolchain: $rust_version" >&2; exit 1; }

# The C compressors build with the same clang/llvm-ar as the runtime.
export CC_x86_64_unknown_linux_gnu=${CC_x86_64_unknown_linux_gnu:-clang}
export AR_x86_64_unknown_linux_gnu=${AR_x86_64_unknown_linux_gnu:-llvm-ar}
export CARGO_TARGET_DIR="$nod_dir/target"

cargo build --locked --release --target "$target" \
    --manifest-path "$nod_dir/nod-ffi/Cargo.toml" \
    --no-default-features --features "$features"

release_dir="$nod_dir/target/$target/release"
[[ -f "$release_dir/libnod.a" ]] || { echo "libnod.a was not produced in $release_dir" >&2; exit 1; }
# The build wrapper refuses an archive without this record.
printf '%s\n' "$features" > "$release_dir/libnod.features.txt"
echo "nod_commit=$actual_commit"
echo "rust_toolchain=$rust_version"
echo "nod_features=$features"
echo "nod_archive=$release_dir/libnod.a"
