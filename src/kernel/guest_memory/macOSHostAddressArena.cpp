#include "kernel/guest_memory/macOSHostAddressArena.h"

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <limits>

namespace Libs::LibKernel::Memory::GuestMemory {

namespace {

[[nodiscard]] int ToNativeProtection(HostMemoryProtection protection) noexcept {
	const auto bits = static_cast<uint8_t>(protection);
	constexpr uint8_t ValidBits =
	    static_cast<uint8_t>(HostMemoryProtection::Read) |
	    static_cast<uint8_t>(HostMemoryProtection::Write) |
	    static_cast<uint8_t>(HostMemoryProtection::Execute);
	if ((bits & static_cast<uint8_t>(~ValidBits)) != 0) {
		return -1;
	}

	int result = PROT_NONE;
	if ((bits & static_cast<uint8_t>(HostMemoryProtection::Read)) != 0) {
		result |= PROT_READ;
	}
	if ((bits & static_cast<uint8_t>(HostMemoryProtection::Write)) != 0) {
		result |= PROT_WRITE;
	}
	if ((bits & static_cast<uint8_t>(HostMemoryProtection::Execute)) != 0) {
		result |= PROT_EXEC;
	}
	return result;
}

} // namespace

MacOSHostAddressArena::~MacOSHostAddressArena() {
	(void)Release();
}

bool MacOSHostAddressArena::Reserve(uint64_t size) noexcept {
	std::lock_guard lock(m_mutex);
	if (m_base != 0 || size == 0) {
		return false;
	}

	vm_size_t page_size = 0;
	if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS || page_size == 0 ||
	    size % page_size != 0) {
		return false;
	}

	mach_vm_address_t address = 0;
	const auto result =
	    mach_vm_allocate(mach_task_self(), &address, static_cast<mach_vm_size_t>(size),
	                     VM_FLAGS_ANYWHERE);
	if (result != KERN_SUCCESS || address == 0 || address % page_size != 0 ||
	    size > std::numeric_limits<mach_vm_address_t>::max() - address) {
		if (result == KERN_SUCCESS && address != 0) {
			(void)mach_vm_deallocate(mach_task_self(), address, size);
		}
		return false;
	}

	m_base = address;
	m_size = size;
	m_page_size = page_size;
	return true;
}

bool MacOSHostAddressArena::Release() noexcept {
	std::lock_guard lock(m_mutex);
	if (m_base == 0 || m_size == 0) {
		return false;
	}

	const auto result = mach_vm_deallocate(mach_task_self(), m_base, m_size);
	if (result != KERN_SUCCESS) {
		return false;
	}

	m_base = 0;
	m_size = 0;
	m_page_size = 0;
	return true;
}

bool MacOSHostAddressArena::IsRangeValidUnlocked(uint64_t arena_offset,
                                                 uint64_t size) const noexcept {
	return m_base != 0 && m_page_size != 0 && size != 0 &&
	       arena_offset % m_page_size == 0 && size % m_page_size == 0 &&
	       arena_offset <= m_size && size <= m_size - arena_offset &&
	       arena_offset <= std::numeric_limits<uint64_t>::max() - m_base &&
	       size <= std::numeric_limits<uint64_t>::max() - (m_base + arena_offset);
}

bool MacOSHostAddressArena::MapShared(uint64_t arena_offset, int backing_fd,
                                      uint64_t backing_offset, uint64_t size,
                                      HostMemoryProtection protection) noexcept {
	std::lock_guard lock(m_mutex);
	const int native_protection = ToNativeProtection(protection);
	const auto max_file_offset =
	    static_cast<uint64_t>(std::numeric_limits<off_t>::max());
	if (backing_fd < 0 || native_protection < 0 ||
	    !IsRangeValidUnlocked(arena_offset, size) ||
	    backing_offset % m_page_size != 0 || backing_offset > max_file_offset ||
	    size > max_file_offset - backing_offset) {
		return false;
	}

	void* const address = reinterpret_cast<void*>(m_base + arena_offset);
	void* const mapped = mmap(address, static_cast<size_t>(size), native_protection,
	                          MAP_SHARED | MAP_FIXED, backing_fd,
	                          static_cast<off_t>(backing_offset));
	if (mapped == MAP_FAILED || mapped != address) {
		// Keep the arena owned even when a file view could not be installed.
		(void)mmap(address, static_cast<size_t>(size), PROT_NONE,
		           MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		return false;
	}
	return true;
}

bool MacOSHostAddressArena::CommitAnonymous(uint64_t arena_offset, uint64_t size,
                                                    HostMemoryProtection protection) noexcept {
	std::lock_guard lock(m_mutex);
	const int native_protection = ToNativeProtection(protection);
	if (native_protection < 0 || !IsRangeValidUnlocked(arena_offset, size)) {
		return false;
	}
	void* const address = reinterpret_cast<void*>(m_base + arena_offset);
	void* const committed =
	    mmap(address, static_cast<size_t>(size), native_protection,
	         MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	return committed != MAP_FAILED && committed == address;
}

bool MacOSHostAddressArena::RestorePlaceholder(uint64_t arena_offset, uint64_t size) noexcept {
	std::lock_guard lock(m_mutex);
	if (!IsRangeValidUnlocked(arena_offset, size)) {
		return false;
	}

	void* const address = reinterpret_cast<void*>(m_base + arena_offset);
	void* const restored =
	    mmap(address, static_cast<size_t>(size), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED,
	         -1, 0);
	return restored != MAP_FAILED && restored == address;
}

bool MacOSHostAddressArena::Protect(uint64_t arena_offset, uint64_t size,
                                    HostMemoryProtection protection) noexcept {
	std::lock_guard lock(m_mutex);
	const int native_protection = ToNativeProtection(protection);
	if (native_protection < 0 || !IsRangeValidUnlocked(arena_offset, size)) {
		return false;
	}
	return mprotect(reinterpret_cast<void*>(m_base + arena_offset), static_cast<size_t>(size),
	                native_protection) == 0;
}

uint64_t MacOSHostAddressArena::Base() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_base;
}

uint64_t MacOSHostAddressArena::Size() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_size;
}

uint64_t MacOSHostAddressArena::PageSize() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_page_size;
}

bool MacOSHostAddressArena::IsReserved() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_base != 0 && m_size != 0;
}

} // namespace Libs::LibKernel::Memory::GuestMemory
