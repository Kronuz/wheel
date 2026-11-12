# Architecture

The internals of `wheel`: why the wheel looks the way it does. For usage see `README.md`; for the
shape, contracts and cost see `docs/timer-wheel.md`; for the repo map and invariants see
`AGENTS.md`.

## Shape

A single header, nothing else:

```
  wheel.hh   timer_entry (32 bytes, intrusive), timer_wheel (levels, bitmaps, inbox)
```

No compiled translation unit, no dependency beyond the standard library (`<algorithm>`,
`<array>`, `<atomic>`, `<bit>`, `<cassert>`, `<cstddef>`, `<cstdint>`, `<thread>`).

## Why one owner

Detent's `docs/design/wheel-evidence.md` measured six candidates on one loop and from 1 to 14
producer threads. A wheel every thread may touch (Kronuz/stash) pays for that on every
operation: 3.5x to 27x slower on the loop, most of it in `expire`. A single-owner wheel touches
no atomic on the loop path; the only shared state is the inbox, and only remote arms write it.
Only the owner cancels or re-arms because those need the entry's slot, which only the owner's
thread may change; a cross-thread cancel is a flag the fire checks, or a post to the owner.

## Why the entry is intrusive and `pprev` is a pointer to a pointer

`pprev` points at whatever points to the entry: a slot head, the due list's head, or the
previous entry's `next`. Unlink is then the same three stores wherever the entry is, with no
search, and `armed()` is `pprev != nullptr`. The slot's occupancy bit clears when the unlink
leaves its head null, found by `pprev` pointing inside the head array. Every head, level 0's and
the upper levels', is in one array, so one range check covers all of them.

## Why the inbox goes through `next`

An unarmed entry's `next` is unused, so a remote arm pushes the entry itself onto a lock-free
stack: no node, no allocation, and the entry stays 32 bytes. One stack head is one contended
cache line (from four producers on, (e) of the evidence lost to stash); 64 stripes, a cache line
each, picked per thread on first use, keep producers apart, and one word of stripe bits lets the
owner skip empty stripes, so an idle inbox costs `expire` one relaxed load. The owner files a
drained entry with `file()`, never `arm()`: `arm()` of an unarmed entry drains again, and
producers refilling the inbox would nest drains without bound.

## Why levels, and why they cascade on entry

Level 0 is a round of 4,096 x 1 ms: a deadline within it is one slot, and `expire` walks only the
slots it passes. A deadline farther away goes in the lowest upper level whose 64 slots reach it.
When the cursor enters an upper slot's span, its entries are filed again relative to the new
cursor and land at least a level lower; each entry moves at most once per level. The alternative,
keeping far entries in level 0 and skipping them each round, makes a tick walk entries that are
not due (a million entries a day out would be walked every 4.1 s).

## Why expire detaches the due entries first

The due entries of the slots the cursor passes move to a detached list in slot order before any
fire runs. A fire may then cancel or re-arm any entry, the one firing included, without
invalidating the walk: a cancelled or re-armed due entry simply leaves the list. An entry armed
during the fires for a time already due goes in the cursor's slot and fires on the next call,
which bounds one `expire` to the entries due when it began.

## Why next() is a slot boundary

A reactor arms one timer at `next()`. Level 0's answer is the end of the next occupied slot, so
every entry there is due by then. An upper level's is the start of its next occupied span,
where `expire` moves those entries down: an early wake, at most one per level an entry crosses.
`next()` is 0 while the inbox holds arms, so a loop that just woke for a remote arm files it at
once.
