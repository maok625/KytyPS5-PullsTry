#include "kernel/guest_memory/guestMemoryAddressSpace.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace Libs::LibKernel::Memory::GuestMemory {

namespace {

[[nodiscard]] bool IsPowerOfTwo(uint64_t value) noexcept {
	return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] bool HasPermissions(GuestMemoryPermission granted,
                                  GuestMemoryPermission required) noexcept {
	const auto granted_bits = static_cast<uint8_t>(granted);
	const auto required_bits = static_cast<uint8_t>(required);
	return (granted_bits & required_bits) == required_bits;
}

[[nodiscard]] bool AddOverflows(uint64_t base, uint64_t size) noexcept {
	return size > std::numeric_limits<uint64_t>::max() - base;
}

} // namespace

GuestMemoryModel::GuestMemoryModel(GuestMemorySize guest_page_size):
	m_guest_page_size(IsPowerOfTwo(guest_page_size) ? guest_page_size : 0) {}

GuestMemorySize GuestMemoryModel::GetGuestPageSize() const noexcept {
	return m_guest_page_size;
}

bool GuestMemoryModel::IsRangeValid(GuestVirtualAddress base, GuestMemorySize size) const noexcept {
	return m_guest_page_size != 0 && base != 0 && size != 0 && !AddOverflows(base, size) &&
	       (base & (m_guest_page_size - 1)) == 0 &&
	       (size & (m_guest_page_size - 1)) == 0;
}

GuestMemoryStatus GuestMemoryModel::Allocate(GuestMemorySize size, GuestMemorySize alignment,
                                             GuestBackingId* out_backing) {
	if (out_backing == nullptr || size == 0) {
		return GuestMemoryStatus::InvalidArgument;
	}
	*out_backing = {};
	if (!IsPowerOfTwo(alignment)) {
		return GuestMemoryStatus::InvalidAlignment;
	}
	if (size > static_cast<GuestMemorySize>(std::numeric_limits<size_t>::max())) {
		return GuestMemoryStatus::HostAllocationFailed;
	}

	std::lock_guard lock(m_mutex);
	if (m_next_backing_id == 0) {
		return GuestMemoryStatus::HostAllocationFailed;
	}
	try {
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		const uint64_t id = m_next_backing_id;
		const auto [unused, inserted] = m_backings.emplace(id, std::move(bytes));
		(void)unused;
		if (!inserted) {
			return GuestMemoryStatus::HostAllocationFailed;
		}
		m_next_backing_id = id == std::numeric_limits<uint64_t>::max() ? 0 : id + 1;
		*out_backing = {id};
		return GuestMemoryStatus::Success;
	} catch (const std::bad_alloc&) {
		return GuestMemoryStatus::HostAllocationFailed;
	} catch (const std::length_error&) {
		return GuestMemoryStatus::HostAllocationFailed;
	}
}

