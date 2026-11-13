# wheel::timer_wheel

The one timer wheel of the Kronuz libraries (Detent PLAN.md D70, D72): a single-owner
hierarchical wheel with a 64-stripe intrusive inbox for arms from other threads, 32-byte
intrusive entries and no allocation per arm. Kronuz/task, Kronuz/logger and Xapiand run their
timers on it.

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
  due fires on the next call. `expire` never touches an entry after its fire, so a fire may also
  destroy its own entry. An entry fires at most one tick after its deadline and never before.
- **next()** is when `expire` next has work: the end of the next occupied 1 ms slot, or the start
  of the next occupied upper slot's span; 0 while the inbox holds arms; `never` when empty. A
  loop arms one timer at it.
- **Destruction** moves what is still armed or queued to a detached list and calls each one's
  `fire(e, false)` once. A `false` fire means the wheel is dying: the entry is dead, so its
  owner marks it so and never arms or cancels it on this wheel again. An arm from such a fire
  is undone (unlinked, unfired), so destruction always ends. A dying fire never calls `expire()`,
  which would overwrite the detached list (Debug-asserted).
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
   push that finds its stripe empty sets the stripe's bit, after its CAS. Each thread takes a stripe
   on first use; past 64 threads stripes are shared, so a push can land behind one whose bit is
   not set yet. Waking an owner that sleeps is the caller's part. A thread
   that is not the owner never re-arms, cancels or reads an entry it handed over until the owner
   gives it back (its fire, say).
2. **Only the owner arms, re-arms, cancels and expires.** The first such call binds the owner
   thread and every later one is asserted in Debug; `release()` unbinds it once the loop is
   stopped, so teardown's thread may destroy the wheel. An owner `arm` or `cancel` of an unarmed
   entry that may wait in the inbox files the inbox first, so an entry another thread just armed
   is re-armed or cancelled exactly:
   - an entry pushed behind another has a `next`, and its bit may still be pending in the other
     push's thread, so the owner files every stripe that holds entries;
   - an entry at the bottom of its stripe has no `next`, and its own push set the bit before
     `arm_remote` returned, which happens before the owner acts on it, so the bits find it.

   `expire` and `next()` see the inbox only through the bits, so an entry pushed behind a push
   whose bit is still pending waits for that bit: until the other thread runs its next
   instruction and its caller wakes the owner. Only shared stripes (past 64 threads) see this
   delay; the owner's arm and cancel of the entry are exact regardless (`wheel_test shared`).

## Cost

Detent's wheel-vs-stash comparison (`docs/perf/wheel-vs-stash.md` in Kronuz/Detent) ran this
library (wheel:bfd72bc) and Kronuz/stash (stash:b7d8876) in one build on macOS x86_64 (i9-9980HK,
Apple clang 17, `-O2 -DNDEBUG`). Every cell is a median over interleaved rounds, each arm in its
own process, from `exclusive` jobs; every timing run delivered each entry once and none early. A
row says whether the two do the same work ("same work"); stash's number appears only where they
do, or beside a row the text calls out. A ratio is a median of per-round ratios with an interval
from order statistics, and holds only when it lies outside the A/A band of stash against itself.

### One thread: arm, re-arm, cancel, expire

One repetition arms every entry, re-arms it, cancels it, arms it again, then expires in 1 ms
steps until all fired, each pass in one random order. ns per entry operation, manual clock.

| n | operation | wheel | stash | same work |
|---|---|---|---|---|
| 1,000 | arm | 6.22 | 134.23 | yes |
| 1,000 | re-arm | 8.86 | | no |
| 1,000 | cancel | 3.69 | 0.66 | no |
| 1,000 | expire | 23.19 | | no |
| 1,000 | whole life (the four, summed) | 42.00 | 1,110.37 | yes |
| 1,000,000 | arm | 140.93 | 179.94 | yes |
| 1,000,000 | re-arm | 127.31 | | no |
| 1,000,000 | cancel | 28.77 | 5.89 | no |
| 1,000,000 | expire | 107.26 | | no |
| 1,000,000 | whole life | 407.14 | 1,230.51 | yes |

- **Whole life is the row to quote.** Stash's re-arm files the entry again and leaves the first
  filing stale, its cancel clears a flag in the entry, and its `expire` pays for every stale
  filing it walks; the wheel moves, unlinks and fires only live entries. The wheel's cancel is a
  real unlink, 3.7 ns against 0.66 ns at 1,000 entries and 28.8 ns against 5.9 ns at 1,000,000,
  where stash defers the work to `expire`.
- At 1,000 entries everything is in cache and an arm is 6 to 9 ns; at 1,000,000 both are bound by
  cache misses on entries in random order.

### Arm from another thread

