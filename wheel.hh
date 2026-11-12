/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// A single-owner hierarchical timer wheel with intrusive 32-byte entries and a 64-stripe inbox
// for arms from other threads. Header-only, standard library only. See docs/timer-wheel.md.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace wheel {

// What a timer_wheel files: 32 bytes inside its owner, so arming allocates nothing.
struct timer_entry {
	timer_entry* next = nullptr;    // the slot's list; the inbox's while another thread's arm waits
	timer_entry** pprev = nullptr;  // the pointer that points here; null while unarmed
	std::uint64_t deadline = 0;     // nanoseconds on the wheel's clock
	// What expire() runs at the deadline (true), or a destroyed wheel for an entry still armed (false).
	void (*fire)(timer_entry&, bool expired) noexcept = nullptr;
	bool armed() const noexcept { return pprev != nullptr; }
};

// A single-owner hierarchical timer wheel: 4,096 slots of 1 ms, six upper levels of 64 slots
// each 64 times wider (4.1 s, 4.4 min, 4.7 h, 12.4 days, 2.2 years, 142 years), occupancy bitmaps
// for next(), and a 64-stripe inbox for arms from other threads. Deadlines are nanoseconds on
// the owner's clock (steady or manual). An arm is O(1); a far deadline waits in an upper level
// and moves down when the cursor enters its slot's span, at most once per level, so it fires at
// most one tick late, never early, and no tick walks entries that are not due.
// Two threading contracts:
//   - the owner thread arms, re-arms, cancels and expires; the first such call binds it
//     (Debug-asserted), and release() unbinds it once the loop is stopped and closed;
//   - any thread may arm_remote() an unarmed entry, which the owner files at its next expire()
//     (or arm or cancel of that entry); only the owner re-arms or cancels it after that.
class timer_wheel {
public:
	static constexpr std::uint64_t tick = 1'000'000;  // 1 ms
	static constexpr std::size_t slots = 4096, mask = slots - 1, levels = 6, level_slots = 64, stripes = 64;
	static constexpr std::uint64_t never = ~std::uint64_t{0};

	explicit timer_wheel(std::uint64_t now = 0) noexcept : cursor_(now / tick) {}
	timer_wheel(const timer_wheel&) = delete;
	timer_wheel& operator=(const timer_wheel&) = delete;
	// Unlinks what is still armed or queued and runs its fire(e, false).
	~timer_wheel() {
		owner_check();
		drain();
		for (std::size_t w = 0; w < words; ++w) {
			while (std::uint64_t bits = bits_[w]) {
				timer_entry* e = head_[w * 64 + static_cast<std::size_t>(std::countr_zero(bits))];
				unlink(*e);
				if (e->fire) { e->fire(*e, false); }
			}
		}
	}

	// Unbinds the owner thread, so another thread may become the owner (teardown's).
	void release() noexcept {
#ifndef NDEBUG
		owner_ = std::thread::id{};
#endif
	}

	// Owner: files e at `deadline`, unlinking it first if armed.
	void arm(timer_entry& e, std::uint64_t deadline) noexcept {
		owner_check();
		if (e.armed()) {
			unlink(e);
		} else if (queued()) {  // e may still wait in the inbox
			drain();
			if (e.armed()) { unlink(e); }
		}
		e.deadline = deadline;
		file(e);
	}
	// Owner: unlinks e if armed; a no-op otherwise.
	void cancel(timer_entry& e) noexcept {
		owner_check();
		if (!e.armed() && queued()) { drain(); }
		if (e.armed()) { unlink(e); }
	}
	// Any thread, an unarmed entry: `next` links its stripe until the owner files it. The owner
	// learns of it at its next expire(); waking an owner that sleeps is the caller's part.
	void arm_remote(timer_entry& e, std::uint64_t deadline) noexcept {
		assert(!e.armed() && "another thread arms only an unarmed entry");
		e.deadline = deadline;
		const unsigned s = stripe();
		auto& head = inbox_[s].head;
		timer_entry* first = head.load(std::memory_order_relaxed);
		do {
			e.next = first;
		} while (!head.compare_exchange_weak(first, &e, std::memory_order_release, std::memory_order_relaxed));
		if (!first) { queued_.fetch_or(std::uint64_t{1} << s, std::memory_order_release); }
	}