GuestMemoryStatus GuestMemoryModel::Release(GuestBackingId backing) {
	if (!backing) {
		return GuestMemoryStatus::InvalidArgument;
	}
	std::lock_guard lock(m_mutex);
	if (m_backings.find(backing.value) == m_backings.end()) {
		return GuestMemoryStatus::BackingNotFound;
	}
	for (const auto& [unused, mapping]: m_mappings) {
		(void)unused;
		if (mapping.backing == backing) {
			return GuestMemoryStatus::BackingInUse;
		}
	}
	m_backings.erase(backing.value);
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::Read(GuestBackingId backing, GuestBackingOffset offset,
                                         void* destination, GuestMemorySize size) const {
	if (size == 0) {
		return GuestMemoryStatus::Success;
	}
	if (!backing || destination == nullptr || AddOverflows(offset, size)) {
		return GuestMemoryStatus::InvalidArgument;
	}
	std::lock_guard lock(m_mutex);
	const auto found = m_backings.find(backing.value);
	if (found == m_backings.end()) {
		return GuestMemoryStatus::BackingNotFound;
	}
	if (offset > found->second.size() || size > found->second.size() - offset) {
		return GuestMemoryStatus::BackingRangeOutOfBounds;
	}
	std::memmove(destination, found->second.data() + static_cast<size_t>(offset),
	             static_cast<size_t>(size));
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::Write(GuestBackingId backing, GuestBackingOffset offset,
                                          const void* source, GuestMemorySize size) {
	if (size == 0) {
		return GuestMemoryStatus::Success;
	}
	if (!backing || source == nullptr || AddOverflows(offset, size)) {
		return GuestMemoryStatus::InvalidArgument;
	}
	std::lock_guard lock(m_mutex);
	auto found = m_backings.find(backing.value);
	if (found == m_backings.end()) {
		return GuestMemoryStatus::BackingNotFound;
	}
	if (offset > found->second.size() || size > found->second.size() - offset) {
		return GuestMemoryStatus::BackingRangeOutOfBounds;
	}
	std::memmove(found->second.data() + static_cast<size_t>(offset), source,
	             static_cast<size_t>(size));
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::Reserve(GuestVirtualAddress guest_base,
                                            GuestMemorySize size) {
	if (guest_base == 0 || size == 0 || AddOverflows(guest_base, size)) {
		return GuestMemoryStatus::InvalidArgument;
	}
	if (m_guest_page_size == 0 || (guest_base & (m_guest_page_size - 1)) != 0 ||
	    (size & (m_guest_page_size - 1)) != 0) {
		return GuestMemoryStatus::InvalidAlignment;
	}

	std::lock_guard lock(m_mutex);
	const auto end = guest_base + size;
	auto next = m_reservations.lower_bound(guest_base);
	if (next != m_reservations.end() && next->first < end) {
		return GuestMemoryStatus::AddressAlreadyMapped;
	}
	if (next != m_reservations.begin()) {
		const auto previous = std::prev(next);
		if (previous->first + previous->second > guest_base) {
			return GuestMemoryStatus::AddressAlreadyMapped;
		}
	}
	try {
		m_reservations.emplace(guest_base, size);
	} catch (const std::bad_alloc&) {
		return GuestMemoryStatus::HostAllocationFailed;
	}
	++m_generation;
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::ReleaseReservation(GuestVirtualAddress guest_base,
                                                       GuestMemorySize size) {
	if (guest_base == 0 || size == 0 || AddOverflows(guest_base, size)) {
		return GuestMemoryStatus::InvalidArgument;
	}
	std::lock_guard lock(m_mutex);
	const auto reservation = m_reservations.find(guest_base);
	if (reservation == m_reservations.end() || reservation->second != size) {
		return GuestMemoryStatus::AddressNotReserved;
	}
	const auto end = guest_base + size;
	auto mapping = m_mappings.lower_bound(guest_base);
	if (mapping != m_mappings.begin()) {
		const auto previous = std::prev(mapping);
		if (previous->second.guest_base + previous->second.size > guest_base) {
			mapping = previous;
		}
	}
	if (mapping != m_mappings.end() && mapping->second.guest_base < end &&
	    mapping->second.guest_base + mapping->second.size > guest_base) {
		return GuestMemoryStatus::AddressStillMapped;
	}
	m_reservations.erase(reservation);
	++m_generation;
	return GuestMemoryStatus::Success;
}

bool GuestMemoryModel::IsInsideReservationUnlocked(GuestVirtualAddress base,
                                                   GuestMemorySize size) const noexcept {
	if (size == 0 || AddOverflows(base, size)) {
		return false;
	}
	auto next = m_reservations.upper_bound(base);
	if (next == m_reservations.begin()) {
		return false;
	}
	const auto reservation = std::prev(next);
	return base >= reservation->first && base + size <= reservation->first + reservation->second;
}

GuestMemoryModel::MappingMap::const_iterator GuestMemoryModel::FindMappingUnlocked(
    GuestVirtualAddress address) const noexcept {
	auto next = m_mappings.upper_bound(address);
	if (next == m_mappings.begin()) {
		return m_mappings.end();
	}
	const auto current = std::prev(next);
	return address < current->second.guest_base + current->second.size ? current : m_mappings.end();
}

bool GuestMemoryModel::IsMappedUnlocked(GuestVirtualAddress base, GuestMemorySize size,
                                         GuestMemoryPermission required) const noexcept {
	if (size == 0 || AddOverflows(base, size)) {
		return false;
	}
	const auto end = base + size;
	auto cursor = base;
	while (cursor < end) {
		const auto mapping = FindMappingUnlocked(cursor);
		if (mapping == m_mappings.end() ||
		    !HasPermissions(mapping->second.permissions, required)) {
			return false;
		}
		cursor = std::min(end, mapping->second.guest_base + mapping->second.size);
	}
	return true;
}

GuestMemoryStatus GuestMemoryModel::Map(GuestVirtualAddress guest_base, GuestBackingId backing,
                                        GuestBackingOffset backing_offset, GuestMemorySize size,
                                        GuestMemoryPermission permissions) {
	if (guest_base == 0 || size == 0 || AddOverflows(guest_base, size)) {
		return GuestMemoryStatus::InvalidArgument;
	}
	if (m_guest_page_size == 0 || (guest_base & (m_guest_page_size - 1)) != 0 ||
	    (size & (m_guest_page_size - 1)) != 0) {
		return GuestMemoryStatus::InvalidAlignment;
	}
	if (m_guest_page_size == 0 || (backing_offset & (m_guest_page_size - 1)) != 0) {
		return GuestMemoryStatus::InvalidAlignment;
	}
	if (AddOverflows(backing_offset, size)) {
		return GuestMemoryStatus::BackingRangeOutOfBounds;
	}

	std::lock_guard lock(m_mutex);
	if (!IsInsideReservationUnlocked(guest_base, size)) {
		return GuestMemoryStatus::AddressNotReserved;
	}
	const auto backing_it = m_backings.find(backing.value);
	if (!backing || backing_it == m_backings.end()) {
		return GuestMemoryStatus::BackingNotFound;
	}
	if (backing_offset > backing_it->second.size() ||
	    size > backing_it->second.size() - backing_offset) {
		return GuestMemoryStatus::BackingRangeOutOfBounds;
	}

	const auto end = guest_base + size;
	auto next = m_mappings.lower_bound(guest_base);
	if (next != m_mappings.end() && next->first < end) {
		return GuestMemoryStatus::AddressAlreadyMapped;
	}
	if (next != m_mappings.begin()) {
		const auto previous = std::prev(next);
		if (previous->second.guest_base + previous->second.size > guest_base) {
			return GuestMemoryStatus::AddressAlreadyMapped;
		}
	}
	try {
		m_mappings.emplace(guest_base, Mapping {guest_base, size, backing, backing_offset, permissions});
	} catch (const std::bad_alloc&) {
		return GuestMemoryStatus::HostAllocationFailed;
	}
	++m_generation;
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::Unmap(GuestVirtualAddress guest_base, GuestMemorySize size) {
	if (!IsRangeValid(guest_base, size)) {
		return GuestMemoryStatus::InvalidAlignment;
	}
	std::lock_guard lock(m_mutex);
	if (!IsMappedUnlocked(guest_base, size, GuestMemoryPermission::None)) {
		return GuestMemoryStatus::AddressNotMapped;
	}

	const auto end = guest_base + size;
	try {
		MappingMap replacement = m_mappings;
		std::vector<Mapping> fragments;
		auto it = replacement.lower_bound(guest_base);
		if (it != replacement.begin()) {
			auto previous = std::prev(it);
			if (previous->second.guest_base + previous->second.size > guest_base) {
				it = previous;
			}
		}
		while (it != replacement.end() && it->second.guest_base < end) {
			const Mapping old = it->second;
			const auto old_end = old.guest_base + old.size;
			if (old.guest_base < guest_base) {
				Mapping left = old;
				left.size = guest_base - old.guest_base;
				fragments.push_back(left);
			}
			if (old_end > end) {
				Mapping right = old;
				right.guest_base = end;
				right.size = old_end - end;
				right.backing_offset += end - old.guest_base;
				fragments.push_back(right);
			}
			it = replacement.erase(it);
		}
		for (const auto& fragment: fragments) {
			replacement.emplace(fragment.guest_base, fragment);
		}
		m_mappings.swap(replacement);
	} catch (const std::bad_alloc&) {
		return GuestMemoryStatus::HostAllocationFailed;
	}
	++m_generation;
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::Protect(GuestVirtualAddress guest_base, GuestMemorySize size,
                                            GuestMemoryPermission permissions) {
	if (!IsRangeValid(guest_base, size)) {
		return GuestMemoryStatus::InvalidAlignment;
	}
	std::lock_guard lock(m_mutex);
	if (!IsMappedUnlocked(guest_base, size, GuestMemoryPermission::None)) {
		return GuestMemoryStatus::AddressNotMapped;
	}

	const auto end = guest_base + size;
	try {
		MappingMap replacement = m_mappings;
		std::vector<Mapping> fragments;
		auto it = replacement.lower_bound(guest_base);
		if (it != replacement.begin()) {
			auto previous = std::prev(it);
			if (previous->second.guest_base + previous->second.size > guest_base) {
				it = previous;
			}
		}
		while (it != replacement.end() && it->second.guest_base < end) {
			const Mapping old = it->second;
			const auto old_end = old.guest_base + old.size;
			const auto middle_start = std::max(old.guest_base, guest_base);
			const auto middle_end = std::min(old_end, end);
			if (old.guest_base < middle_start) {
				Mapping left = old;
				left.size = middle_start - old.guest_base;
				fragments.push_back(left);
			}
			Mapping middle = old;
			middle.guest_base = middle_start;
			middle.size = middle_end - middle_start;
			middle.backing_offset += middle_start - old.guest_base;
			middle.permissions = permissions;
			fragments.push_back(middle);
			if (old_end > middle_end) {
				Mapping right = old;
				right.guest_base = middle_end;
				right.size = old_end - middle_end;
				right.backing_offset += middle_end - old.guest_base;
				fragments.push_back(right);
			}
			it = replacement.erase(it);
		}
		for (const auto& fragment: fragments) {
			replacement.emplace(fragment.guest_base, fragment);
		}
		m_mappings.swap(replacement);
	} catch (const std::bad_alloc&) {
		return GuestMemoryStatus::HostAllocationFailed;
	}
	++m_generation;
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::Translate(GuestVirtualAddress guest_address,
                                              GuestMemorySize minimum_size,
                                              GuestMemoryPermission required,
                                              GuestMemoryTranslation* out) const {
	if (out == nullptr || guest_address == 0 || minimum_size == 0 ||
	    AddOverflows(guest_address, minimum_size)) {
		return GuestMemoryStatus::InvalidArgument;
	}
	std::lock_guard lock(m_mutex);
	auto mapping = FindMappingUnlocked(guest_address);
	if (mapping == m_mappings.end()) {
		return GuestMemoryStatus::AddressNotMapped;
	}
	const auto& first = mapping->second;
	if (!HasPermissions(first.permissions, required)) {
		return GuestMemoryStatus::PermissionDenied;
	}
	const auto first_offset = first.backing_offset + guest_address - first.guest_base;
	GuestMemorySize contiguous = first.guest_base + first.size - guest_address;
	auto next = std::next(mapping);
	while (next != m_mappings.end() &&
	       next->second.guest_base == guest_address + contiguous &&
	       next->second.backing == first.backing &&
	       next->second.backing_offset == first_offset + contiguous &&
	       next->second.permissions == first.permissions) {
		contiguous += next->second.size;
		++next;
	}
	if (contiguous < minimum_size) {
		return GuestMemoryStatus::AddressNotMapped;
	}
	*out = {first.backing, first_offset, contiguous, first.permissions, m_generation};
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::Read(GuestVirtualAddress guest_address, void* destination,
                                         GuestMemorySize size) const {
	if (size == 0) {
		return GuestMemoryStatus::Success;
	}
	if (guest_address == 0 || destination == nullptr || AddOverflows(guest_address, size)) {
		return GuestMemoryStatus::InvalidArgument;
	}
	std::lock_guard lock(m_mutex);
	if (!IsMappedUnlocked(guest_address, size, GuestMemoryPermission::None)) {
		return GuestMemoryStatus::AddressNotMapped;
	}
	if (!IsMappedUnlocked(guest_address, size, GuestMemoryPermission::CpuRead)) {
		return GuestMemoryStatus::PermissionDenied;
	}
	auto* output = static_cast<std::byte*>(destination);
	auto cursor = guest_address;
	GuestMemorySize copied = 0;
	while (copied < size) {
		const auto mapping = FindMappingUnlocked(cursor);
		const auto& entry = mapping->second;
		const auto offset = entry.backing_offset + cursor - entry.guest_base;
		const auto chunk = std::min(size - copied, entry.guest_base + entry.size - cursor);
		const auto backing = m_backings.find(entry.backing.value);
		if (backing == m_backings.end()) {
			return GuestMemoryStatus::BackingNotFound;
		}
		std::memcpy(output + static_cast<size_t>(copied),
		            backing->second.data() + static_cast<size_t>(offset),
		            static_cast<size_t>(chunk));
		cursor += chunk;
		copied += chunk;
	}
	return GuestMemoryStatus::Success;
}

GuestMemoryStatus GuestMemoryModel::Write(GuestVirtualAddress guest_address, const void* source,
                                          GuestMemorySize size) {
	if (size == 0) {
		return GuestMemoryStatus::Success;
	}
	if (guest_address == 0 || source == nullptr || AddOverflows(guest_address, size)) {
		return GuestMemoryStatus::InvalidArgument;
	}
	std::lock_guard lock(m_mutex);
	if (!IsMappedUnlocked(guest_address, size, GuestMemoryPermission::None)) {
		return GuestMemoryStatus::AddressNotMapped;
	}
	if (!IsMappedUnlocked(guest_address, size, GuestMemoryPermission::CpuWrite)) {
		return GuestMemoryStatus::PermissionDenied;
	}
	const auto* input = static_cast<const std::byte*>(source);
	auto cursor = guest_address;
	GuestMemorySize copied = 0;
	while (copied < size) {
		const auto mapping = FindMappingUnlocked(cursor);
		const auto& entry = mapping->second;
		const auto offset = entry.backing_offset + cursor - entry.guest_base;
		const auto chunk = std::min(size - copied, entry.guest_base + entry.size - cursor);
		auto backing = m_backings.find(entry.backing.value);
		if (backing == m_backings.end()) {
			return GuestMemoryStatus::BackingNotFound;
		}
		std::memcpy(backing->second.data() + static_cast<size_t>(offset),
		            input + static_cast<size_t>(copied),
		            static_cast<size_t>(chunk));
		cursor += chunk;
		copied += chunk;
	}
	return GuestMemoryStatus::Success;
}

uint64_t GuestMemoryModel::GetGeneration() const noexcept {
	std::lock_guard lock(m_mutex);
	return m_generation;
}

} // namespace Libs::LibKernel::Memory::GuestMemory
