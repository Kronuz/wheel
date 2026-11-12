# wheel::timer_wheel

The one timer wheel of the Kronuz libraries (Detent PLAN.md D70, D72): a single-owner
hierarchical wheel with a 64-stripe intrusive inbox for arms from other threads, 32-byte
intrusive entries and no allocation per arm. It is candidate (f) of Detent's
`docs/design/wheel-evidence.md` with upper levels for far deadlines; Kronuz/task, Kronuz/logger
and Xapiand run their timers on it.

## Shape

```
entry (32 B, inside its owner): next | pprev | deadline (ns) | fire(entry&, bool expired)

level 0   4,096 slots x 1 ms        ticks [cursor, cursor + 4,096)
upper 1      64 slots x 4.1 s       \
...                                   one 64-bit occupancy word per upper level;
upper 6      64 slots x 142 years   /  every head in one array, so unlink is one shape
inbox     64 stripes, a cache line each, and one word of stripe bits
```

- **Arm** is O(1): an entry within a round of the cursor goes in its 1 ms slot (one already due
  goes in the cursor's slot); a farther one goes in the lowest upper level whose 64 slots reach
  it, in a slot after the cursor's. Choosing the level is at most six compares. Cancel unlinks
  through `pprev`; the slot's bit clears when it empties.
- **expire(now)** first files what other threads queued, then moves the cursor. Each upper slot
  whose span the cursor entered files its entries again, relative to the new cursor, so they move
  down. The due level-0 entries move to a detached list before any fire runs, so a fire may
  cancel or re-arm any entry, itself included; an entry armed during the fires for a time already
  due fires on the next call. An entry fires at most one tick after its deadline and never before.
- **next()** is when `expire` next has work: the end of the next occupied 1 ms slot, or the start
  of the next occupied upper slot's span; 0 while the inbox holds arms; `never` when empty. A
  loop arms one timer at it.
- **Destruction** unlinks what is still armed or queued and calls its `fire(e, false)`.
- **The clock is the owner's:** nanoseconds on `steady_clock` for a reactor, a manual counter for
  a simulator. The wheel never reads a clock.

## Far deadlines

A deadline more than a round (4.096 s) ahead waits in an upper level. When the cursor enters
the span of its slot, `expire` files it again relative to the new cursor, which puts it at least
one level lower; so an entry moves at most once per level (six times in its life), and a far
deadline costs `next()` at most one early wake per level it crosses. No tick walks an entry that
is not due: level 0 holds only the current round, and a run of 1 ms ticks over a million entries
a day out walks none of them (`wheel_test far`).

A `std::uint64_t` of nanoseconds spans under 2^45 ms, five of the top level's 64 slots of 2^42 ms,
so every deadline up to `never` has a slot of its own and none aliases one the cursor reaches first;
`wheel_test far` arms deadlines up to `never` from cursors as late as 2^62 ns.

## Threading contracts

1. **Other threads arm only an unarmed entry**, with `arm_remote(e, deadline)`: it links the entry
   into the thread's stripe through `e.next` (a CAS on that stripe's head, no allocation), and the
   push that finds its stripe empty sets the stripe's bit. Each thread takes a stripe on first use;
   past 64 threads stripes are shared. Waking an owner that sleeps is the caller's part. A thread
   that is not the owner never re-arms, cancels or reads an entry it handed over until the owner
   gives it back (its fire, say).
2. **Only the owner arms, re-arms, cancels and expires.** The first such call binds the owner
   thread and every later one is asserted in Debug; `release()` unbinds it once the loop is
   stopped, so teardown's thread may destroy the wheel. An owner `arm` or `cancel` of an unarmed
   entry files the inbox first when it holds anything, so an entry another thread just armed is
   re-armed or cancelled exactly.

## Cost (A1)

