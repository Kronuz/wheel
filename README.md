# wheel

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/wheel/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/wheel/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Header-only hierarchical timer wheel for C++20. One owner thread arms, re-arms, cancels and
fires intrusive 32-byte entries in O(1) with no allocation; other threads arm unarmed entries
through a 64-stripe intrusive inbox the owner drains. Deadlines are nanoseconds on the owner's
clock, steady or manual, up to `UINT64_MAX`. Reach for it for an event loop's timeouts,
heartbeats, debouncers and deadlines, where re-arm and cancel are as frequent as arm.

## ✨ Highlights

- **No allocation, ever:** the entry lives inside its owner (`next`, `pprev`, deadline, fire
  pointer), and a remote arm's inbox link is the entry's own `next`.
- **O(1) arm, re-arm and cancel:** a 4,096 x 1 ms level and six 64-slot upper levels up to 142
  years per slot; a far deadline moves down at most once per level, and no tick walks an entry
  that is not due.
- **Fires never early, at most one tick late,** and a fire may cancel or re-arm any entry,
  itself included.
- **Cross-thread arms without a lock:** a CAS on the arming thread's own stripe; the owner
  drains only the stripes whose bits it takes.
- **`next()` for one reactor timer:** the end of the next occupied 1 ms slot, or the start of
  the next upper span; `never` when empty.
- **Clock-agnostic:** the wheel never reads a clock, so a simulator's manual clock runs days of
  timers in no time.
- **Checked against a reference model** in the tests and in a libFuzzer target, under ASan,
  UBSan and TSan.

## 🚀 Quick start

```cpp
#include "wheel.hh"

#include <cstdint>
#include <cstdio>

int main() {
	wheel::timer_wheel w(0);  // nanoseconds on the owner's clock
	wheel::timer_entry e;
	e.fire = [](wheel::timer_entry& x, bool expired) noexcept {
		if (expired) { std::printf("fired at %llu ms\n", (unsigned long long)(x.deadline / 1'000'000)); }
	};
	w.arm(e, 250'000'000);  // 250 ms
	for (std::uint64_t now = 0; w.size(); now = w.next()) { w.expire(now); }  // sleep until next()
	// fired at 250 ms
}
```

[`examples/demo.cc`](examples/demo.cc) adds a timer that re-arms itself, a deadline two days
out and an arm from another thread.

## 📦 Installation

Requires C++20 (`<bit>`) and CMake 3.14 or newer; no other dependencies.

```cmake
include(FetchContent)
FetchContent_Declare(wheel
  GIT_REPOSITORY https://github.com/Kronuz/wheel.git
  GIT_TAG        main)
FetchContent_MakeAvailable(wheel)

target_link_libraries(my_app PRIVATE wheel::wheel)
```

Link `wheel::wheel` and include `wheel.hh`, or copy `wheel.hh` onto the include path.

## 🧵 Threading contracts

1. Only the owner thread arms, re-arms, cancels and expires; the first such call binds it
   (asserted in Debug), and `release()` unbinds it for teardown.
2. Any thread may `arm_remote()` an unarmed entry; the owner files it at its next `expire()`,
   and only the owner re-arms or cancels it after that. Waking a sleeping owner is the caller's
   part.

[`docs/timer-wheel.md`](docs/timer-wheel.md) has the shape, far deadlines and the measured
cost; [`ARCHITECTURE.md`](ARCHITECTURE.md) the reasons behind them.

## 🎯 When to use it

- One event loop's timers: socket timeouts, retries, heartbeats, election timers, debouncers.
- Arms from worker threads into that loop, many producers at once.

Not for:

- Timers shared by several loops with no owner: give each loop its own wheel.
- A cross-thread cancel or re-arm: post it to the owner, or keep a cancelled flag the fire
  checks.

## ⚡ Performance

Pending the A1 rows (`docs/timer-wheel.md`, Cost).

## License

MIT; see [LICENSE](LICENSE).
