#!/usr/bin/env bash
# Host tests for the pure overlay layout and settings web app helpers
# (no PS5 SDK needed).
#   ./scripts/test.sh                 build and run tests/host/web_config_test.cpp
#                                     and tests/host/scene_test.cpp
#   ./scripts/test.sh --render [DIR]  also render sample scenes to PNG
#                                     (default DIR: build/renders; needs rsvg-convert)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Werror -I "$root/source/fps_monitor/source" \
  "$root/tests/host/web_config_test.cpp" -o "$tmp/web_config_test"
"$tmp/web_config_test"

"${CXX:-c++}" -std=c++20 -Wall -Werror -I "$root/source/shellui/include" \
  "$root/tests/host/scene_test.cpp" -o "$tmp/scene_test"

if [ "${1:-}" != "--render" ]; then
  exec "$tmp/scene_test"
fi

out="${2:-$root/build/renders}"
mkdir -p "$out" "$tmp/svg"
status=0
"$tmp/scene_test" --render "$tmp/svg" || status=$?
for svg in "$tmp"/svg/*.svg; do
  rsvg-convert "$svg" -o "$out/$(basename "${svg%.svg}").png"
done
echo "renders: $out"
exit "$status"
