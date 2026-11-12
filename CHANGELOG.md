# Changelog

All notable changes to `wheel` are recorded here, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Unreleased

### Added

- `wheel.hh`: `wheel::timer_entry` and `wheel::timer_wheel`, a single-owner hierarchical timer
  wheel (4,096 x 1 ms and six 64-slot upper levels) with a 64-stripe intrusive inbox for arms
  from other threads, moved from Kronuz/task's timers (task:5a1e516).
- Tests: a reference model (`test/model.hh`) run from a generator and from a libFuzzer target
  (`fuzz/fuzz_ops.cc`), far deadlines up to `never`, arms from 1, 4 and 14 threads, the owner
  assertions, a counting-allocator test.
- `bench/`: Detent's wheel-evidence harness with the library beside the prototype and (f).
- `examples/demo.cc`, `docs/timer-wheel.md`.
