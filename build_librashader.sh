#!/usr/bin/env bash
# Builds librashader's C API as a shared library and drops it, correctly
# named, next to the AGS engine binary so librashader_ld.h's dlopen() call
# finds it at runtime (no linking step, no changes to the AGS build system).
#
# Requires a Rust toolchain >= 1.85 (librashader's dependency tree uses the
# 2024 edition). On Arch/CachyOS: `sudo pacman -S rust` is current enough.
# On Debian/Ubuntu, the distro rustc is usually too old — use rustup
# (https://rustup.rs) instead.
set -euo pipefail

ENGINE_DIR="${1:?Usage: build_librashader.sh /path/to/ags/engine/output/dir}"

if ! command -v cargo >/dev/null; then
  echo "cargo not found. Install a Rust toolchain (rustup.rs) and re-run." >&2
  exit 1
fi

RUST_VER="$(rustc --version | awk '{print $2}')"
echo "Using rustc $RUST_VER"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

git clone --depth 1 https://github.com/SnowflakePowered/librashader.git "$WORKDIR/librashader"
cd "$WORKDIR/librashader"

# Only the OpenGL runtime is built: no Vulkan/D3D/Metal deps, much faster,
# and it's the only backend the AGS patch calls into.
cargo build --release -p librashader-capi --no-default-features --features runtime-opengl

mkdir -p "$ENGINE_DIR"
cp target/release/liblibrashader_capi.so "$ENGINE_DIR/librashader.so"
echo "Installed $ENGINE_DIR/librashader.so"
