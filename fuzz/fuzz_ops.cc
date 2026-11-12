// libFuzzer target: the input chooses a sequence of arms, remote arms, cancels, re-arms and
// expires (fires that mutate entries included) on one wheel, checked against the reference
// model of test/model.hh after every expire and at the wheel's destruction.
#include "../test/model.hh"

#include <cstddef>
#include <cstdint>

namespace {
// Draws little-endian bytes from the input, as many as the bound needs; zeros once it runs out.
struct byte_source {
	const std::uint8_t* p;
	const std::uint8_t* end;
	std::uint64_t draw(std::uint64_t bound) {
		if (bound <= 1) { return 0; }
		std::uint64_t v = 0;
		for (std::uint64_t span = bound - 1, shift = 0; span; span >>= 8, shift += 8) {
			v |= std::uint64_t{p < end ? *p++ : std::uint8_t{0}} << shift;
		}
		return v % bound;
	}
	bool done() const { return p >= end; }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	if (size < 2) { return 0; }
	byte_source src{data, data + size};
	wheel_model::harness h(1 + src.draw(64), src.draw(wheel_model::u64{1} << 40) * wheel_model::ms);
	while (!src.done()) { h.step(src); }
	h.destroy();
	return 0;
}
