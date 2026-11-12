# Example environment for building the port on Linux. Copy it, adjust the paths and `source` it in
# every shell that builds or runs the game. It keeps the toolchain, caches and temporary files on one
# disk of your choice (the maintainer uses a dedicated disk mounted at /mnt/mp3port).
export MP3PORT=$HOME/mp3port                       # tools, caches and builds live under here
export MP3_TOOLCHAIN=$MP3PORT/tools/portable/toolchain-x86_64   # from prepare-portable-tools.sh
export NOD_DIR=$MP3PORT/tools/nod-source           # build_nod_ffi.sh clones and builds nod here
export MP3_BUILD_DIR=$MP3PORT/builds/RM3E01_00_runtime_linux    # build_prime_runtime.sh output

# .NET 8 SDK
export DOTNET_ROOT=$MP3PORT/tools/dotnet
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_CLI_HOME=$MP3PORT/cache/dotnet-home
export NUGET_PACKAGES=$MP3PORT/cache/nuget
# Rust (for nod)
export RUSTUP_HOME=$MP3PORT/tools/rustup
export CARGO_HOME=$MP3PORT/tools/cargo
# Caches (CMake FetchContent, pip) and temporary files
export XDG_CACHE_HOME=$MP3PORT/cache/xdg
export PIP_CACHE_DIR=$MP3PORT/cache/pip
export TMPDIR=$MP3PORT/cache/tmp

export PATH=$MP3_TOOLCHAIN/bin:$DOTNET_ROOT:$CARGO_HOME/bin:$PATH
mkdir -p "$DOTNET_CLI_HOME" "$NUGET_PACKAGES" "$XDG_CACHE_HOME" "$PIP_CACHE_DIR" "$TMPDIR" "$MP3PORT/builds"
