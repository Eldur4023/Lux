#!/usr/bin/env bash
# Regenerates the committed profile (pgo/) that every normal build applies on its own.
#
#   tools/pgo.sh [BUILD_DIR] -- COMMAND...
#
# 1. builds an instrumented lux in BUILD_DIR (default build-pgo);
# 2. runs COMMAND with LUX_BIN pointing at it: drive it with a load that looks like production (the profile
#    is only as good as the traffic: routes, templates, JSON, SQLite reads and writes, static files).  The
#    server must exit cleanly (SIGINT) for the counters to be written;
# 3. replaces pgo/*.gcda with the new counters and rebuilds BUILD_DIR with them.
# Commit pgo/ afterwards.  Run it again after big changes to the hot paths or a compiler upgrade.
#
# Example:  tools/pgo.sh build-pgo -- ~/Escritorio/bench/forum/run.sh "1000" 10 100
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-build-pgo}"; shift || true
[ "${1:-}" = "--" ] && shift
[ $# -gt 0 ] || { echo "usage: tools/pgo.sh [BUILD_DIR] -- COMMAND..." >&2; exit 2; }
case "$BUILD" in /*) ;; *) BUILD="$HERE/$BUILD" ;; esac
rm -rf "$BUILD/pgo"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DLUX_EXAMPLES=OFF -DLUX_PGO=gen
cmake --build "$BUILD" --target lux-bin -j"$(nproc)"
LUX_BIN="$BUILD/lux" "$@"
ls "$BUILD/pgo"/*.gcda >/dev/null 2>&1 || { echo "no profile written: did the server exit cleanly?" >&2; exit 1; }
rm -rf "$HERE/pgo"; mkdir "$HERE/pgo"; cp "$BUILD/pgo"/*.gcda "$HERE/pgo/"
cmake -S "$HERE" -B "$BUILD" -DLUX_PGO=auto
cmake --build "$BUILD" --target lux-bin -j"$(nproc)"
echo "profile in $HERE/pgo ($(ls "$HERE/pgo" | wc -l) files); optimised binary: $BUILD/lux"
