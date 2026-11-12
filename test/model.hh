// The reference model of wheel::timer_wheel, shared by test/wheel_test.cc and the libFuzzer
// target: a map of each entry's deadline beside the wheel, driven by one source of choices.
// Every operation runs on the wheel and on the map, and after each expire the two must agree:
//   - an expire fires exactly the entries the model holds due when it began, each once, with
//     the deadline last armed, except those a fire cancelled or re-armed first;
//   - size() counts the filed entries, next() is 0 while remote arms wait, `never` when empty,
//     else later than now and no later than the end of the earliest deadline's tick;
//   - destroying the wheel runs fire(e, false) once for each entry still armed or queued.
// Choices come from `Source::draw(bound)`, a value in [0, bound); the random test draws from a
// generator, the fuzz target from its input bytes.
#pragma once

#include "wheel.hh"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace wheel_model {

using u64 = std::uint64_t;
constexpr u64 ms = 1'000'000;
constexpr u64 day = 86'400'000 * ms;
constexpr u64 never = wheel::timer_wheel::never;

#define MODEL_CHECK(c)                                                               \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
			std::abort();                                                            \
		}                                                                            \
	} while (0)

struct counts {
	u64 arms = 0, remote_arms = 0, cancels = 0, expires = 0, fires = 0, from_fires = 0, far = 0, drops = 0;
};

class harness {
public:
	explicit harness(std::size_t n, u64 start) : entries_(n), now_(start), w_(std::make_unique<wheel::timer_wheel>(start)) {
		for (auto& e : entries_) { e.fire = &on_fire; }
	}
	~harness() {
		if (current == this) { current = nullptr; }
	}

	// One operation, chosen by the source.
	template <class Source>
	void step(Source& src) {
		const std::size_t i = src.draw(entries_.size());
		switch (src.draw(16)) {
		case 0:
		case 1:
		case 2: cancel(i); break;
		case 3:
		case 4: remote(i, deadline(src)); break;
		case 5:
		case 6:
		case 7: expire(src); break;
		default: arm(i, deadline(src)); break;
		}
	}

	// Destroys the wheel: fire(e, false) runs once for each entry still armed or queued.
	void destroy() {
		current = this;
		dropped_.clear();
		w_.reset();
		current = nullptr;
		MODEL_CHECK(dropped_.size() == armed_.size());
		for (const auto& [i, at] : armed_) {
			MODEL_CHECK(dropped_.count(i) == 1 && !entries_[i].armed());
			(void)at;
		}
		stats.drops += dropped_.size();
		armed_.clear();
		queued_.clear();
	}

