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
- `fuzz/corpus/` and `wheel_fuzz_replay`, which replays it under every compiler.
- `examples/demo.cc`, `docs/timer-wheel.md`.

### Changed (from task's copy)

- `next()` saturates at `never` for an entry due in the last millisecond before it, where task's
  copy wrapped past `UINT64_MAX` to a time already passed.
- An owner `arm` or `cancel` of an entry pushed behind another on a shared stripe (past 64
  threads) files every stripe first, since the earlier push may not have set its bit yet; task's
  copy read an empty inbox, fired a cancelled entry and linked an armed one while still queued.
- Destruction fires each dying entry once from a detached list and undoes what those fires
  arm, where task's copy re-read the bitmaps and looped forever on a `false` fire that re-armed.
- `arm` and `cancel` are always inlined, and the inbox drain and upper-level filing are out of
  line, so an operation is no call at all; clang had left arm, cancel and file out of line. `arm`
  files a level-0 deadline inline and sends a far one, or an inbox to file first, to one call.
