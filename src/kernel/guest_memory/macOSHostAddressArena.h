#ifndef EMULATOR_SRC_KERNEL_GUEST_MEMORY_MACOSHOSTADDRESSARENA_H_
#define EMULATOR_SRC_KERNEL_GUEST_MEMORY_MACOSHOSTADDRESSARENA_H_

#include <cstdint>
#include <mutex>

namespace Libs::LibKernel::Memory::GuestMemory {

enum class HostMemoryProtection : uint8_t {
	None = 0,
	Read = 1u << 0u,
	Write = 1u << 1u,
	Execute = 1u << 2u,
};

[[nodiscard]] constexpr HostMemoryProtection operator|(HostMemoryProtection left,
                                                       HostMemoryProtection right) noexcept {
	return static_cast<HostMemoryProtection>(static_cast<uint8_t>(left) |
	                                         static_cast<uint8_t>(right));
}

class MacOSHostAddressArena final {
public:
	MacOSHostAddressArena() = default;
	~MacOSHostAddressArena();

	MacOSHostAddressArena(const MacOSHostAddressArena&) = delete;
	MacOSHostAddressArena& operator=(const MacOSHostAddressArena&) = delete;

	// Reserves a contiguous, dynamically selected host virtual-address range.
	// This reserves address space only; it does not commit physical memory.
	[[nodiscard]] bool Reserve(uint64_t size) noexcept;

	// Releases the entire arena, including any views that are still mapped in it.
	[[nodiscard]] bool Release() noexcept;

	// Maps a shared file descriptor view into a range owned by this arena.
	// The descriptor must remain open while the view is in use.
	[[nodiscard]] bool MapShared(uint64_t arena_offset, int backing_fd,
	                             uint64_t backing_offset, uint64_t size,
	                             HostMemoryProtection protection) noexcept;

	// Replaces a shared view with an anonymous, inaccessible placeholder.
	[[nodiscard]] bool RestorePlaceholder(uint64_t arena_offset, uint64_t size) noexcept;

	[[nodiscard]] bool Protect(uint64_t arena_offset, uint64_t size,
	                           HostMemoryProtection protection) noexcept;

	[[nodiscard]] uint64_t Base() const noexcept;
	[[nodiscard]] uint64_t Size() const noexcept;
	[[nodiscard]] uint64_t PageSize() const noexcept;
	[[nodiscard]] bool IsReserved() const noexcept;

private:
	[[nodiscard]] bool IsRangeValidUnlocked(uint64_t arena_offset,
	                                        uint64_t size) const noexcept;

	mutable std::mutex m_mutex;
	uint64_t m_base = 0;
	uint64_t m_size = 0;
	uint64_t m_page_size = 0;
};

} // namespace Libs::LibKernel::Memory::GuestMemory

#endif // EMULATOR_SRC_KERNEL_GUEST_MEMORY_MACOSHOSTADDRESSARENA_H_
