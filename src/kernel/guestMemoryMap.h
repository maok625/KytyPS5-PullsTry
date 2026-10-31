#ifndef EMULATOR_SRC_KERNEL_GUESTMEMORYMAP_H_
#define EMULATOR_SRC_KERNEL_GUESTMEMORYMAP_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace Libs::LibKernel::Memory {

// Experimental guest-to-host memory map.
// Guest virtual addresses are identifiers here; they are never cast to host pointers.
class GuestMemoryMap final {
public:
	static constexpr uint64_t PageSize = 0x4000;

	GuestMemoryMap() = default;
	~GuestMemoryMap() = default;

	GuestMemoryMap(const GuestMemoryMap&) = delete;
	GuestMemoryMap& operator=(const GuestMemoryMap&) = delete;

	// Creates zero-initialized host backing for an aligned guest address range.
	// Returns false for invalid, overlapping, or unallocatable ranges.
	[[nodiscard]] bool Map(uint64_t guest_address, size_t size);

	// Removes the mapping whose base is exactly guest_address.
	[[nodiscard]] bool Unmap(uint64_t guest_address);

	// Copies bytes by guest address. A request may span adjacent mappings, but
	// it must be completely mapped; failed writes never partially modify memory.
	[[nodiscard]] bool Read(uint64_t guest_address, void* destination, size_t size) const noexcept;
	[[nodiscard]] bool Write(uint64_t guest_address, const void* source, size_t size) noexcept;

	[[nodiscard]] bool IsMapped(uint64_t guest_address, size_t size) const noexcept;
	[[nodiscard]] size_t MappingCount() const noexcept;

private:
	using RegionMap = std::map<uint64_t, std::vector<std::byte>>;

	[[nodiscard]] bool IsMappedUnlocked(uint64_t guest_address, size_t size) const noexcept;
	void CopyFromGuestUnlocked(uint64_t guest_address, void* destination, size_t size) const noexcept;
	void CopyToGuestUnlocked(uint64_t guest_address, const void* source, size_t size) noexcept;

	mutable std::mutex m_mutex;
	RegionMap m_regions;
};

} // namespace Libs::LibKernel::Memory

#endif // EMULATOR_SRC_KERNEL_GUESTMEMORYMAP_H_
