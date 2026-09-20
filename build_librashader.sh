#!/usr/bin/env bash
# Builds librashader's C API as a shared library and drops it, correctly
# named, next to the AGS engine binary (no linking step, no changes to the AGS
# build system). librashader_ld.h dlopen()s the bare name "librashader.so",
# which the loader only finds through LD_LIBRARY_PATH or the system library
# path (AGS sets no $ORIGIN rpath), so run the engine with LD_LIBRARY_PATH
# pointing at this folder. agssetup does that automatically.
#
# Requires a Rust toolchain >= 1.87 (librashader's dependency tree needs it:
# naga 30 refuses to build on older compilers). On Arch/CachyOS: `sudo pacman -S rust` is current enough.
# On Debian/Ubuntu, the distro rustc is usually too old — use rustup
# (https://rustup.rs) instead.
set -euo pipefail

ENGINE_DIR="${1:?Usage: build_librashader.sh /path/to/ags/engine/output/dir}"

# Validate engine directory exists and is writable
if [ ! -d "$ENGINE_DIR" ]; then
  echo "Error: Engine directory does not exist: $ENGINE_DIR" >&2
  exit 1
fi

if [ ! -w "$ENGINE_DIR" ]; then
  echo "Error: Engine directory is not writable: $ENGINE_DIR" >&2
  exit 1
fi

if ! command -v cargo >/dev/null; then
  echo "cargo not found. Install a Rust toolchain (rustup.rs) and re-run." >&2
  exit 1
fi

RUST_VER="$(rustc --version | awk '{print $2}')"
echo "Using rustc $RUST_VER"

# Check Rust version (librashader requires >= 1.87)
RUST_MAJOR=$(echo "$RUST_VER" | cut -d. -f1)
RUST_MINOR=$(echo "$RUST_VER" | cut -d. -f2)
RUST_VERSION_NUM=$((RUST_MAJOR * 100 + RUST_MINOR))

if [ "$RUST_VERSION_NUM" -lt 187 ]; then
  echo "Error: librashader requires Rust >= 1.87, but found $RUST_VER" >&2
  echo "Update Rust via rustup: https://rustup.rs" >&2
  exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "Cloning librashader..."
git clone --depth 1 --quiet https://github.com/SnowflakePowered/librashader.git "$WORKDIR/librashader" 2>&1 | grep -v "^hint:" || true
if [ ! -d "$WORKDIR/librashader" ]; then
  echo "Error: Failed to clone librashader repository" >&2
  exit 1
fi

cd "$WORKDIR/librashader" || exit 1

# Only the OpenGL runtime is built: no Vulkan/D3D/Metal deps, much faster,
# and it's the only backend the AGS patch calls into.
cargo build --release -p librashader-capi --no-default-features --features runtime-opengl

mkdir -p "$ENGINE_DIR"
cp target/release/liblibrashader_capi.so "$ENGINE_DIR/librashader.so"
echo "Installed $ENGINE_DIR/librashader.so"
