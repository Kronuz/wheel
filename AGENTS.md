# AGENTS.md

Working notes for agents modifying this repository. For the design read `ARCHITECTURE.md` and
`docs/timer-wheel.md`; for usage read `README.md`. This file covers the repo layout, how to build
and test, the invariants you must not break, and the traps that are easy to fall into.

## Repo map

```
wheel.hh                 The whole library: timer_entry, timer_wheel.
test/model.hh            The reference model: the wheel beside a map of deadlines, driven by
                         one source of choices, checked after every expire and at destruction.
test/wheel_test.cc       reentry, model (random), far, cross (1, 4, 14 threads), drop, owner.
test/wheel_alloc_test.cc A counting operator new: no allocation per operation.
fuzz/fuzz_ops.cc         libFuzzer: the model driven by the input bytes.
fuzz/replay.cc           Replays fuzz/corpus/ (inputs the fuzzer found) under every compiler.
examples/demo.cc         A runnable tour (also a CTest).
bench/                   Detent's wheel-evidence harness: the prototype (a), (f) and this
                         library (w); wheel_bench.sh runs the rows, wheel_bench_summary.py
                         prints medians, A/A bands and ratios.
docs/timer-wheel.md      Shape, contracts, far deadlines, cost.
CMakeLists.txt           INTERFACE target `wheel` (+ alias wheel::wheel), C++20; CTest tests
                         wheel, wheel_alloc, wheel_fuzz_corpus, wheel_demo; -DWHEEL_FUZZ=ON
                         for wheel_fuzz.
```

## Build and test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build
```

Debug keeps the owner assertions live, and `wheel_test owner` checks them (it forks). CI
(Kronuz/cpp-libraries' cmake workflow) builds RelWithDebInfo without `NDEBUG`, plus ASan+UBSan
and TSan. The fuzz target needs a clang with libFuzzer (Homebrew LLVM on macOS):

```sh
cmake -B build-fuzz -DWHEEL_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ && cmake --build build-fuzz --target wheel_fuzz
./build-fuzz/wheel_fuzz -max_total_time=600 -max_len=4096
```

The bench rows: `bench/wheel_bench.sh single 10` and `bench/wheel_bench.sh cross 5`, on a quiet
machine (an exclusive hold); `WHEEL_BENCH_GATE=w` exits 1 when the library misses 1.10x.

## Conventions

- C++20, header-only, standard library only.
- Tabs for indentation, double quotes in code, no em dashes in prose.
- MIT-licensed; keep the copyright header (Copyright (c) 2026 Germán Méndez Bravo) on
  `wheel.hh`.
- Every operation is `noexcept` except `expire` with a caller's callable.

## Load-bearing invariants

- **No allocation per operation.** `wheel_alloc_test` counts it; an entry is 32 bytes and holds
  everything a filing needs.
- **Fires never early.** Level 0 fires an entry only when `deadline <= now`; a far entry moves
  down when the cursor enters its span. Removing the cascade loses far entries (the model test
  catches it).
- **Only the owner touches the slots.** `owner_check()` asserts it in Debug; `arm_remote` writes
  only the inbox. Don't let another thread read `pprev` or `next` of an entry it handed over.
- **drain() files with `file()`, not `arm()`:** an `arm()` of an unarmed entry drains, so producers
  refilling the inbox would nest drains without bound.
- **arm and cancel inline; drain and the upper-level filing do not.** A call per operation was
  the whole macOS gap to the prototype (1.36-1.50x at n = 1,000): clang left arm, cancel and file
  out of line while drain's loop sat inside them. arm files a level-0 deadline itself and has one
  call, `arm_slow`, for a far deadline or an inbox to file first. Keep the hot paths free of loops;
  check with `nm -C` on the bench build that no `timer_wheel::arm`, `cancel` or `file` symbol is
  left.
- **expire() is not re-entrant**, and the due list is detached before any fire, so a fire may
  cancel or re-arm anything.

## Traps

- `next()` returns 0 while remote arms wait: a loop must expire, not sleep. It also returns
  `never` while entries are armed when all of them are due in the last millisecond before
  `never` (that tick ends there); check `size()`, not `next() == never`, for emptiness.
- A deadline already passed is filed in the cursor's slot and fires at the next `expire`, not at
  once.
- `arm_remote` of an entry that is armed or still queued corrupts both lists; the assertion
  catches only the armed case. The caller owns that state.
- Past 64 producer threads, stripes are shared: still correct, more contention.
- Benchmarks on this machine move with code and data placement: compare arms in one interleaved
  run with its A/A pair, never across runs (`bench/wheel_bench.sh` does).
