#include "kernel/guestMemoryMap.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

using Libs::LibKernel::Memory::GuestMemoryMap;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "GuestMemoryMapTests: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	constexpr uint64_t GuestBase = 0x15b1400000ull;
	constexpr size_t GuestSize = 4 * GuestMemoryMap::PageSize;
	GuestMemoryMap memory;

	Check(memory.Map(GuestBase, GuestSize), "could not map the simulated PS5 guest range");
	Check(memory.MappingCount() == 1, "unexpected mapping count after Map");
	Check(memory.IsMapped(GuestBase, GuestSize), "new mapping is not reported as mapped");
	Check(!memory.Map(GuestBase + GuestMemoryMap::PageSize, GuestMemoryMap::PageSize),
	      "overlapping mapping was accepted");
	Check(!memory.Map(GuestBase + 1, GuestMemoryMap::PageSize),
	      "unaligned guest address was accepted");
	Check(!memory.Map(GuestBase, GuestMemoryMap::PageSize + 1),
	      "unaligned mapping size was accepted");
	Check(!memory.Map(std::numeric_limits<uint64_t>::max() - GuestMemoryMap::PageSize + 1,
	                  2 * GuestMemoryMap::PageSize),
	      "overflowing guest range was accepted");

	constexpr uint64_t Value = 0x4b59545950534d45ull;
	uint64_t actual = 0;
	Check(memory.Write(GuestBase + 0x20, &Value, sizeof(Value)), "write to guest memory failed");
	Check(memory.Read(GuestBase + 0x20, &actual, sizeof(actual)), "read from guest memory failed");
	Check(actual == Value, "read-back value differs from the written value");

	// This access deliberately straddles a 16 KiB page boundary.
	const std::array<uint8_t, 8> across_page {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87};
	std::array<uint8_t, 8> page_result {};
	const auto page_boundary_address = GuestBase + GuestMemoryMap::PageSize - 4;
	Check(memory.Write(page_boundary_address, across_page.data(), across_page.size()),
	      "cross-page write failed");
	Check(memory.Read(page_boundary_address, page_result.data(), page_result.size()),
	      "cross-page read failed");
	Check(page_result == across_page, "cross-page read-back data differs");

	// Two adjacent guest mappings may be accessed as one continuous range.
	const auto second_region = GuestBase + GuestSize;
	Check(memory.Map(second_region, GuestMemoryMap::PageSize), "adjacent mapping failed");
	const std::array<uint8_t, 8> across_mappings {1, 3, 5, 7, 9, 11, 13, 15};
	std::array<uint8_t, 8> mapping_result {};
	const auto mapping_boundary_address = second_region - 4;
	Check(memory.Write(mapping_boundary_address, across_mappings.data(), across_mappings.size()),
	      "write spanning adjacent mappings failed");
	Check(memory.Read(mapping_boundary_address, mapping_result.data(), mapping_result.size()),
	      "read spanning adjacent mappings failed");
	Check(mapping_result == across_mappings, "adjacent-mapping read-back data differs");

	// A failed write spanning a hole must not modify the mapped bytes before the hole.
	const auto gap_start = second_region + GuestMemoryMap::PageSize;
	Check(memory.Map(gap_start + GuestMemoryMap::PageSize, GuestMemoryMap::PageSize),
	      "mapping after the test gap failed");
	const uint32_t sentinel = 0xaabbccdd;
	Check(memory.Write(second_region, &sentinel, sizeof(sentinel)), "sentinel setup failed");
	const uint64_t spanning_gap = second_region + GuestMemoryMap::PageSize - 4;
	Check(!memory.Write(spanning_gap, across_mappings.data(), across_mappings.size()),
	      "write spanning an unmapped gap unexpectedly succeeded");
	uint32_t sentinel_after = 0;
	Check(memory.Read(second_region, &sentinel_after, sizeof(sentinel_after)),
	      "could not read sentinel after failed write");
	Check(sentinel_after == sentinel, "failed write partially modified mapped memory");

	Check(memory.Unmap(GuestBase), "unmapping the first guest region failed");
	Check(!memory.IsMapped(GuestBase, 1), "unmapped address is still reported as mapped");
	Check(!memory.Read(GuestBase, &actual, sizeof(actual)), "read from unmapped memory succeeded");
	Check(!memory.Write(GuestBase, &Value, sizeof(Value)), "write to unmapped memory succeeded");
	Check(!memory.Unmap(GuestBase), "unmapping the same region twice unexpectedly succeeded");
	Check(memory.MappingCount() == 2, "unexpected mapping count after unmap");

	// Zero-byte operations are harmless no-ops and do not require a backing pointer.
	Check(memory.Read(0, nullptr, 0), "zero-byte read should be a no-op");
	Check(memory.Write(0, nullptr, 0), "zero-byte write should be a no-op");

	std::puts("GuestMemoryMapTests: all tests passed");
	return 0;
}