	counts stats;

private:
	// A deadline relative to now: mostly within level 0, some a few rounds or days out, a few
	// already passed, a few beyond any level (up to `never`).
	template <class Source>
	u64 deadline(Source& src) {
		switch (src.draw(32)) {
		case 0: return now_ - std::min(now_, src.draw(5'000 * ms));               // passed
		case 1: return never - src.draw(2 * day);                                 // beyond any level
		case 2: return now_ + src.draw(u64{1} << 50);                             // up to 36 years
		case 3:
		case 4: return now_ + src.draw(40 * day);                                 // the upper levels
		case 5:
		case 6:
		case 7: return now_ + src.draw(600'000 * ms);                             // levels 1 and 2
		case 8:
		case 9:
		case 10: return now_ + src.draw(20'000 * ms);                             // a few rounds
		default: return now_ + src.draw(3'000 * ms);                              // level 0
		}
	}
	bool filed(std::size_t i) const { return armed_.count(i) && !queued_.count(i); }
	// An owner arm or cancel of an entry it has not filed files the inbox first.
	void before_owner_op(std::size_t i) {
		if (!filed(i)) { queued_.clear(); }
	}
	void arm(std::size_t i, u64 at) {
		before_owner_op(i);
		w_->arm(entries_[i], at);
		armed_[i] = at;
		due_.erase(i);
		late_.erase(i);
		++stats.arms;
		stats.far += at / wheel::timer_wheel::tick >= now_ / wheel::timer_wheel::tick + wheel::timer_wheel::slots;
	}
	void cancel(std::size_t i) {
		before_owner_op(i);
		w_->cancel(entries_[i]);
		armed_.erase(i);
		due_.erase(i);
		late_.erase(i);
		++stats.cancels;
	}
	// Remote arms take only an idle entry: neither filed nor queued.
	void remote(std::size_t i, u64 at) {
		if (armed_.count(i)) { return; }
		w_->arm_remote(entries_[i], at);
		armed_[i] = at;
		queued_.insert(i);
		++stats.remote_arms;
	}
	template <class Source>
	void expire(Source& src) {
		u64 step = src.draw(3 * ms);
		if (src.draw(64) == 0) { step += src.draw(2 * day); }                       // many rounds
		if (src.draw(512) == 0 && now_ < (u64{1} << 62)) { step += src.draw(u64{1} << 60); }  // years
		now_ += step;
		queued_.clear();  // expire files the inbox first
		due_.clear();
		for (const auto& [i, at] : armed_) {
			if (at <= now_) { due_.insert(i); }
		}
		current = this;
		fired_now_ = 0;
		late_.clear();
		src_draw_ = [](void* s, u64 bound) { return static_cast<Source*>(s)->draw(bound); };
		src_ = &src;
		const bool by_pointer = src.draw(2) == 0;
		const std::size_t fired = by_pointer ? w_->expire(now_) : w_->expire(now_, [this](wheel::timer_entry& e) { fired_one(e, true); });
		current = nullptr;
		MODEL_CHECK(due_.empty());
		MODEL_CHECK(fired == fired_now_);
		++stats.expires;
		check();
	}
	static void on_fire(wheel::timer_entry& e, bool expired) noexcept { current->fired_one(e, expired); }
	void fired_one(wheel::timer_entry& e, bool expired) {
		const std::size_t i = static_cast<std::size_t>(&e - entries_.data());
		if (!expired) {  // the wheel's destruction
			MODEL_CHECK(dropped_.insert(i).second);
			return;
		}
		MODEL_CHECK(due_.count(i) == 1);  // due when expire began, never fired twice
		MODEL_CHECK(!e.armed() && armed_.at(i) == e.deadline && e.deadline <= now_);
		due_.erase(i);
		armed_.erase(i);
		++fired_now_;
		++stats.fires;
		if (draw(3) == 0) {  // a fire mutates an entry, itself included
			const std::size_t other = draw(4) == 0 ? i : draw(entries_.size());
			++stats.from_fires;
			switch (draw(3)) {
			case 0: cancel(other); break;
			case 1: {
				const u64 at = now_ + draw(5 * ms);
				remote(other, at);
				if (at <= now_ && queued_.count(other)) { late_.insert(other); }  // a later fire's arm may file it
				break;
			}
			default: {
				const u64 at = now_ + draw(3'000 * ms);
				arm(other, at);
				if (at <= now_) { late_.insert(other); }  // already due: fires on the next expire
				break;
			}
			}
		}
	}
	u64 draw(u64 bound) { return src_draw_(src_, bound); }
	void check() const {
		MODEL_CHECK(w_->size() == armed_.size() - queued_.size());
		for (const auto& [i, at] : armed_) {
			MODEL_CHECK(entries_[i].deadline == at);
			MODEL_CHECK(queued_.count(i) ? !entries_[i].armed() : entries_[i].armed() && (at > now_ || late_.count(i)));
		}
		const u64 wake = w_->next();
		if (!queued_.empty()) {
			MODEL_CHECK(wake == 0);
			return;
		}
		MODEL_CHECK(armed_.empty() ? wake == never : wake > now_);
		for (const auto& [i, at] : armed_) {
			const u64 t = std::max(at, now_) / wheel::timer_wheel::tick;  // a passed one waits in the cursor's slot
			const u64 bound = t + 1 > never / wheel::timer_wheel::tick ? never : (t + 1) * wheel::timer_wheel::tick;
			MODEL_CHECK(wake <= bound);  // never wakes past a deadline's tick
		}
	}

	static inline harness* current = nullptr;
	std::vector<wheel::timer_entry> entries_;
	u64 now_;
	std::unique_ptr<wheel::timer_wheel> w_;
	std::map<std::size_t, u64> armed_;  // armed or queued: entry -> deadline
	std::set<std::size_t> queued_;      // arm_remote()d, not filed yet
	std::set<std::size_t> due_, dropped_, late_;  // late_: armed by a fire for a time already due
	std::size_t fired_now_ = 0;
	u64 (*src_draw_)(void*, u64) = nullptr;
	void* src_ = nullptr;
};

}  // namespace wheel_model
