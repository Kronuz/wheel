#!/usr/bin/env bash
# Probe: the single row for a and w twice, with the harness's forwarders left to the compiler and
# then always inlined, so the inlining of the harness's own wrappers is told apart from the wheel.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
rounds=${1:-3}
CANDIDATES="a w" BUILD_DIR="$here/../.scratch/fwd-default" "$here/wheel_bench.sh" single "$rounds"
CANDIDATES="a w" BUILD_DIR="$here/../.scratch/fwd-inline" WHEEL_BENCH_CXXFLAGS="-DWHEEL_BENCH_FORWARD=[[gnu::always_inline]]" "$here/wheel_bench.sh" single "$rounds"
