#!/usr/bin/env bash
# The wheel-evidence rows for wheel::timer_wheel (wheel_bench.cc):
#   wheel_bench.sh single [R]   R interleaved rounds (default 40) of a a2 f w at n = 1e3 and 1e6
#   wheel_bench.sh cross [R]    R interleaved rounds (default 40) of f f2 w at P = 1, 4 and 14
#   wheel_bench.sh pads         the placement probe: a and f at n = 1e3, 3 rounds, with
#                               $PADS bytes between the wheel and the entries
#   wheel_bench.sh ab OLD [R]   w and a against OLD's wheel.hh and this tree's, built alike: arms
#                               w.old (twin w.old.2), w.new, a.old, a.new; R rounds (default 40) at
#                               n = $N (default 1e3). $AB_VARIANTS ("tag=flags;...", default "=")
#                               repeats them per extra flag set, the tag appended to each build.
# Each round runs every arm once, in its own process, in a rotated order, and logs the host's
# load and CPU speed limit. $CANDIDATES narrows the arms (a2 follows a, f2 follows f), $PRODUCERS
# the cross row's P, and $WHEEL_BENCH_CXXFLAGS adds compiler flags to every arm.
# $WHEEL_BENCH_GATE=ARM exits 1 when ARM's median paired ratio misses 1.10x in any row. Each row
# prints its runs and then the medians, the paired ratios and the A/A band (wheel_bench_summary.py).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
mode=${1:?mode}
build=${BUILD_DIR:-$root/.scratch/wheel-bench}
mkdir -p "$build"
echo "host=$(uname -sm) cpu=\"$(sysctl -n machdep.cpu.brand_string 2>/dev/null || sed -n 's/^model name\t*: //p' /proc/cpuinfo | head -1)\" hw_threads=$(getconf _NPROCESSORS_ONLN) compiler=\"$(c++ --version | head -1)\" commit=$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null || echo none) cxxflags=\"${WHEEL_BENCH_CXXFLAGS:-}\""
compile() {  # OUT WHEEL_TREE [FLAGS...]: this tree's bench against WHEEL_TREE's wheel.hh
	local out=$1 tree=$2
	shift 2
	# shellcheck disable=SC2086
	c++ -std=c++20 -pthread -O2 -DNDEBUG ${WHEEL_BENCH_CXXFLAGS:-} "$@" -I"$here" -I"$tree" "$here/wheel_bench.cc" -o "$out"
}
log="$build/$mode.log"
: >"$log"
bin="$build/timing"
run() { "$bin" "$@" | tee -a "$log"; }
declare -A bins=()
run_ab() {  # ARM N: ARM's build is the label after its dot, without a twin's .2
	local tag=${1#*.}
	"${bins[${tag%.2}]}" single "$1" "$2" | tee -a "$log"
}
round() {  # R: a round's header, with what else the host is doing
	local limit
	limit=$(pmset -g therm 2>/dev/null | awk '/CPU_Speed_Limit/ {print $3}')
	echo "round=$1 load=\"$(sysctl -n vm.loadavg 2>/dev/null || cut -d' ' -f1-3 /proc/loadavg)\" speed_limit=${limit:-na}"
}
with_pairs() {  # the arms, each A/A twin after its arm
	local out=() arm
	for arm in "$@"; do
		out+=("$arm")
		[ "$arm" != "$1" ] || out+=("${arm}2")
	done
	echo "${out[@]}"
}
[ "$mode" = ab ] || compile "$bin" "$root"
echo "start=$(date -u +%FT%TZ)"
case $mode in
single)
	rounds=${2:-40}
	read -ra arms <<<"$(with_pairs ${CANDIDATES:-a f w})"
	for ((r = 0; r < rounds; ++r)); do
		for ((k = 0; k < ${#arms[@]}; ++k)); do
			arm=${arms[$(((k + r) % ${#arms[@]}))]}
			round "$r"
			run single "$arm" 1000
			run single "$arm" 1000000
		done
	done
	;;
cross)
	rounds=${2:-40}
	read -ra arms <<<"$(with_pairs ${CANDIDATES:-f w})"
	read -ra producers <<<"${PRODUCERS:-1 4 14}"
	for ((r = 0; r < rounds; ++r)); do
		for ((k = 0; k < ${#arms[@]}; ++k)); do
			arm=${arms[$(((k + r) % ${#arms[@]}))]}
			round "$r"
			for p in "${producers[@]}"; do run cross "$arm" "$p" 1000000; done
		done
	done
	;;
pads)
	read -ra pads <<<"${PADS:-0 4096 8192 12288 16384 20480 24576 28672 32768 36864 40960 45056 49152 53248 57344 61440}"
	for r in 0 1 2; do
		for pad in "${pads[@]}"; do
			for arm in ${CANDIDATES:-a f}; do
				round "$r"
				WHEEL_BENCH_PAD=$pad run single "$arm" 1000
			done
		done
	done
	;;
ab)
	old=$(cd "${2:?the old tree}" && pwd)
	rounds=${3:-40}
	IFS=';' read -ra variants <<<"${AB_VARIANTS:-=}"
	arms=()
	for v in "${variants[@]}"; do
		tag=${v%%=*}
		read -ra flags <<<"${v#*=}"
		echo "variant tag=$tag flags=\"${flags[*]}\" old=$(git -C "$old" rev-parse --short=12 HEAD 2>/dev/null || echo "$old")"
		bins[old$tag]="$build/timing-old$tag"
		bins[new$tag]="$build/timing-new$tag"
		compile "${bins[old$tag]}" "$old" "${flags[@]}"
		compile "${bins[new$tag]}" "$root" "${flags[@]}"
		arms+=("w.old$tag" "w.old$tag.2" "w.new$tag" "a.old$tag" "a.new$tag")
	done
	for ((r = 0; r < rounds; ++r)); do
		for ((k = 0; k < ${#arms[@]}; ++k)); do
			round "$r"
			run_ab "${arms[$(((k + r) % ${#arms[@]}))]}" "${N:-1000}"
		done
	done
	echo "end=$(date -u +%FT%TZ)"
	for v in "${variants[@]}"; do
		tag=${v%%=*}
		echo "== variant '$tag': w.new$tag against w.old$tag (twin w.old$tag.2); a.new$tag against a.old$tag (layout alone)"
		python3 "$here/wheel_bench_summary.py" --ref "w.old$tag" --twin "w.old$tag.2" --arms "w.old$tag,w.old$tag.2,w.new$tag" "$log"
		python3 "$here/wheel_bench_summary.py" --ref "a.old$tag" --twin none --arms "a.old$tag,a.new$tag" "$log" | grep '/a\.'
	done
	exit 0
	;;
*)
	echo "unknown mode $mode" >&2
	exit 2
	;;
esac
echo "end=$(date -u +%FT%TZ)"
python3 "$here/wheel_bench_summary.py" ${WHEEL_BENCH_GATE:+--gate "$WHEEL_BENCH_GATE"} "$log"
