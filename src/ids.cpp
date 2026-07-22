#include "plume/ids.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>

namespace plume {

namespace {

// base36 keeps ids short and still lexicographically ordered for a fixed width.
std::string base36(std::uint64_t v, int width) {
	static constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	std::string out(static_cast<std::size_t>(width), '0');
	for (int i = width - 1; i >= 0 && v > 0; --i) {
		out[static_cast<std::size_t>(i)] = digits[v % 36];
		v /= 36;
	}
	return out;
}

std::uint32_t random_tail() {
	static thread_local std::mt19937 rng{std::random_device{}()};
	return rng();
}

}  // namespace

std::string new_id(std::string_view prefix) {
	using namespace std::chrono;
	static std::atomic<std::uint32_t> counter{0};

	const auto ms = static_cast<std::uint64_t>(
	    duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
	const std::uint32_t seq = counter.fetch_add(1, std::memory_order_relaxed);

	std::string out;
	out.reserve(prefix.size() + 20);
	out.append(prefix);
	out.push_back('_');
	out.append(base36(ms, 9));               // ~ good until year 5000
	out.append(base36(seq & 0xffffffU, 4));  // dedupe within a millisecond
	out.append(base36(random_tail() & 0xffffffU, 4));
	return out;
}

}  // namespace plume