	// Owner: fires every entry due at `now`, at most one tick after its deadline and never before.
	// The due entries move to a detached list first, so a fire may cancel or re-arm any entry,
	// itself included; one armed during the fires for a time already due fires on the next call.
	template <typename F>
	std::size_t expire(std::uint64_t now, F&& fire) {
		owner_check();
		assert(!expiring_ && "expire() is not re-entrant");
		drain();
		const std::uint64_t last = now / tick;
		if (last < cursor_) { return 0; }
		const std::uint64_t first = cursor_;
		cursor_ = last;
		if ((first >> 12) != (last >> 12)) { cascade(first, last); }
		const std::uint64_t steps = last - first + 1 > slots ? slots : last - first + 1;
		timer_entry** tail = &due_;  // the due list keeps slot order
		for (std::uint64_t k = 0; k < steps; ++k) {
			const std::size_t s = (first + k) & mask;
			if (!(bits_[s / 64] & (std::uint64_t{1} << (s % 64)))) { continue; }
			for (timer_entry* e = head_[s]; e;) {
				timer_entry* following = e->next;
				if (e->deadline <= now) {
					unlink(*e);
					e->next = nullptr;
					*tail = e;
					e->pprev = tail;
					tail = &e->next;
					++count_;
				}
				e = following;
			}
		}
		expiring_ = true;
		std::size_t fired = 0;
		while (timer_entry* e = due_) {
			unlink(*e);
			++fired;
			fire(*e);
		}
		expiring_ = false;
		return fired;
	}
	std::size_t expire(std::uint64_t now) {
		return expire(now, [](timer_entry& e) { e.fire(e, true); });
	}

	// Owner: when expire() next has work: the end of the next occupied 1 ms slot, or the start of
	// the next occupied upper slot's span (its entries move down then); 0 while other threads'
	// arms wait to be filed (expire() files them); `never` when empty.
	std::uint64_t next() const noexcept {
		if (queued()) { return 0; }
		if (!count_) { return never; }
		std::uint64_t best = never;
		const std::size_t start = cursor_ & mask;  // level 0 holds ticks [cursor_, cursor_ + slots)
		for (std::size_t k = 0; k <= slots / 64; ++k) {
			const std::size_t w = (start / 64 + k) % (slots / 64);
			std::uint64_t word = bits_[w];
			if (k == 0) { word &= ~std::uint64_t{0} << (start % 64); }
			if (word) {
				const std::size_t s = w * 64 + static_cast<std::size_t>(std::countr_zero(word));
				best = (cursor_ + ((s - start) & mask) + 1) * tick;
				break;
			}
		}
		unsigned shift = 12;
		for (std::size_t level = 0; level < levels; ++level, shift += 6) {
			const std::uint64_t word = bits_[slots / 64 + level];
			if (!word) { continue; }
			const std::uint64_t from = (cursor_ >> shift) + 1;  // upper entries lie in (cursor's slot, +64)
			const std::uint64_t span = from + static_cast<std::uint64_t>(std::countr_zero(std::rotr(word, static_cast<int>(from & 63))));
			best = std::min(best, (span << shift) * tick);
		}
		return best;
	}
	// Owner: the entries filed (not those still in the inbox).
	std::size_t size() const noexcept { return count_; }

private:
	static constexpr std::size_t all_slots = slots + levels * level_slots, words = all_slots / 64;

	struct alignas(64) stripe_head {
		std::atomic<timer_entry*> head{nullptr};
	};

