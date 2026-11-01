#include "kernel/guest_memory/macOSHostAddressArena.h"

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
using Libs::LibKernel::Memory::GuestMemory::MacOSHostAddressArena;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "MacOSHostAddressArenaTests: %s\n", message);
		std::abort();
	}
}

} // namespace

int main() {
	constexpr uint64_t ArenaSize = 64ull * 1024ull * 1024ull;
	MacOSHostAddressArena arena;

	Check(arena.Reserve(ArenaSize), "dynamic arena reservation failed");
	Check(arena.IsReserved(), "arena did not report reserved state");
	Check(arena.Base() != 0, "arena base is null");
	Check(arena.Size() == ArenaSize, "arena size differs from request");
	const uint64_t page_size = arena.PageSize();
	Check(page_size != 0, "Mach host page size is zero");
	Check(page_size <= ArenaSize, "host page exceeds arena size");
	Check(!arena.Reserve(ArenaSize), "second reservation unexpectedly succeeded");
	Check(!arena.Protect(1, page_size, HostMemoryProtection::Read),
	      "unaligned protection range was accepted");
	Check(!arena.RestorePlaceholder(ArenaSize, page_size),
	      "out-of-range placeholder restoration was accepted");

	char name[64] {};
	const int name_length =
	    std::snprintf(name, sizeof(name), "/kyty-arena-test-%d", static_cast<int>(getpid()));
	Check(name_length > 0 && static_cast<size_t>(name_length) < sizeof(name),
	      "shared-memory name formatting failed");

	const int backing_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	Check(backing_fd >= 0, "shm_open failed");
	(void)shm_unlink(name);
	Check(ftruncate(backing_fd, static_cast<off_t>(page_size)) == 0,
	      "ftruncate failed");

	const uint64_t first_offset = page_size * 4;
	const uint64_t second_offset = page_size * 8;
	const auto read_write = HostMemoryProtection::Read | HostMemoryProtection::Write;
	Check(arena.MapShared(first_offset, backing_fd, 0, page_size, read_write),
	      "first shared view mapping failed");
	Check(arena.MapShared(second_offset, backing_fd, 0, page_size, read_write),
	      "second shared view mapping failed");

	static constexpr char Pattern[] = "Kyty host arena shared backing";
	auto* const first = reinterpret_cast<char*>(arena.Base() + first_offset);
	auto* const second = reinterpret_cast<char*>(arena.Base() + second_offset);
	std::memcpy(first, Pattern, sizeof(Pattern));
	Check(std::memcmp(second, Pattern, sizeof(Pattern)) == 0,
	      "second view did not observe writes through the first view");

	Check(arena.Protect(first_offset, page_size, HostMemoryProtection::Read),
	      "read-only protection failed");
	Check(arena.Protect(first_offset, page_size, read_write),
	      "read-write protection restoration failed");

	Check(arena.RestorePlaceholder(first_offset, page_size),
	      "first placeholder restoration failed");
	Check(arena.RestorePlaceholder(second_offset, page_size),
	      "second placeholder restoration failed");
	Check(close(backing_fd) == 0, "backing descriptor close failed");
	Check(arena.Release(), "arena release failed");
	Check(!arena.IsReserved(), "arena remained reserved after release");

	std::puts("MacOSHostAddressArenaTests: all tests passed");
	return 0;
}
