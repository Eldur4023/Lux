#!/usr/bin/env bash
# Profile-guided build of Lux (about -4% instructions per request on the forum bench, with LTO).
#
#   tools/pgo.sh [BUILD_DIR] -- COMMAND...
#
# 1. configures BUILD_DIR (default build-pgo) with -DLUX_PGO=gen -DLUX_LTO=ON and builds the instrumented lux;
# 2. runs COMMAND with LUX_BIN pointing at it: drive it with a load that looks like production (the profile is
#    only as good as the traffic: routes, templates, JSON, static files).  The server must exit cleanly (SIGINT)
#    for the counters to be written;
# 3. reconfigures the same tree with -DLUX_PGO=use and rebuilds: BUILD_DIR/lux is the optimised binary.
#
# Example:  tools/pgo.sh build-pgo -- ~/Escritorio/bench/forum/run.sh "1000" 10 100
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-build-pgo}"; shift || true
[ "${1:-}" = "--" ] && shift
[ $# -gt 0 ] || { echo "usage: tools/pgo.sh [BUILD_DIR] -- COMMAND..." >&2; exit 2; }
case "$BUILD" in /*) ;; *) BUILD="$HERE/$BUILD" ;; esac
rm -rf "$BUILD/pgo"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DLUX_EXAMPLES=OFF -DLUX_LTO=ON -DLUX_PGO=gen
cmake --build "$BUILD" --target lux-bin -j"$(nproc)"
LUX_BIN="$BUILD/lux" "$@"
ls "$BUILD/pgo"/*.gcda >/dev/null 2>&1 || { echo "no profile written: did the server exit cleanly?" >&2; exit 1; }
cmake -S "$HERE" -B "$BUILD" -DLUX_PGO=use
cmake --build "$BUILD" --target lux-bin -j"$(nproc)"
echo "optimised binary: $BUILD/lux"
