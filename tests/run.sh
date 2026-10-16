#!/usr/bin/env bash
# Builds and runs the agssetup GUI tests.
#
#   tests/run.sh                         # offscreen: everything but the OpenGL preview
#   xvfb-run -a tests/run.sh             # with a display: the preview is rendered for real
#   LIBRASHADER_SO=/path/librashader.so tests/run.sh
#                                        # also exercises the shader parameter listing and the
#                                        # filter chain (skipped without a librashader.so)
#   SANITIZE=1 tests/run.sh              # AddressSanitizer + UBSan
#
# agssetup keeps its classes inside agssetup.cpp, so the tests are built from a
# COPY of that file with tests/cases.inc appended: the copy gets a different main()
# and is compiled with -fno-access-control, which lets the tests call private
# members without changing the shipped source.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
if [ -n "${TEST_BUILD_DIR:-}" ]; then
    work=$TEST_BUILD_DIR
else
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
fi
mkdir -p "$work"
cd "$work"

qmake=${QMAKE:-$(command -v qmake6 || command -v qmake)}

sed -e 's/^int main(int argc, char \*argv\[\]) {$/int agssetup_main(int argc, char *argv[]) {/' \
    -e 's/^#include "agssetup.moc"$//' "$root/agssetup.cpp" > gui_test.cpp
if ! grep -q '^int agssetup_main' gui_test.cpp; then
    echo "tests/run.sh: could not find main() in agssetup.cpp to rename" >&2
    exit 2
fi
cat "$here/cases.inc" >> gui_test.cpp

cat > gui_test.pro <<EOF
QT += widgets openglwidgets network testlib
CONFIG += c++17 console
CONFIG -= app_bundle
QMAKE_CXXFLAGS += -fno-access-control -Wno-unused-parameter
INCLUDEPATH += $root
SOURCES = gui_test.cpp
TARGET = gui_test
EOF
if [ -n "${SANITIZE:-}" ]; then
    echo 'CONFIG += sanitizer sanitize_address sanitize_undefined' >> gui_test.pro
fi

"$qmake" gui_test.pro > /dev/null
make -j"$(nproc)" > build.log 2>&1 || { cat build.log; exit 1; }

if [ -n "${LIBRASHADER_SO:-}" ]; then
    cp "$LIBRASHADER_SO" ./librashader.so
fi

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
if [ -z "${QT_QPA_PLATFORM:-}" ] && [ -z "${DISPLAY:-}" ]; then
    export QT_QPA_PLATFORM=offscreen
fi
echo "platform: ${QT_QPA_PLATFORM:-default (DISPLAY=${DISPLAY:-})}"
./gui_test