P producer threads arm 1,000,000 entries at now plus 1 to 1,000 ms (`arm_remote` for the wheel)
while one owner fires them; ns per arm on the producer, same work.

| P | wheel | stash |
|---|---|---|
| 1 | 14.77 | 148.20 |
| 4 | 12.12 | 198.91 |
| 14 | 31.76 | 306.60 |

The row is the producer's cost; the owner's cost of filing what the inbox holds is not in it.

### Fire lateness of a cross-thread burst

The worst fire after its deadline in that run, median over rounds (the minimum to the maximum
over 40 rounds), in ms: the wheel 0.20 (0.14 to 0.94) at P = 1, 3.42 (2.44 to 4.83) at P = 4 and
4.46 (2.66 to 5.52) at P = 14; stash 1.02 at each P, its tick.

The lateness is the owner's longest loop iteration, not a tick: one `expire` that files and fires
a backlog of 0.3 to 0.9 million entries at about 9 ns each (4 to 5 ms), after the producers have
finished (`max_late_idle_ms` carries it; `max_late_pushing_ms` is 0 to 1.8 ms at P = 4 and 14).
A burst of arms at about 290 million a second (P = 4) outruns the owner's filing at about 110
million a second, so the inbox holds a backlog that drains in one pass. It is inherent to
handing over through an inbox, so it is documented, not fixed. The same run with the producers
paced to stash's rate (100 to 300 ns an arm) has no backlog: the worst fire is 0.21 ms at P = 1
and 4, and 0.2 to 0.9 ms at P = 14 (job 261112-160910-a7d660, 30 rounds, medians; unpaced
P = 1 0.21 ms, P = 4 3.48 ms, P = 14 4.08 ms). Stash, which pays for each arm on the producer
at 150 to 300 ns, has no such backlog: its 1.02 ms is its tick, so for a burst faster than
about 110 million arms a second stash fires late less.

### Sparse wakes

One wake of an event loop that sleeps until `next()` and jumps far ahead (24 h horizon, 1,000,000
entries, about one entry a wake) walked up to 4,096 empty slots bit by bit; `expire` now walks the
occupied slots by bitmap word, at most 65 words a wake (`wheel_test sparse` counts them). Paired
against the old walk in one build, exclusive, 24 rounds (job 261112-160906-c85f23): 201.6 ns a
wake (177.5 to 231.8) against 264.4 ns (234.4 to 289.3), a ratio of 0.773 (0.724 to 0.801),
outside the A/A band of 3.1%. The dense one-thread row did not move (job 261112-160908-f37dd5, 40
rounds: score 0.988, 0.940 to 1.039, inside its 3.4% band). A wake still costs about 200 ns where
an entry a millisecond apart costs nothing extra; the small-N row where Detent measured stash
ahead was not rerun against stash here.

### Fire-and-forget keys over 24 hours

P producers file N keys uniformly over a 24 h horizon (no cancel, no re-arm), then one consumer
drains them in key order, on a manual clock. Insert is the producer's median ns per key, same work.

| N | P | wheel insert | stash insert |
|---|---|---|---|
| 1,000,000 | 1 | 10.4 | 746.3 |
| 1,000,000 | 4 | 10.0 | 859.9 |
| 1,000,000 | 14 | 33.4 | 1,811.3 |
| 1,000 | 1 | 12.9 | 660.8 |
| 1,000 | 4 | 29.3 | 1,200.8 |
| 1,000 | 14 | 174.6 | 7,221.5 |

The wheel's consumer does what an event loop does (expire, sleep until `next()`, expire again),
and stash's drains with one walk to the horizon's end, so their drain times are not the same work.
Insert plus drain per key at 1,000,000 keys: the wheel 211 to 239 ns, stash 2,237 to 3,315 ns.

### Memory

| Row | wheel | stash | same work |
|---|---|---|---|
| entry, bytes | 32 | 16 | no: the entry types differ |
| bytes per entry at n = 1,000 (entry and heap) | 97.55 | 269.25 | yes |
| bytes per entry at n = 1,000,000 | 32.07 | 43.53 | yes |
| heap per armed entry, n = 1,000 | 0 | 244.00 | yes |
| heap per armed entry, n = 1,000,000 | 0 | 27.52 | yes |
| 24 h keys, N = 1,000,000: bytes per key | 32.07 | 382.21 | yes |
| structure after construction | 65,552 | 9,248 | yes |

The wheel allocates its 65,552 bytes at construction and nothing per entry; a process with a
handful of timers pays that block. Stash grows as entries arrive.

### Reproducing

`bench/wheel_bench.sh single|cross|late|ab` (and `AB_ROW=wake` for `ab`) run the library's own
rows; the paired statistics are `bench/wheel_bench_summary.py`'s (a ratio is taken inside a
round). The comparison with stash is Detent's `docs/design/async-drivers-bench`.
