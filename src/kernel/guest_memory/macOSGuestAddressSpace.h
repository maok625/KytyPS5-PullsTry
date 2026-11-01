#ifndef EMULATOR_SRC_KERNEL_GUEST_MEMORY_MACOSGUESTADDRESSSPACE_H_
#define EMULATOR_SRC_KERNEL_GUEST_MEMORY_MACOSGUESTADDRESSSPACE_H_

#include "kernel/guest_memory/macOSHostAddressArena.h"

#include <cstdint>
#include <mutex>

namespace Libs::LibKernel::Memory::GuestMemory {

// Maps one guest virtual-address interval into a separately allocated host
// virtual-address arena. Guest addresses remain integer values; only
// TranslateToHost returns a dereferenceable host pointer.
//
// Mapping, unmapping and protection ranges must be aligned to the macOS host
// page size. This is a host backend constraint, not a definition of PS5 guest
// page size. Callers must also synchronize pointer use against Release().
class MacOSGuestAddressSpace final {
public:
	MacOSGuestAddressSpace() = default;
	~MacOSGuestAddressSpace();

	MacOSGuestAddressSpace(const MacOSGuestAddressSpace&) = delete;
	MacOSGuestAddressSpace& operator=(const MacOSGuestAddressSpace&) = delete;

	[[nodiscard]] bool Reserve(uint64_t guest_base, uint64_t size) noexcept;
	[[nodiscard]] bool Release() noexcept;

	[[nodiscard]] bool MapShared(uint64_t guest_base, int backing_fd,
	                             uint64_t backing_offset, uint64_t size,
	                             HostMemoryProtection protection) noexcept;
	[[nodiscard]] bool CommitAnonymous(uint64_t guest_base, uint64_t size,
	                                   HostMemoryProtection protection) noexcept;
	[[nodiscard]] bool Unmap(uint64_t guest_base, uint64_t size) noexcept;
	[[nodiscard]] bool Protect(uint64_t guest_base, uint64_t size,
	                           HostMemoryProtection protection) noexcept;

	// Returns the host pointer corresponding to a guest address range.
	// Translation validates the reserved interval, not whether pages are mapped
	// or readable/writable; the caller must enforce guest mapping permissions.
	[[nodiscard]] bool TranslateToHost(uint64_t guest_address, uint64_t minimum_size,
	                                   void** host_address) const noexcept;

	[[nodiscard]] uint64_t GuestBase() const noexcept;
	[[nodiscard]] uint64_t Size() const noexcept;
	[[nodiscard]] uint64_t HostBase() const noexcept;
	[[nodiscard]] uint64_t PageSize() const noexcept;
	[[nodiscard]] bool IsReserved() const noexcept;

private:
	[[nodiscard]] bool GuestRangeToOffsetUnlocked(uint64_t guest_address, uint64_t size,
	                                              uint64_t* arena_offset) const noexcept;

	mutable std::mutex m_mutex;
	MacOSHostAddressArena m_arena;
	uint64_t m_guest_base = 0;
	uint64_t m_size = 0;
};

} // namespace Libs::LibKernel::Memory::GuestMemory

#endif // EMULATOR_SRC_KERNEL_GUEST_MEMORY_MACOSGUESTADDRESSSPACE_H_
