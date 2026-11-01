#include "kernel/guest_memory/macOSGuestAddressSpace.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Libs::LibKernel::Memory::GuestMemory::HostMemoryProtection;
using Libs::LibKernel::Memory::GuestMemory::MacOSGuestAddressSpace;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "MacOSGuestAddressSpaceTests: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	constexpr uint64_t GuestBase = 0x15b1400000ull;
	constexpr uint64_t ArenaSize = 64ull * 1024ull * 1024ull;
	MacOSGuestAddressSpace address_space;

	Check(address_space.Reserve(GuestBase, ArenaSize),
	      "guest range could not be assigned to a dynamic host arena");
	Check(address_space.IsReserved(), "address space did not report reserved state");
	Check(address_space.GuestBase() == GuestBase, "guest base differs from request");
	Check(address_space.Size() == ArenaSize, "reserved size differs from request");

	const uint64_t page_size = address_space.PageSize();
	Check(page_size != 0, "Mach host page size is zero");
	Check(page_size <= ArenaSize, "host page exceeds reserved arena");
	Check(address_space.HostBase() != 0, "host arena base is null");

	void* translated_base = nullptr;
	Check(address_space.TranslateToHost(GuestBase, page_size, &translated_base),
	      "guest base translation failed");
	Check(reinterpret_cast<uint64_t>(translated_base) == address_space.HostBase(),
	      "guest base did not translate to host arena base");

	const uint64_t first_guest = GuestBase + page_size * 4;
	const uint64_t second_guest = GuestBase + page_size * 8;
	void* first_address = nullptr;
	void* second_address = nullptr;
	Check(address_space.TranslateToHost(first_guest, page_size, &first_address),
	      "first guest offset translation failed");
	Check(address_space.TranslateToHost(second_guest, page_size, &second_address),
	      "second guest offset translation failed");
	Check(reinterpret_cast<uint64_t>(first_address) == address_space.HostBase() + page_size * 4,
	      "first guest offset translated incorrectly");
	Check(reinterpret_cast<uint64_t>(second_address) == address_space.HostBase() + page_size * 8,
	      "second guest offset translated incorrectly");
	Check(!address_space.TranslateToHost(GuestBase - page_size, 1, &first_address),
	      "address below the guest interval was accepted");
	Check(first_address == nullptr, "failed translation did not clear its output");
	Check(!address_space.TranslateToHost(GuestBase + ArenaSize - page_size, page_size + 1,
	                                     &first_address),
	      "range extending beyond the guest interval was accepted");
	Check(!address_space.TranslateToHost(GuestBase, 0, &first_address),
	      "zero-length translation was accepted");
	Check(!address_space.Reserve(GuestBase, ArenaSize),
	      "second reservation unexpectedly succeeded");

	char name[64] {};
	const int name_length =
	    std::snprintf(name, sizeof(name), "/kyty-guest-space-test-%d", static_cast<int>(getpid()));
	Check(name_length > 0 && static_cast<size_t>(name_length) < sizeof(name),
	      "shared-memory name formatting failed");

	const int backing_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	Check(backing_fd >= 0, "shm_open failed");
	(void)shm_unlink(name);
	Check(ftruncate(backing_fd, static_cast<off_t>(page_size)) == 0,
	      "ftruncate failed");

	const auto read_write = HostMemoryProtection::Read | HostMemoryProtection::Write;
	Check(address_space.MapShared(first_guest, backing_fd, 0, page_size, read_write),
	      "first guest shared view mapping failed");
	Check(address_space.MapShared(second_guest, backing_fd, 0, page_size, read_write),
	      "second guest shared view mapping failed");

	static constexpr char Pattern[] = "Kyty guest-to-host address translation";
	Check(address_space.TranslateToHost(first_guest, page_size, &first_address),
	      "first alias host translation failed");
	Check(address_space.TranslateToHost(second_guest, page_size, &second_address),
	      "second alias host translation failed");
	std::memcpy(first_address, Pattern, sizeof(Pattern));
	Check(std::memcmp(second_address, Pattern, sizeof(Pattern)) == 0,
	      "second guest alias did not observe the same backing bytes");

	Check(address_space.Protect(first_guest, page_size, HostMemoryProtection::Read),
	      "guest-range read-only protection failed");
	Check(address_space.Protect(first_guest, page_size, read_write),
	      "guest-range read-write protection restoration failed");
	Check(!address_space.Protect(GuestBase - page_size, page_size, read_write),
	      "protection outside the guest interval was accepted");
	Check(!address_space.MapShared(GuestBase + ArenaSize, backing_fd, 0, page_size, read_write),
	      "mapping outside the guest interval was accepted");

	Check(address_space.Unmap(first_guest, page_size),
	      "first guest placeholder restoration failed");
	Check(address_space.Unmap(second_guest, page_size),
	      "second guest placeholder restoration failed");
	Check(close(backing_fd) == 0, "backing descriptor close failed");
	Check(address_space.Release(), "guest address-space release failed");
	Check(!address_space.IsReserved(), "guest address space remained reserved after release");

	std::puts("MacOSGuestAddressSpaceTests: all tests passed");
	return 0;
}
