#!/usr/bin/env bash
# Disassembles timer_wheel::arm and cancel as a caller inlines them (-O2 -DNDEBUG), for this tree's
# wheel.hh and each TREE given, then with every flag set in $DISASM_VARIANTS ("tag=flags;...").
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
build=${BUILD_DIR:-$root/.scratch/disasm}
mkdir -p "$build"
cat >"$build/probe.cc" <<'CC'
#include "wheel.hh"
void probe_arm(wheel::timer_wheel& w, wheel::timer_entry& e, std::uint64_t d) noexcept { w.arm(e, d); }
void probe_cancel(wheel::timer_wheel& w, wheel::timer_entry& e) noexcept { w.cancel(e); }
CC
IFS=';' read -ra variants <<<"=${DISASM_VARIANTS:+;$DISASM_VARIANTS}"
for tree in "$root" "$@"; do
	tree=$(cd "$tree" && pwd)
	for v in "${variants[@]}"; do
		read -ra flags <<<"${v#*=}"
		echo "== wheel.hh at $(git -C "$tree" rev-parse --short=12 HEAD) flags=\"${flags[*]}\""
		c++ -std=c++20 -O2 -DNDEBUG "${flags[@]}" -I"$tree" -c "$build/probe.cc" -o "$build/probe.o"
		objdump -d --no-show-raw-insn --no-leading-addr -C "$build/probe.o" 2>/dev/null || objdump -d --no-show-raw-insn -C "$build/probe.o"
	done
done
