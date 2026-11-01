#include "kernel/guest_memory/macOSGuestAddressSpace.h"

#include <limits>

namespace Libs::LibKernel::Memory::GuestMemory {

MacOSGuestAddressSpace::~MacOSGuestAddressSpace() {
	(void)Release();
}

bool MacOSGuestAddressSpace::Reserve(uint64_t guest_base, uint64_t size) noexcept {
	std::lock_guard lock(m_mutex);
	if (m_guest_base != 0 || m_size != 0 || guest_base == 0 || size == 0 ||
	    size > std::numeric_limits<uint64_t>::max() - guest_base) {
		return false;
	}
	if (!m_arena.Reserve(size)) {
		return false;
	}
	m_guest_base = guest_base;
	m_size = size;
	return true;
}

bool MacOSGuestAddressSpace::Release() noexcept {
	std::lock_guard lock(m_mutex);
	if (m_guest_base == 0 || m_size == 0) {
		return false;
	}
	if (!m_arena.Release()) {
		return false;
	}
	m_guest_base = 0;
	m_size = 0;
	return true;
}

bool MacOSGuestAddressSpace::GuestRangeToOffsetUnlocked(uint64_t guest_address, uint64_t size,
                                                        uint64_t* arena_offset) const noexcept {
	if (arena_offset == nullptr) {
		return false;
	}
	*arena_offset = 0;
	if (m_guest_base == 0 || m_size == 0 || size == 0 || guest_address < m_guest_base ||
	    size > std::numeric_limits<uint64_t>::max() - guest_address) {
		return false;
	}
	const uint64_t offset = guest_address - m_guest_base;
	if (offset > m_size || size > m_size - offset) {
		return false;
	}
	*arena_offset = offset;
	return true;
}

bool MacOSGuestAddressSpace::MapShared(uint64_t guest_base, int backing_fd,
                                       uint64_t backing_offset, uint64_t size,
                                       HostMemoryProtection protection) noexcept {
	std::lock_guard lock(m_mutex);
	uint64_t arena_offset = 0;
	return GuestRangeToOffsetUnlocked(guest_base, size, &arena_offset) &&
	       m_arena.MapShared(arena_offset, backing_fd, backing_offset, size, protection);
}

bool MacOSGuestAddressSpace::Unmap(uint64_t guest_base, uint64_t size) noexcept {
	std::lock_guard lock(m_mutex);
	uint64_t arena_offset = 0;
	return GuestRangeToOffsetUnlocked(guest_base, size, &arena_offset) &&
	       m_arena.RestorePlaceholder(arena_offset, size);
}

bool MacOSGuestAddressSpace::Protect(uint64_t guest_base, uint64_t size,
                                     HostMemoryProtection protection) noexcept {
	std::lock_guard lock(m_mutex);
	uint64_t arena_offset = 0;
	return GuestRangeToOffsetUnlocked(guest_base, size, &arena_offset) &&
	       m_arena.Protect(arena_offset, size, protection);
}

bool MacOSGuestAddressSpace::TranslateToHost(uint64_t guest_address, uint64_t minimum_size,
                                             void** host_address) const noexcept {
	if (host_address == nullptr) {
		return false;
	}
	*host_address = nullptr;

	std::lock_guard lock(m_mutex);
	uint64_t arena_offset = 0;
	if (!GuestRangeToOffsetUnlocked(guest_address, minimum_size, &arena_offset)) {
		return false;
	}
	const uint64_t host_base = m_arena.Base();
	if (host_base == 0 || arena_offset > std::numeric_limits<uint64_t>::max() - host_base) {
		return false;
	}
	*host_address = reinterpret_cast<void*>(host_base + arena_offset);
	return true;
}

uint64_t MacOSGuestAddressSpace::GuestBase() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_guest_base;
}

uint64_t MacOSGuestAddressSpace::Size() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_size;
}

uint64_t MacOSGuestAddressSpace::HostBase() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_arena.Base();
}

uint64_t MacOSGuestAddressSpace::PageSize() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_arena.PageSize();
}

bool MacOSGuestAddressSpace::IsReserved() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_guest_base != 0 && m_size != 0 && m_arena.IsReserved();
}

} // namespace Libs::LibKernel::Memory::GuestMemory
