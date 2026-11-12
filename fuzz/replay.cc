// Runs fuzz_ops.cc's entry point over the files it is given (the checked-in corpus), so every
// compiler in CI replays the inputs the fuzzer found.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
	for (int i = 1; i < argc; ++i) {
		std::ifstream in(argv[i], std::ios::binary);
		const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
		if (!in.eof() && in.fail()) {
			std::fprintf(stderr, "cannot read %s\n", argv[i]);
			return 1;
		}
		LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
		std::printf("replayed %s (%zu bytes)\n", argv[i], bytes.size());
	}
	std::printf("wheel_fuzz_replay: ok\n");
	return 0;
}
