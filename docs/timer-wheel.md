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

Pending the rows.
