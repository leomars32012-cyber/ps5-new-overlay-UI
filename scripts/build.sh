#!/usr/bin/env bash
# Build fps-monitor.elf (monitor_shellui.elf is built first and embedded).
#
# Usage:
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   ./scripts/build.sh                 # Release build into ./build
#   ./scripts/build.sh --debug         # Debug build (defines DEBUG)
#   ./scripts/build.sh --jobs 8 --build-dir /tmp/fm-build
#
# Output: <build-dir>/bin/fps-monitor.elf
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-${ROOT}/build}"
BUILD_TYPE="Release"
JOBS="$(nproc 2>/dev/null || echo 4)"
PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-${PS5SDK:-}}"

die() { printf '[error] %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --jobs) JOBS="$2"; shift 2 ;;
    --build-dir) BUILD="$2"; shift 2 ;;
    --debug) BUILD_TYPE="Debug"; shift ;;
    --release) BUILD_TYPE="Release"; shift ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
done

[[ -n "${PS5_PAYLOAD_SDK}" ]] || die "PS5_PAYLOAD_SDK is not set"
[[ -x "${PS5_PAYLOAD_SDK}/bin/prospero-cmake" ]] ||
  die "prospero-cmake not found under ${PS5_PAYLOAD_SDK}"
for cmd in cmake ninja python3; do
  command -v "${cmd}" >/dev/null 2>&1 || die "missing command: ${cmd}"
done
# prospero-clang++ links libc++ from the SDK; binary installs without it fail
# at link time with "unable to find library -lc++".
[[ -f "${PS5_PAYLOAD_SDK}/target/lib/libc++.a" ]] ||
  die "SDK has no target/lib/libc++.a; install a full ps5-payload-sdk release"

export PS5_PAYLOAD_SDK
export PATH="${PS5_PAYLOAD_SDK}/bin:${PATH}"

"${PS5_PAYLOAD_SDK}/bin/prospero-cmake" \
  -S "${ROOT}/source" \
  -B "${BUILD}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DPS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK}"
cmake --build "${BUILD}" -j"${JOBS}" --target fps_monitor

printf '\n[ok] %s\n' "${BUILD}/bin/fps-monitor.elf"
