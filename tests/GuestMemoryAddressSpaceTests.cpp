#include "kernel/guest_memory/guestMemoryAddressSpace.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::LibKernel::Memory::GuestMemory;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "GuestMemoryAddressSpaceTests: %s\n", message);
		std::abort();
	}
}

void CheckStatus(GuestMemoryStatus actual, GuestMemoryStatus expected, const char* message) {
	if (actual != expected) {
		std::fprintf(stderr, "GuestMemoryAddressSpaceTests: %s (status %u, expected %u)\n",
		             message, static_cast<unsigned>(actual), static_cast<unsigned>(expected));
		std::abort();
	}
}

} // namespace

int main() {
	// The test passes an explicit 4 KiB guest page size. This is a test fixture,
	// not a claim about the host page size or Kyty's final PS5 page-size setting.
	constexpr GuestMemorySize GuestPage = 0x1000;
	constexpr GuestVirtualAddress GuestBase = 0x15b1400000ull;
	constexpr GuestVirtualAddress AliasBase = GuestBase + 0x4000;
	constexpr GuestMemorySize BackingSize = 3 * GuestPage;
	GuestMemoryModel memory(GuestPage);

	Check(memory.GetGuestPageSize() == GuestPage, "guest page size was not retained");
	GuestBackingId backing {};
	CheckStatus(memory.Allocate(BackingSize, GuestPage, &backing), GuestMemoryStatus::Success,
	            "backing allocation failed");
	Check(static_cast<bool>(backing), "backing ID is empty");

	CheckStatus(memory.Reserve(GuestBase, 0x8000), GuestMemoryStatus::Success,
	            "guest reservation failed");
	CheckStatus(memory.Reserve(GuestBase, GuestPage), GuestMemoryStatus::AddressAlreadyMapped,
	            "overlapping reservation was accepted");
	CheckStatus(memory.Map(GuestBase, backing, 0, BackingSize,
	                       GuestMemoryPermission::CpuRead | GuestMemoryPermission::CpuWrite |
	                           GuestMemoryPermission::GpuRead),
	            GuestMemoryStatus::Success, "primary mapping failed");
	CheckStatus(memory.Map(AliasBase, backing, 0, BackingSize,
	                       GuestMemoryPermission::CpuRead | GuestMemoryPermission::GpuRead),
	            GuestMemoryStatus::Success, "alias mapping failed");

	const uint64_t marker = 0x4b59545948414452ull;
	CheckStatus(memory.Write(GuestBase + 0x118, &marker, sizeof(marker)),
	            GuestMemoryStatus::Success, "write through primary mapping failed");
	uint64_t alias_value = 0;
	CheckStatus(memory.Read(AliasBase + 0x118, &alias_value, sizeof(alias_value)),
	            GuestMemoryStatus::Success, "read through alias failed");
	Check(alias_value == marker, "alias did not observe the same backing bytes");

	CheckStatus(memory.Write(AliasBase + 0x118, &marker, sizeof(marker)),
	            GuestMemoryStatus::PermissionDenied, "read-only alias allowed a CPU write");
	GuestMemoryTranslation denied_translation {};
	CheckStatus(memory.Translate(AliasBase + 0x118, sizeof(marker),
	                             GuestMemoryPermission::CpuExecute, &denied_translation),
	            GuestMemoryStatus::PermissionDenied, "alias allowed CPU execution without permission");
	CheckStatus(memory.Translate(AliasBase + 0x118, sizeof(marker),
	                             GuestMemoryPermission::GpuWrite, &denied_translation),
	            GuestMemoryStatus::PermissionDenied, "alias allowed GPU writes without permission");

	// Two adjacent virtual mappings to consecutive offsets in one backing object
	// must behave as one contiguous guest byte range.
	constexpr GuestVirtualAddress AdjacentBase = GuestBase + 0x10000;
	CheckStatus(memory.Reserve(AdjacentBase, 2 * GuestPage), GuestMemoryStatus::Success,
	            "adjacent-range reservation failed");
	CheckStatus(memory.Map(AdjacentBase, backing, 0, GuestPage,
	                       GuestMemoryPermission::CpuRead | GuestMemoryPermission::CpuWrite),
	            GuestMemoryStatus::Success, "first adjacent mapping failed");
	CheckStatus(memory.Map(AdjacentBase + GuestPage, backing, GuestPage, GuestPage,
	                       GuestMemoryPermission::CpuRead | GuestMemoryPermission::CpuWrite),
	            GuestMemoryStatus::Success, "second adjacent mapping failed");
	const std::array<uint8_t, 8> boundary_bytes {9, 8, 7, 6, 5, 4, 3, 2};
	std::array<uint8_t, 8> boundary_result {};
	CheckStatus(memory.Write(AdjacentBase + GuestPage - 4, boundary_bytes.data(),
	                         boundary_bytes.size()),
	            GuestMemoryStatus::Success, "write across adjacent mappings failed");
	CheckStatus(memory.Read(AdjacentBase + GuestPage - 4, boundary_result.data(),
	                        boundary_result.size()),
	            GuestMemoryStatus::Success, "read across adjacent mappings failed");
	Check(boundary_result == boundary_bytes, "adjacent mapping read-back differs");
	GuestMemoryTranslation adjacent_translation {};
	CheckStatus(memory.Translate(AdjacentBase + GuestPage - 4, boundary_bytes.size(),
	                             GuestMemoryPermission::CpuRead, &adjacent_translation),
	            GuestMemoryStatus::Success, "translation did not span compatible adjacent maps");
	Check(adjacent_translation.contiguous_size >= boundary_bytes.size(),
	      "adjacent translation did not combine contiguous backing offsets");
	CheckStatus(memory.Unmap(AdjacentBase, GuestPage), GuestMemoryStatus::Success,
	            "first adjacent mapping cleanup failed");
	CheckStatus(memory.Unmap(AdjacentBase + GuestPage, GuestPage), GuestMemoryStatus::Success,
	            "second adjacent mapping cleanup failed");
	CheckStatus(memory.ReleaseReservation(AdjacentBase, 2 * GuestPage),
	            GuestMemoryStatus::Success, "adjacent-range reservation cleanup failed");

	GuestMemoryTranslation translation {};
	CheckStatus(memory.Translate(AliasBase + 0x118, sizeof(marker),
	                             GuestMemoryPermission::GpuRead, &translation),
	            GuestMemoryStatus::Success, "GPU-read translation failed");
	Check(translation.backing == backing && translation.backing_offset == 0x118,
	      "translation returned the wrong backing or offset");
	Check(translation.contiguous_size >= sizeof(marker), "translation span is too short");
	Check(translation.generation == memory.GetGeneration(),
	      "translation generation does not match current mapping generation");

	CheckStatus(memory.Protect(GuestBase + GuestPage, GuestPage,
	                           GuestMemoryPermission::CpuRead),
	            GuestMemoryStatus::Success, "partial protect failed");
	const uint8_t byte = 0x5a;
	CheckStatus(memory.Write(GuestBase + GuestPage, &byte, sizeof(byte)),
	            GuestMemoryStatus::PermissionDenied, "protected page allowed a CPU write");
	CheckStatus(memory.Write(GuestBase + 2 * GuestPage, &byte, sizeof(byte)),
	            GuestMemoryStatus::Success, "protect changed a neighbouring page");
	Check(memory.GetGeneration() != translation.generation,
	      "mapping generation did not change after protection update");

	CheckStatus(memory.Unmap(GuestBase + GuestPage, GuestPage), GuestMemoryStatus::Success,
	            "partial unmap failed");
	CheckStatus(memory.Read(GuestBase + GuestPage, &alias_value, sizeof(alias_value)),
	            GuestMemoryStatus::AddressNotMapped, "unmapped guest page remained readable");
	CheckStatus(memory.Write(GuestBase, &marker, sizeof(marker)), GuestMemoryStatus::Success,
	            "left mapping fragment was lost after partial unmap");
	CheckStatus(memory.Write(GuestBase + 2 * GuestPage, &marker, sizeof(marker)),
	            GuestMemoryStatus::Success, "right mapping fragment was lost after partial unmap");

	CheckStatus(memory.Release(backing), GuestMemoryStatus::BackingInUse,
	            "backing was released while aliases still referenced it");
	CheckStatus(memory.Unmap(AliasBase, BackingSize), GuestMemoryStatus::Success,
	            "alias unmap failed");
	CheckStatus(memory.Unmap(GuestBase, GuestPage), GuestMemoryStatus::Success,
	            "left fragment unmap failed");
	CheckStatus(memory.Unmap(GuestBase + 2 * GuestPage, GuestPage), GuestMemoryStatus::Success,
	            "right fragment unmap failed");
	CheckStatus(memory.ReleaseReservation(GuestBase, 0x8000), GuestMemoryStatus::Success,
	            "reservation release failed");
	CheckStatus(memory.Release(backing), GuestMemoryStatus::Success, "backing release failed");

	// A write crossing an unmapped guest page is rejected before any part is copied.
	GuestBackingId second_backing {};
	CheckStatus(memory.Allocate(2 * GuestPage, GuestPage, &second_backing),
	            GuestMemoryStatus::Success, "second backing allocation failed");
	CheckStatus(memory.Reserve(GuestBase, 3 * GuestPage), GuestMemoryStatus::Success,
	            "second reservation failed");
	const auto read_write = GuestMemoryPermission::CpuRead | GuestMemoryPermission::CpuWrite;
	CheckStatus(memory.Map(GuestBase, second_backing, 0, GuestPage, read_write),
	            GuestMemoryStatus::Success, "first page mapping failed");
	CheckStatus(memory.Map(GuestBase + 2 * GuestPage, second_backing, GuestPage, GuestPage,
	                       read_write),
	            GuestMemoryStatus::Success, "third page mapping failed");
	const uint32_t sentinel = 0xaabbccdd;
	CheckStatus(memory.Write(GuestBase + GuestPage - sizeof(sentinel), &sentinel, sizeof(sentinel)),
	            GuestMemoryStatus::Success, "sentinel write failed");
	const std::array<uint8_t, 8> across_gap {1, 2, 3, 4, 5, 6, 7, 8};
	CheckStatus(memory.Write(GuestBase + GuestPage - 4, across_gap.data(), across_gap.size()),
	            GuestMemoryStatus::AddressNotMapped, "write across unmapped gap was accepted");
	uint32_t sentinel_after = 0;
	CheckStatus(memory.Read(GuestBase + GuestPage - sizeof(sentinel), &sentinel_after,
	                        sizeof(sentinel_after)),
	            GuestMemoryStatus::Success, "sentinel read failed");
	Check(sentinel_after == sentinel, "failed cross-gap write partially modified backing");

	CheckStatus(memory.Unmap(GuestBase, GuestPage), GuestMemoryStatus::Success,
	            "first test mapping cleanup failed");
	CheckStatus(memory.Unmap(GuestBase + 2 * GuestPage, GuestPage), GuestMemoryStatus::Success,
	            "third test mapping cleanup failed");
	CheckStatus(memory.ReleaseReservation(GuestBase, 3 * GuestPage), GuestMemoryStatus::Success,
	            "second reservation cleanup failed");
	CheckStatus(memory.Release(second_backing), GuestMemoryStatus::Success,
	            "second backing cleanup failed");

	GuestMemoryModel invalid_page_size(3000);
	CheckStatus(invalid_page_size.Reserve(GuestBase, GuestPage), GuestMemoryStatus::InvalidAlignment,
	            "non-power-of-two guest page size was accepted");

	std::puts("GuestMemoryAddressSpaceTests: all tests passed");
	return 0;
}