	// Each thread pushes on its own stripe, by first use; past 64 threads stripes are shared.
	static unsigned stripe() noexcept {
		static std::atomic<unsigned> seq{0};
		static thread_local const unsigned mine = seq.fetch_add(1, std::memory_order_relaxed) % stripes;
		return mine;
	}
	bool queued() const noexcept { return queued_.load(std::memory_order_relaxed) != 0; }
	// Files every entry other threads queued, from the stripes whose bits it takes.
	void drain() noexcept {
		if (!queued()) { return; }
		for (std::uint64_t bits = queued_.exchange(0, std::memory_order_acquire); bits; bits &= bits - 1) {
			auto& head = inbox_[static_cast<std::size_t>(std::countr_zero(bits))].head;
			for (timer_entry* e = head.exchange(nullptr, std::memory_order_acquire); e;) {
				timer_entry* following = e->next;
				e->next = nullptr;
				file(*e);  // unarmed: an arm() here would drain again while producers refill
				e = following;
			}
		}
	}
	// Links an unarmed e by its deadline: in level 0 within a round of the cursor (a deadline
	// already passed goes in the cursor's slot), else in the lowest upper level whose 64 slots
	// reach it, in a slot after the cursor's.
	void file(timer_entry& e) noexcept {
		const std::uint64_t t = e.deadline / tick;
		std::size_t s;
		if (t < cursor_ + slots) {
			s = (t < cursor_ ? cursor_ : t) & mask;
		} else {
			unsigned shift = 12;
			std::size_t level = 0;
			while (level + 1 < levels && (t >> shift) - (cursor_ >> shift) >= level_slots) {
				++level;
				shift += 6;
			}
			s = slots + level * level_slots + ((t >> shift) & (level_slots - 1));
		}
		link(e, head_[s]);
		bits_[s / 64] |= std::uint64_t{1} << (s % 64);
	}
	// The cursor moved from `first` to `last`: every upper slot whose span it entered files its
	// entries again, now relative to `last`, so they move to lower levels.
	void cascade(std::uint64_t first, std::uint64_t last) noexcept {
		unsigned shift = 12;
		for (std::size_t level = 0; level < levels; ++level, shift += 6) {
			const std::uint64_t from = first >> shift, to = last >> shift;
			if (from == to) { return; }  // and so for every level above
			const std::uint64_t n = to - from < level_slots ? to - from : level_slots;
			for (std::uint64_t j = 1; j <= n; ++j) {
				const std::size_t s = slots + level * level_slots + ((from + j) & (level_slots - 1));
				while (timer_entry* e = head_[s]) {
					unlink(*e);
					file(*e);
				}
			}
		}
	}
	void owner_check() noexcept {
#ifndef NDEBUG
		if (owner_ == std::thread::id{}) { owner_ = std::this_thread::get_id(); }
		assert(owner_ == std::this_thread::get_id() && "a wheel is armed, cancelled and expired only on its owner's thread");
#endif
	}
	void link(timer_entry& e, timer_entry*& head) noexcept {
		e.next = head;
		if (e.next) { e.next->pprev = &e.next; }
		head = &e;
		e.pprev = &head;
		++count_;
	}
	void unlink(timer_entry& e) noexcept {
		*e.pprev = e.next;
		if (e.next) { e.next->pprev = e.pprev; }
		auto** first = head_.data();
		if (e.pprev >= first && e.pprev < first + all_slots && !*e.pprev) {
			const std::size_t s = static_cast<std::size_t>(e.pprev - first);
			bits_[s / 64] &= ~(std::uint64_t{1} << (s % 64));
		}
		e.next = nullptr;
		e.pprev = nullptr;
		--count_;
	}

	std::array<timer_entry*, all_slots> head_{};  // level 0's 4,096, then 64 per upper level
	std::array<std::uint64_t, words> bits_{};
	timer_entry* due_ = nullptr;
	std::uint64_t cursor_;  // the tick expire() last reached
	std::size_t count_ = 0;
	bool expiring_ = false;
#ifndef NDEBUG
	std::thread::id owner_{};
#endif
	alignas(64) std::atomic<std::uint64_t> queued_{0};  // a bit per stripe that holds entries
	std::array<stripe_head, stripes> inbox_{};
};

}  // namespace wheel
