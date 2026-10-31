#ifndef EMULATOR_SRC_KERNEL_GUEST_MEMORY_GUESTMEMORYADDRESSSPACE_H_
#define EMULATOR_SRC_KERNEL_GUEST_MEMORY_GUESTMEMORYADDRESSSPACE_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Libs::LibKernel::Memory::GuestMemory {

using GuestVirtualAddress = uint64_t;
using GuestMemorySize = uint64_t;
using GuestBackingOffset = uint64_t;

struct GuestBackingId {
	uint64_t value = 0;
	[[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
	friend bool operator==(GuestBackingId, GuestBackingId) = default;
};

enum class GuestMemoryPermission : uint8_t {
	None = 0,
	CpuRead = 1u << 0u,
	CpuWrite = 1u << 1u,
	CpuExecute = 1u << 2u,
	GpuRead = 1u << 3u,
	GpuWrite = 1u << 4u,
};

[[nodiscard]] constexpr GuestMemoryPermission operator|(GuestMemoryPermission left,
                                                        GuestMemoryPermission right) noexcept {
	return static_cast<GuestMemoryPermission>(static_cast<uint8_t>(left) |
	                                          static_cast<uint8_t>(right));
}
[[nodiscard]] constexpr GuestMemoryPermission operator&(GuestMemoryPermission left,
                                                        GuestMemoryPermission right) noexcept {
	return static_cast<GuestMemoryPermission>(static_cast<uint8_t>(left) &
	                                          static_cast<uint8_t>(right));
}
constexpr GuestMemoryPermission& operator|=(GuestMemoryPermission& left,
                                             GuestMemoryPermission right) noexcept {
	left = left | right;
	return left;
}

enum class GuestMemoryStatus : uint8_t {
	Success,
	InvalidArgument,
	InvalidAlignment,
	AddressAlreadyMapped,
	AddressAlreadyReserved,
	AddressNotReserved,
	AddressNotMapped,
	AddressStillMapped,
	BackingNotFound,
	BackingInUse,
	BackingRangeOutOfBounds,
	PermissionDenied,
	HostAllocationFailed,
	HostMappingUnavailable,
	NotImplemented,
};

struct GuestMemoryTranslation {
	GuestBackingId backing {};
	GuestBackingOffset backing_offset = 0;
	GuestMemorySize contiguous_size = 0;
	GuestMemoryPermission permissions = GuestMemoryPermission::None;
	uint64_t generation = 0;
};

class IGuestBackingStore {
public:
	virtual ~IGuestBackingStore() = default;
	[[nodiscard]] virtual GuestMemoryStatus Allocate(GuestMemorySize size,
	                                                 GuestMemorySize alignment,
	                                                 GuestBackingId* out_backing) = 0;
	[[nodiscard]] virtual GuestMemoryStatus Release(GuestBackingId backing) = 0;
	[[nodiscard]] virtual GuestMemoryStatus Read(GuestBackingId backing,
	                                            GuestBackingOffset offset,
	                                            void* destination,
	                                            GuestMemorySize size) const = 0;
	[[nodiscard]] virtual GuestMemoryStatus Write(GuestBackingId backing,
	                                             GuestBackingOffset offset,
	                                             const void* source,
	                                             GuestMemorySize size) = 0;
};

class IGuestAddressSpace {
public:
	virtual ~IGuestAddressSpace() = default;
	[[nodiscard]] virtual GuestMemoryStatus Reserve(GuestVirtualAddress guest_base,
	                                               GuestMemorySize size) = 0;
	[[nodiscard]] virtual GuestMemoryStatus ReleaseReservation(GuestVirtualAddress guest_base,
	                                                          GuestMemorySize size) = 0;
	[[nodiscard]] virtual GuestMemoryStatus Map(GuestVirtualAddress guest_base,
	                                           GuestBackingId backing,
	                                           GuestBackingOffset backing_offset,
	                                           GuestMemorySize size,
	                                           GuestMemoryPermission permissions) = 0;
	[[nodiscard]] virtual GuestMemoryStatus Unmap(GuestVirtualAddress guest_base,
	                                             GuestMemorySize size) = 0;
	[[nodiscard]] virtual GuestMemoryStatus Protect(GuestVirtualAddress guest_base,
	                                               GuestMemorySize size,
	                                               GuestMemoryPermission permissions) = 0;
	[[nodiscard]] virtual GuestMemoryStatus Translate(GuestVirtualAddress guest_address,
	                                                 GuestMemorySize minimum_size,
	                                                 GuestMemoryPermission required,
	                                                 GuestMemoryTranslation* out) const = 0;
	[[nodiscard]] virtual GuestMemoryStatus Read(GuestVirtualAddress guest_address,
	                                            void* destination,
	                                            GuestMemorySize size) const = 0;
	[[nodiscard]] virtual GuestMemoryStatus Write(GuestVirtualAddress guest_address,
	                                             const void* source,
	                                             GuestMemorySize size) = 0;
	[[nodiscard]] virtual uint64_t GetGeneration() const noexcept = 0;
};

// Reference implementation for tests and early integration. A guest address
// is only a key: backing bytes are held separately and can be aliased.
//
// Pass the guest page size explicitly. It must not be inferred from the macOS
// host page size reported by sysctl or Mach.
class GuestMemoryModel final : public IGuestBackingStore, public IGuestAddressSpace {
public:
	explicit GuestMemoryModel(GuestMemorySize guest_page_size);
	[[nodiscard]] GuestMemorySize GetGuestPageSize() const noexcept;

	[[nodiscard]] GuestMemoryStatus Allocate(GuestMemorySize size, GuestMemorySize alignment,
	                                         GuestBackingId* out_backing) override;
	[[nodiscard]] GuestMemoryStatus Release(GuestBackingId backing) override;
	[[nodiscard]] GuestMemoryStatus Read(GuestBackingId backing, GuestBackingOffset offset,
	                                     void* destination, GuestMemorySize size) const override;
	[[nodiscard]] GuestMemoryStatus Write(GuestBackingId backing, GuestBackingOffset offset,
	                                      const void* source, GuestMemorySize size) override;

	[[nodiscard]] GuestMemoryStatus Reserve(GuestVirtualAddress guest_base,
	                                        GuestMemorySize size) override;
	[[nodiscard]] GuestMemoryStatus ReleaseReservation(GuestVirtualAddress guest_base,
	                                                   GuestMemorySize size) override;
	[[nodiscard]] GuestMemoryStatus Map(GuestVirtualAddress guest_base, GuestBackingId backing,
	                                    GuestBackingOffset backing_offset, GuestMemorySize size,
	                                    GuestMemoryPermission permissions) override;
	[[nodiscard]] GuestMemoryStatus Unmap(GuestVirtualAddress guest_base,
	                                      GuestMemorySize size) override;
	[[nodiscard]] GuestMemoryStatus Protect(GuestVirtualAddress guest_base, GuestMemorySize size,
	                                        GuestMemoryPermission permissions) override;
	[[nodiscard]] GuestMemoryStatus Translate(GuestVirtualAddress guest_address,
	                                          GuestMemorySize minimum_size,
	                                          GuestMemoryPermission required,
	                                          GuestMemoryTranslation* out) const override;
	[[nodiscard]] GuestMemoryStatus Read(GuestVirtualAddress guest_address, void* destination,
	                                     GuestMemorySize size) const override;
	[[nodiscard]] GuestMemoryStatus Write(GuestVirtualAddress guest_address, const void* source,
	                                      GuestMemorySize size) override;
	[[nodiscard]] uint64_t GetGeneration() const noexcept override;

private:
	struct Mapping {
		GuestVirtualAddress guest_base = 0;
		GuestMemorySize size = 0;
		GuestBackingId backing {};
		GuestBackingOffset backing_offset = 0;
		GuestMemoryPermission permissions = GuestMemoryPermission::None;
	};
	using MappingMap = std::map<GuestVirtualAddress, Mapping>;
	using BackingMap = std::unordered_map<uint64_t, std::vector<std::byte>>;

	[[nodiscard]] bool IsRangeValid(GuestVirtualAddress base, GuestMemorySize size) const noexcept;
	[[nodiscard]] bool IsInsideReservationUnlocked(GuestVirtualAddress base,
	                                               GuestMemorySize size) const noexcept;
	[[nodiscard]] bool IsMappedUnlocked(GuestVirtualAddress base, GuestMemorySize size,
	                                    GuestMemoryPermission required) const noexcept;
	[[nodiscard]] MappingMap::const_iterator FindMappingUnlocked(
	    GuestVirtualAddress address) const noexcept;

	GuestMemorySize m_guest_page_size = 0;
	mutable std::mutex m_mutex;
	BackingMap m_backings;
	std::map<GuestVirtualAddress, GuestMemorySize> m_reservations;
	MappingMap m_mappings;
	uint64_t m_next_backing_id = 1;
	uint64_t m_generation = 1;
};

} // namespace Libs::LibKernel::Memory::GuestMemory

#endif // EMULATOR_SRC_KERNEL_GUEST_MEMORY_GUESTMEMORYADDRESSSPACE_H_
