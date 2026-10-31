#ifndef EMULATOR_SRC_KERNEL_GUEST_MEMORY_GUESTMADDRESSSPACE_H_
#define EMULATOR_SRC_KERNEL_GUEST_MEMORY_GUESTMADDRESSSPACE_H_

#include <cstddef>
#include <cstdint>

namespace Libs::LibKernel::Memory::GuestMemory {

using GuestVirtualAddress = uint64_t;
using GuestMemorySize = uint64_t;
using GuestBackingOffset = uint64_t;

// Opaque identity for host-owned physical backing. It is not a host pointer and
// it is not a PS5 physical address.
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
	AddressNotReserved,
	AddressNotMapped,
	BackingNotFound,
	BackingRangeOutOfBounds,
	PermissionDenied,
	HostAllocationFailed,
	HostMappingUnavailable,
	NotImplemented,
};

// A translation describes only the first contiguous backing span. Callers must
// translate again when they reach contiguous_size. The handle remains valid
// only while the address-space mapping generation is unchanged.
struct GuestMemoryTranslation {
	GuestBackingId backing {};
	GuestBackingOffset backing_offset = 0;
	GuestMemorySize contiguous_size = 0;
	GuestMemoryPermission permissions = GuestMemoryPermission::None;
	uint64_t generation = 0;
};

// Owns bytes independently of any guest virtual address. Two guest virtual
// addresses can refer to the same backing handle and offset to model aliases.
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

// Models guest virtual reservations and mappings. Reserve/ReleaseReservation
// manage address-space ownership; Map/Unmap attach or detach physical backing.
// This interface deliberately exposes no host pointer and requires no
// MAP_FIXED mapping at the guest's numeric address.
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
};

} // namespace Libs::LibKernel::Memory::GuestMemory

#endif // EMULATOR_SRC_KERNEL_GUEST_MEMORY_GUESTMADDRESSSPACE_H_
