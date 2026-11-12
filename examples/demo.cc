// A runnable tour (not a test): a loop that owns a wheel on a manual clock, timers that re-arm
// themselves from their fire, a far deadline, and arms from another thread through the inbox.
#include "wheel.hh"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {
using u64 = std::uint64_t;
constexpr u64 ms = 1'000'000;

// An object that owns its entry: arming allocates nothing.
struct heartbeat {
	wheel::timer_entry entry;
	wheel::timer_wheel* w = nullptr;
	int beats = 0;
	static void fire(wheel::timer_entry& e, bool expired) noexcept {
		if (!expired) { return; }  // the wheel was destroyed with it armed
		auto& self = *reinterpret_cast<heartbeat*>(reinterpret_cast<char*>(&e) - offsetof(heartbeat, entry));
		std::printf("  heartbeat %d at %llu ms\n", ++self.beats, static_cast<unsigned long long>(e.deadline / ms));
		if (self.beats < 3) { self.w->arm(e, e.deadline + 150 * ms); }  // a fire may re-arm itself
	}
};
}  // namespace

int main() {
	wheel::timer_wheel w(0);  // nanoseconds on the owner's clock; here a counter we move
	u64 now = 0;

	heartbeat hb;
	hb.w = &w;
	hb.entry.fire = &heartbeat::fire;
	w.arm(hb.entry, 100 * ms);

	wheel::timer_entry far;  // two days out: waits in an upper level, moves down as time passes
	far.fire = [](wheel::timer_entry& e, bool expired) noexcept {
		if (expired) { std::printf("  far deadline at %llu s\n", static_cast<unsigned long long>(e.deadline / (1000 * ms))); }
	};
	w.arm(far, 172'800'000 * ms);

	wheel::timer_entry remote;  // armed by another thread while unarmed; the owner files it
	remote.fire = [](wheel::timer_entry& e, bool expired) noexcept {
		if (expired) { std::printf("  remote arm fired at %llu ms\n", static_cast<unsigned long long>(e.deadline / ms)); }
	};
	std::thread([&] { w.arm_remote(remote, 250 * ms); }).join();

	// The loop: sleep until next() (a reactor would arm one timer there), then expire.
	std::printf("loop:\n");
	for (int wakes = 0; w.size() || w.next() != wheel::timer_wheel::never; ++wakes) {
		const u64 wake = w.next();
		if (wake > now) { now = wake; }
		const std::size_t fired = w.expire(now);
		if (fired == 0 && wakes < 12) {
			std::printf("  woke at %llu ms, nothing due (remote arms filed, or a far entry moved down)\n", static_cast<unsigned long long>(now / ms));
		}
	}
	std::printf("done: %d heartbeats, wheel empty\n", hb.beats);
	return 0;
}