Measured with `bench/wheel_bench.sh` (Detent's wheel-evidence harness), exclusive jobs on
macOS x86_64 (i9-9980HK, Apple clang 17, `-O2`), at wheel:1cd7e7a. Each cell is the median of
R runs, in ns per operation. Candidate (a) is the owner-only prototype, (f) is (a) with the
inbox, and (w) is this library. The A/A band is how far apart two identical runs of (a) land.

The harness calls each candidate through a one-line wrapper (`cand_x::arm`). Detent decision D75
gates A1 on the build that forces those wrappers inline for every candidate alike
(`WHEEL_BENCH_CXXFLAGS=-DWHEEL_BENCH_FORWARD=[[gnu::always_inline]]`), and reports the default
build next to it.

**Single loop, wrappers inlined (the A1 row), R = 10, job 261111-234624-f5cc8e:**

| n | | arm | re-arm | cancel | expire | score | w / a | A/A |
|---|---|---|---|---|---|---|---|---|
| 1,000 | a | 5.31 | 11.79 | 4.98 | 20.04 | 42.14 | | 23.7% |
| 1,000 | w | 5.07 | 10.41 | 4.73 | 24.02 | 44.22 | 1.049x | |
| 1,000,000 | a | 106.97 | 106.48 | 30.75 | 106.30 | 349.24 | | 21.1% |
| 1,000,000 | w | 101.97 | 96.73 | 28.17 | 97.91 | 322.67 | 0.924x | |

**Single loop, default build, R = 10, job 261111-233420-9245bb:**

| n | | arm | re-arm | cancel | expire | score | w / a | A/A |
|---|---|---|---|---|---|---|---|---|
| 1,000 | a | 5.02 | 7.93 | 4.97 | 20.34 | 38.30 | | 10.5% |
| 1,000 | w | 5.69 | 8.46 | 5.00 | 22.70 | 41.84 | 1.092x | |
| 1,000,000 | a | 102.77 | 101.39 | 29.42 | 98.89 | 325.41 | | 26.9% |
| 1,000,000 | w | 133.55 | 117.89 | 28.38 | 98.94 | 378.06 | 1.162x | |

**Cross-thread, R = 5, P producers arming 1,000,000 entries, producer ns per arm.** Every run
fired each entry once, none early (valid).

| P | f | w | w / f | A/A | job |
|---|---|---|---|---|---|
| 1 | 11.24 | 12.65 | 1.125x | 35.1% | 261111-233422-3a6c0b |
| 4 | 11.32 | 11.11 | 0.981x | 15.4% | 261111-233422-3a6c0b |
| 14 | 31.99 | 32.62 | 1.020x | 17.4% | 261111-233422-3a6c0b |
| 1 | 12.48 | 12.55 | 1.006x | 24.2% | 261111-221548-4b10b6 (wheel:94a51c6) |
| 4 | 11.30 | 11.69 | 1.035x | 23.6% | 261111-221548-4b10b6 |
| 14 | 34.49 | 32.32 | 0.937x | 38.1% | 261111-221548-4b10b6 |

wheel:94a51c6 and wheel:1cd7e7a run the same `arm_remote`, `drain` and `expire`; only the
owner's `arm` differs between them.

**What each feature (a) lacks costs:**

- **The inbox test and the far-deadline branch in `arm`:** nothing measurable once inlined. With
  the wrappers inlined, w's arm, re-arm and cancel sit at or below (a)'s (5.07 vs 5.31 ns at
  n = 1,000).
- **Their code size:** clang's inline cost for the harness's `cand_w::arm` is 285. For
  `cand_a::arm` it is 205, and the threshold at `-O2` is 225 (job 261111-233446-2a29ab). So in
  the default build, w's wrapper stays a call. That call costs arm +0.67 ns and re-arm +0.53 ns
  at n = 1,000, and arm +31 ns and re-arm +16 ns at n = 1,000,000; this is the whole default-build
  gap. The cost is the inbox test, the far branch and `arm`'s one call to `arm_slow`. A caller
  that calls `timer_wheel::arm` directly inlines it, since `arm` is always inlined.
- **expire's residue:** +1.2 to +4.3 ns per entry at n = 1,000 across the probes (+3.98 in the
  A1 row). It disappears at n = 1,000,000 (97.91 vs 106.30). Per call, `expire` does work (a)
  doesn't: it tests `queued()` and whether the cursor crossed into an upper slot. Neither has been
  shown to be the cause, which is untraced.
- **Cross-thread arm:** the same CAS as (f); within the A/A band at every P.
