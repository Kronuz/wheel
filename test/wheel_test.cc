// wheel::timer_wheel:
//   reentry   a fire cancels or re-arms another due entry, or itself
//   model     random arm, remote arm, cancel and re-arm, from fires too, against the reference
//             model (test/model.hh), with deadlines up to `never` and clock jumps of years
//   far       deadlines beyond a round, up to `never`: never early, at most a tick late, at most
//             one move per level and one early wake per level; a million far entries leave
//             level 0 empty and cost no tick a walk
//   cross     arm_remote of unarmed entries from 1, 4 and 14 threads while the owner expires:
//             each fires once, never early
//   drop      a destroyed wheel runs fire(e, false) for what is armed or queued
//   owner     (Debug) a cancel, a re-arm and an expire from another thread abort
#include "model.hh"
#include "wheel.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

#ifndef NDEBUG
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
			std::exit(1);                                                            \
		}                                                                            \
	} while (0)

namespace {
using wheel::timer_entry;
using wheel::timer_wheel;
using u64 = std::uint64_t;
constexpr u64 ms = 1'000'000;
constexpr u64 day = 86'400'000 * ms;

void reentry() {
	for (const std::string_view mode : {"cancel", "rearm", "self", "arm-due"}) {
		timer_wheel w(0);
		timer_entry a, b, c;
		w.arm(a, 500'000);  // both due in slot 0; b is the list head, a follows
		w.arm(b, 600'000);
		int fired_a = 0, fired_b = 0, fired_c = 0;
		auto fire = [&](timer_entry& e) {
			if (&e == &b) {
				++fired_b;
				if (mode == "cancel") { w.cancel(a); }
				if (mode == "rearm") { w.arm(a, 50 * ms); }
				if (mode == "self" && fired_b == 1) { w.arm(b, 1 * ms); }  // due now: fires on the next expire
				if (mode == "arm-due") { w.arm(c, 0); }
			} else if (&e == &a) {
				++fired_a;
			} else {
				++fired_c;
			}
		};
		const auto first = w.expire(1 * ms, fire);
		if (mode == "cancel") { CHECK(first == 1 && fired_a == 0 && !a.armed() && w.size() == 0); }
		if (mode == "rearm") {
			CHECK(first == 1 && fired_a == 0 && a.armed() && w.size() == 1);
			CHECK(w.expire(49 * ms, fire) == 0);
			CHECK(w.expire(50 * ms, fire) == 1 && fired_a == 1);
		}
		if (mode == "self") {
			CHECK(first == 2 && fired_b == 1 && b.armed());
			CHECK(w.expire(1 * ms, fire) == 1 && fired_b == 2 && w.size() == 0);
		}
		if (mode == "arm-due") {
			CHECK(first == 2 && fired_c == 0 && c.armed());
			CHECK(w.expire(1 * ms, fire) == 1 && fired_c == 1);
		}
		std::printf("reentry mode=%.*s ok\n", int(mode.size()), mode.data());
	}
}

struct random_source {
	std::mt19937_64 random;
	u64 draw(u64 bound) { return bound ? random() % bound : 0; }
};

void model() {
	wheel_model::counts total;
	for (u64 seed = 0; seed < 8; ++seed) {
		random_source src{std::mt19937_64(20261111 + seed)};
		wheel_model::harness h(seed % 2 ? 64 : 512, 7'000 * ms + seed * 977 * ms);
		for (int k = 0; k < 200'000; ++k) { h.step(src); }
		h.destroy();
		const auto& s = h.stats;
		total.arms += s.arms;
		total.remote_arms += s.remote_arms;
		total.cancels += s.cancels;
		total.expires += s.expires;
		total.fires += s.fires;
		total.from_fires += s.from_fires;
		total.far += s.far;
		total.drops += s.drops;
	}
	std::printf("model seeds=8 arms=%llu remote_arms=%llu cancels=%llu expires=%llu fires=%llu from_fires=%llu far_arms=%llu drops=%llu ok\n",
		static_cast<unsigned long long>(total.arms), static_cast<unsigned long long>(total.remote_arms),
		static_cast<unsigned long long>(total.cancels), static_cast<unsigned long long>(total.expires),
		static_cast<unsigned long long>(total.fires), static_cast<unsigned long long>(total.from_fires),
		static_cast<unsigned long long>(total.far), static_cast<unsigned long long>(total.drops));
	CHECK(total.fires > 100'000 && total.far > 10'000 && total.from_fires > 10'000 && total.remote_arms > 10'000 && total.drops > 0);
}

// One entry, the loop waking at next(): it fires in its own tick, never before, after at most one
// early wake and one move per level. A lone entry's pprev points at its slot's head, so each
// change of pprev before it fires is one move.
void far_deadlines() {
	for (const u64 d : {4'095 * ms, 4'096 * ms, 10'000 * ms, 30'000 * ms, 300'000 * ms, day, 30 * day, 1000 * day,
			 100 * 365 * day, u64{1} << 63, timer_wheel::never - 1, timer_wheel::never}) {
		for (const u64 start : {u64{0}, 123'456 * ms, u64{1} << 62}) {
			if (d <= start) { continue; }
			timer_wheel w(start);
			timer_entry e;
			bool fired = false;
			u64 fired_at = 0, wakes = 0, moves = 0;
			w.arm(e, d);
			timer_entry** at = e.pprev;
			for (u64 now = start; !fired;) {
				const u64 wake = w.next();
				CHECK(wake > now && (wake != timer_wheel::never || d / timer_wheel::tick == timer_wheel::never / timer_wheel::tick));  // the last tick ends at never
				now = wake;
				++wakes;
				w.expire(now, [&](timer_entry& x) {
					CHECK(x.deadline <= now);
					fired = true;
					fired_at = now;
				});
				if (!fired && e.pprev != at) {
					++moves;
					at = e.pprev;
				}
				CHECK(wakes <= timer_wheel::levels + 1);
			}
			CHECK(fired_at >= d && fired_at - d <= timer_wheel::tick);
			CHECK(moves <= timer_wheel::levels);
			std::printf("far start_ms=%llu deadline_ms=%llu wakes=%llu moves=%llu late_ns=%llu ok\n", static_cast<unsigned long long>(start / ms),
				static_cast<unsigned long long>(d / ms), static_cast<unsigned long long>(wakes), static_cast<unsigned long long>(moves),
				static_cast<unsigned long long>(fired_at - d));
		}
	}
	// Beyond any level: across jumps of days to millennia nothing fires early, and from 63 days
	// before `never` each daily expire fires exactly the one entry it reaches.
	{
		timer_wheel w(0);
		std::vector<timer_entry> es(64);
		for (std::size_t i = 0; i < es.size(); ++i) { w.arm(es[i], timer_wheel::never - i * day); }
		std::size_t fired = 0;
		auto count = [&](timer_entry& e) {
			CHECK(e.deadline <= timer_wheel::never - (63 - fired) * day);
			++fired;
		};
		for (u64 now = day; now < (timer_wheel::never - 64 * day) / 2; now += now / 2) { CHECK(w.expire(now, count) == 0); }
		CHECK(w.expire(timer_wheel::never - 64 * day, count) == 0 && w.size() == es.size());
		for (u64 k = 64; k-- > 0;) { CHECK(w.expire(timer_wheel::never - k * day, count) == 1); }
		CHECK(fired == es.size() && w.size() == 0);
		std::printf("far beyond entries=64 fired=%zu ok\n", fired);
	}
	// A million entries a day out stay above level 0: the next wake is the span of their level-2
	// slot, not a 1 ms slot, and a run of 1 ms ticks walks none of them.
	constexpr std::size_t n = 1'000'000;
	std::vector<timer_entry> entries(n);
	std::mt19937_64 random(24);
	timer_wheel w(0);
	const auto arm_start = std::chrono::steady_clock::now();
	for (auto& e : entries) { w.arm(e, day + random() % (60'000 * ms)); }
	const auto arms = std::chrono::steady_clock::now() - arm_start;
	CHECK(w.next() > timer_wheel::slots * ms);
	const auto start = std::chrono::steady_clock::now();
	std::size_t fired = 0;
	for (u64 t = 0; t < 100'000 * ms; t += ms) { fired += w.expire(t); }
	const auto carry = std::chrono::steady_clock::now() - start;
	CHECK(fired == 0 && w.size() == n);
	std::size_t total = 0;
	for (u64 t = day; t <= day + 61'000 * ms; t += ms) {
		total += w.expire(t, [&](timer_entry& e) { CHECK(e.deadline <= t); });
	}
	CHECK(total == n && w.size() == 0);
	std::printf("far carry entries=%zu arm_us=%lld ticks=100000 us=%lld ok\n", n,
		static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(arms).count()),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(carry).count()));
}

// Unarmed entries armed from P threads while the owner expires: each fires once, never early.
void cross() {
	for (const std::size_t p : {1, 4, 14}) {
		constexpr std::size_t per = 20'000;
		std::vector<timer_entry> entries(p * per);
		std::vector<int> fires(entries.size(), 0);
		timer_wheel w(0);
		std::atomic<u64> clock{0};
		std::atomic<std::size_t> done{0};
		std::vector<std::thread> threads;
		for (std::size_t k = 0; k < p; ++k) {
			threads.emplace_back([&, k] {
				std::mt19937_64 random(k);
				for (std::size_t i = 0; i < per; ++i) {
					const u64 at = clock.load(std::memory_order_relaxed) + random() % (200 * ms) + (i % 97 == 0 ? 10'000 * ms : 0);
					w.arm_remote(entries[k * per + i], at);
				}
				done.fetch_add(1);
			});
		}
		std::size_t fired = 0;
		u64 now = 0, stop = timer_wheel::never;
		while (fired < entries.size() && now <= stop) {
			now += ms / 2;
			clock.store(now, std::memory_order_relaxed);
			fired += w.expire(now, [&](timer_entry& e) {
				CHECK(e.deadline <= now);
				++fires[static_cast<std::size_t>(&e - entries.data())];
			});
			// Every arm read the clock before `done` counted it: none is due after this.
			if (stop == timer_wheel::never && done.load() == p) { stop = now + 10'200 * ms + ms; }
		}
		for (auto& t : threads) { t.join(); }
		std::size_t none = 0, twice = 0;
		for (int f : fires) {
			none += f == 0;
			twice += f > 1;
		}
		if (none || twice) { std::fprintf(stderr, "cross producers=%zu fired=%zu never_fired=%zu twice=%zu now_ms=%llu\n", p, fired, none, twice, static_cast<unsigned long long>(now / ms)); }
		CHECK(none == 0 && twice == 0);
		CHECK(w.size() == 0 && w.next() == timer_wheel::never);
		std::printf("cross producers=%zu arms=%zu ok\n", p, entries.size());
	}
}

int drops = 0;
void count_drop(timer_entry& e, bool expired) noexcept {
	CHECK(!expired && !e.armed());
	++drops;
}

void drop() {
	std::vector<timer_entry> es(5);
	for (auto& e : es) { e.fire = &count_drop; }
	{
		timer_wheel w(0);
		w.arm(es[0], 5 * ms);           // level 0
		w.arm(es[1], 30 * day);         // an upper level
		w.arm(es[2], 6 * ms);
		w.cancel(es[2]);                // cancelled: not dropped
		w.arm_remote(es[3], 7 * ms);    // queued
		CHECK(w.next() == 0);
	}
	CHECK(drops == 3 && !es[0].armed() && !es[1].armed() && !es[3].armed());
	std::printf("drop armed=2 queued=1 dropped=%d ok\n", drops);
}

#ifndef NDEBUG
// Binds the wheel to this thread, then runs `op` on another; the child must die on SIGABRT.
template <class Op>
bool aborts_off_owner(Op op) {
	std::fflush(stdout);
	const pid_t pid = fork();
	if (pid == 0) {
		timer_wheel w(0);
		timer_entry e;
		w.arm(e, 5 * ms);
		std::thread([&] { op(w, e); }).join();
		std::_Exit(0);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
void owner() {
	CHECK(aborts_off_owner([](timer_wheel& w, timer_entry& e) { w.cancel(e); }));
	CHECK(aborts_off_owner([](timer_wheel& w, timer_entry& e) { w.arm(e, 9 * ms); }));
	CHECK(aborts_off_owner([](timer_wheel& w, timer_entry&) { w.expire(1 * ms); }));
	// release() unbinds: another thread may then take the wheel over.
	CHECK(!aborts_off_owner([](timer_wheel& w, timer_entry& e) {
		(void)w;
		(void)e;
	}));
	{
		timer_wheel w(0);
		timer_entry e;
		w.arm(e, 5 * ms);
		w.release();
		std::thread([&] {
			w.cancel(e);
			w.release();
		}).join();
		CHECK(!e.armed());
	}
	std::printf("owner cancel, re-arm and expire off the owner abort; release hands over ok\n");
}
#endif
}  // namespace

int main(int argc, char** argv) {
	const std::string_view only = argc > 1 ? argv[1] : "all";
	if (only == "all" || only == "reentry") { reentry(); }
	if (only == "all" || only == "model") { model(); }
	if (only == "all" || only == "far") { far_deadlines(); }
	if (only == "all" || only == "cross") { cross(); }
	if (only == "all" || only == "drop") { drop(); }
#ifndef NDEBUG
	if (only == "all" || only == "owner") { owner(); }
#endif
	std::printf("wheel_test: ok\n");
	return 0;
}
