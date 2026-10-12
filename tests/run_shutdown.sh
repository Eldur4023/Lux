#!/usr/bin/env bash
# Orderly shutdown with idle keep-alive connections (see tests/shutdown.py).
#   tests/run_shutdown.sh [path-to-binary]
exec python3 "$(dirname "$0")/shutdown.py" "${1:-$(dirname "$0")/../build/lux}" "${LUX_TEST_SHUTDOWN_PORT:-8861}"
