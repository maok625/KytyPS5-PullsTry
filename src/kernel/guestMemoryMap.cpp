#include "kernel/guestMemoryMap.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace Libs::LibKernel::Memory {

bool GuestMemoryMap::Map(uint64_t guest_address, size_t size) {
	if (guest_address == 0 || size == 0 || (guest_address & (PageSize - 1)) != 0 ||
	    (size & (PageSize - 1)) != 0 || size > std::numeric_limits<uint64_t>::max() - guest_address) {
		return false;
	}

	const auto guest_end = guest_address + size;
	std::lock_guard lock(m_mutex);

	auto next = m_regions.lower_bound(guest_address);
	if (next != m_regions.end() && next->first < guest_end) {
		return false;
	}
	if (next != m_regions.begin()) {
		const auto previous = std::prev(next);
		const auto previous_end = previous->first + previous->second.size();
		if (previous_end > guest_address) {
			return false;
		}
	}

	try {
		std::vector<std::byte> backing(size);
		return m_regions.emplace(guest_address, std::move(backing)).second;
	} catch (const std::bad_alloc&) {
		return false;
	} catch (const std::length_error&) {
		return false;
	}
}

bool GuestMemoryMap::Unmap(uint64_t guest_address) {
	std::lock_guard lock(m_mutex);
	return m_regions.erase(guest_address) == 1;
}

bool GuestMemoryMap::IsMapped(uint64_t guest_address, size_t size) const noexcept {
	if (size == 0) {
		return true;
	}
	if (guest_address == 0 || size > std::numeric_limits<uint64_t>::max() - guest_address) {
		return false;
	}
	std::lock_guard lock(m_mutex);
	return IsMappedUnlocked(guest_address, size);
}

bool GuestMemoryMap::Read(uint64_t guest_address, void* destination, size_t size) const noexcept {
	if (size == 0) {
		return true;
	}
	if (destination == nullptr || guest_address == 0 ||
	    size > std::numeric_limits<uint64_t>::max() - guest_address) {
		return false;
	}

	std::lock_guard lock(m_mutex);
	if (!IsMappedUnlocked(guest_address, size)) {
		return false;
	}
	CopyFromGuestUnlocked(guest_address, destination, size);
	return true;
}

bool GuestMemoryMap::Write(uint64_t guest_address, const void* source, size_t size) noexcept {
	if (size == 0) {
		return true;
	}
	if (source == nullptr || guest_address == 0 ||
	    size > std::numeric_limits<uint64_t>::max() - guest_address) {
		return false;
	}

	std::lock_guard lock(m_mutex);
	if (!IsMappedUnlocked(guest_address, size)) {
		return false;
	}
	CopyToGuestUnlocked(guest_address, source, size);
	return true;
}

size_t GuestMemoryMap::MappingCount() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_regions.size();
}

bool GuestMemoryMap::IsMappedUnlocked(uint64_t guest_address, size_t size) const noexcept {
	uint64_t cursor = guest_address;
	size_t remaining = size;

	while (remaining != 0) {
		auto next = m_regions.upper_bound(cursor);
		if (next == m_regions.begin()) {
			return false;
		}
		const auto region = std::prev(next);
		const auto region_end = region->first + region->second.size();
		if (cursor < region->first || cursor >= region_end) {
			return false;
		}

		const auto available = static_cast<size_t>(region_end - cursor);
		const auto copied = std::min(remaining, available);
		cursor += copied;
		remaining -= copied;
	}
	return true;
}

void GuestMemoryMap::CopyFromGuestUnlocked(uint64_t guest_address, void* destination,
                                           size_t size) const noexcept {
	auto* output = static_cast<std::byte*>(destination);
	uint64_t cursor = guest_address;
	size_t copied = 0;

	while (copied != size) {
		auto region = std::prev(m_regions.upper_bound(cursor));
		const auto offset = static_cast<size_t>(cursor - region->first);
		const auto chunk = std::min(size - copied, region->second.size() - offset);
		std::memcpy(output + copied, region->second.data() + offset, chunk);
		cursor += chunk;
		copied += chunk;
	}
}

void GuestMemoryMap::CopyToGuestUnlocked(uint64_t guest_address, const void* source,
                                         size_t size) noexcept {
	const auto* input = static_cast<const std::byte*>(source);
	uint64_t cursor = guest_address;
	size_t copied = 0;

	while (copied != size) {
		auto region = std::prev(m_regions.upper_bound(cursor));
		const auto offset = static_cast<size_t>(cursor - region->first);
		const auto chunk = std::min(size - copied, region->second.size() - offset);
		std::memcpy(region->second.data() + offset, input + copied, chunk);
		cursor += chunk;
		copied += chunk;
	}
}

} // namespace Libs::LibKernel::Memory
