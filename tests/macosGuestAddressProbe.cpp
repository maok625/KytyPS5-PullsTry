#include <mach/error.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach/vm_statistics.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr mach_vm_address_t ProbeAddress = 0x15b1400000ull;

uint64_t ReadSysctlPageSize() {
	uint64_t value = 0;
	size_t size = sizeof(value);
	if (sysctlbyname("hw.pagesize", &value, &size, nullptr, 0) != 0) {
		std::fprintf(stderr, "sysctl(hw.pagesize) failed: errno=%d\n", errno);
		return 0;
	}
	return value;
}

void PrintArchitecture() {
#if defined(__arm64__) || defined(__aarch64__)
	std::printf("process_architecture=arm64\n");
#elif defined(__x86_64__)
	std::printf("process_architecture=x86_64\n");
#else
	std::printf("process_architecture=unknown\n");
#endif
}

void PrintTargetRegionState() {
	mach_vm_address_t address = ProbeAddress;
	mach_vm_size_t region_size = 0;
	natural_t depth = 0;
	vm_region_submap_short_info_data_64_t info {};

	for (unsigned int level = 0; level < 32; ++level) {
		mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
		const kern_return_t result = mach_vm_region_recurse(
		    mach_task_self(), &address, &region_size, &depth,
		    reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
		if (result != KERN_SUCCESS) {
			std::printf("target_region=query_failed (%s)\n", mach_error_string(result));
			return;
		}
		if (info.is_submap) {
			address = ProbeAddress;
			++depth;
			continue;
		}
		if (address <= ProbeAddress && ProbeAddress - address < region_size) {
			std::printf("target_region=occupied region_base=0x%016" PRIx64
			            " region_size=%" PRIu64 "\n",
			            static_cast<uint64_t>(address), static_cast<uint64_t>(region_size));
		} else if (address > ProbeAddress) {
			std::printf("target_region=gap_until_next_region next_base=0x%016" PRIx64 "\n",
			            static_cast<uint64_t>(address));
		} else {
			std::printf("target_region=not_classified\n");
		}
		return;
	}
	std::printf("target_region=query_depth_limit_reached\n");
}

int RunFixedAddressProbe(uint64_t expected_page_size) {
	vm_size_t vm_page_size = 0;
	const kern_return_t page_result = host_page_size(mach_host_self(), &vm_page_size);
	if (page_result != KERN_SUCCESS || vm_page_size == 0) {
		std::fprintf(stderr, "host_page_size failed: %s (%d)\n",
		             mach_error_string(page_result), page_result);
		return 2;
	}
	if (static_cast<uint64_t>(vm_page_size) != expected_page_size) {
		std::fprintf(stderr, "host page size changed between parent and child\n");
		return 2;
	}
	if (ProbeAddress % vm_page_size != 0) {
		std::fprintf(stderr, "probe address is not aligned to VM page size (%" PRIu64 ")\n",
		             static_cast<uint64_t>(vm_page_size));
		return 2;
	}

	PrintTargetRegionState();

	// VM_FLAGS_FIXED is zero. Crucially, VM_FLAGS_OVERWRITE is not supplied:
	// an existing mapping is not replaced by this diagnostic allocation.
	mach_vm_address_t actual = ProbeAddress;
	const kern_return_t result =
	    mach_vm_allocate(mach_task_self(), &actual, vm_page_size, VM_FLAGS_FIXED);
	if (result != KERN_SUCCESS) {
		std::printf("fixed_address_probe=unavailable\n");
		std::printf("requested=0x%016" PRIx64 " size=%" PRIu64 "\n",
		            static_cast<uint64_t>(ProbeAddress), static_cast<uint64_t>(vm_page_size));
		std::printf("mach_result=%d message=%s\n", result, mach_error_string(result));
		return 0; // Refusal is a diagnostic result, not a test crash.
	}

	const bool exact = actual == ProbeAddress;
	std::printf("fixed_address_probe=%s\n", exact ? "reserved_exactly" : "returned_different_address");
	std::printf("requested=0x%016" PRIx64 " returned=0x%016" PRIx64 " size=%" PRIu64 "\n",
	            static_cast<uint64_t>(ProbeAddress), static_cast<uint64_t>(actual),
	            static_cast<uint64_t>(vm_page_size));
	const kern_return_t deallocate_result =
	    mach_vm_deallocate(mach_task_self(), actual, vm_page_size);
	if (deallocate_result != KERN_SUCCESS) {
		std::fprintf(stderr, "probe cleanup failed: %s (%d)\n",
		             mach_error_string(deallocate_result), deallocate_result);
		return 3;
	}
	std::printf("probe_cleanup=success\n");
	return exact ? 0 : 4;
}


int ProbeMachFixedRange(const char* label, mach_vm_address_t start,
                        mach_vm_address_t end, uint64_t page_size) {
	if (end <= start || page_size == 0 || start % page_size != 0 ||
	    (end - start) % page_size != 0) {
		std::fprintf(stderr, "%s has invalid page alignment or size\n", label);
		return 2;
	}

	const mach_vm_size_t size = end - start;
	mach_vm_address_t actual = start;
	const kern_return_t result =
	    mach_vm_allocate(mach_task_self(), &actual, size, VM_FLAGS_FIXED);
	if (result != KERN_SUCCESS) {
		std::printf("%s=unavailable\n", label);
		std::printf("%s_start=0x%016" PRIx64 " size=%" PRIu64 "\n",
		            label, static_cast<uint64_t>(start), static_cast<uint64_t>(size));
		std::printf("%s_mach_result=%d message=%s\n",
		            label, result, mach_error_string(result));
		return 0; // A refused non-overwriting probe is diagnostic, not a test failure.
	}

	if (actual != start) {
		std::fprintf(stderr, "%s returned an unexpected address\n", label);
		const kern_return_t cleanup = mach_vm_deallocate(mach_task_self(), actual, size);
		return cleanup == KERN_SUCCESS ? 4 : 3;
	}

	std::printf("%s=available\n", label);
	std::printf("%s_start=0x%016" PRIx64 " size=%" PRIu64 "\n",
	            label, static_cast<uint64_t>(start), static_cast<uint64_t>(size));
	const kern_return_t cleanup = mach_vm_deallocate(mach_task_self(), actual, size);
	if (cleanup != KERN_SUCCESS) {
		std::fprintf(stderr, "%s cleanup failed: %s (%d)\n",
		             label, mach_error_string(cleanup), cleanup);
		return 3;
	}
	std::printf("%s_cleanup=success\n", label);
	return 0;
}

void PrintMachRangeLayout(const char* label, mach_vm_address_t start,
                           mach_vm_address_t end) {
	std::printf("%s_layout_begin=0x%016" PRIx64 " end=0x%016" PRIx64 "\n",
	            label, static_cast<uint64_t>(start), static_cast<uint64_t>(end));
	mach_vm_address_t cursor = start;
	unsigned int      entries = 0;

	while (cursor < end && entries < 128) {
		mach_vm_address_t region_address = cursor;
		mach_vm_size_t    region_size = 0;
		natural_t         depth = 0;
		vm_region_submap_info_data_64_t info {};

		kern_return_t result = KERN_SUCCESS;
		for (;;) {
			mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
			result = mach_vm_region_recurse(
			    mach_task_self(), &region_address, &region_size, &depth,
			    reinterpret_cast<vm_region_recurse_info_t>(&info), &info_count);
			if (result != KERN_SUCCESS || !info.is_submap) {
				break;
			}
			// Descend into the returned submap at the first address in our scan range.
			region_address = std::max(region_address, cursor);
			++depth;
		}

		if (result == KERN_INVALID_ADDRESS) {
			std::printf("%s_free=0x%016" PRIx64 " size=%" PRIu64 "\n",
			            label, static_cast<uint64_t>(cursor),
			            static_cast<uint64_t>(end - cursor));
			cursor = end;
			break;
		}
		if (result != KERN_SUCCESS || region_size == 0 ||
		    UINT64_MAX - region_address < region_size) {
			std::printf("%s_layout_error=%s (%d) at=0x%016" PRIx64 "\n",
			            label, mach_error_string(result), result,
			            static_cast<uint64_t>(cursor));
			break;
		}

		if (region_address > cursor) {
			const auto gap_end = std::min<mach_vm_address_t>(region_address, end);
			std::printf("%s_free=0x%016" PRIx64 " size=%" PRIu64 "\n",
			            label, static_cast<uint64_t>(cursor),
			            static_cast<uint64_t>(gap_end - cursor));
			cursor = gap_end;
			if (cursor == end) {
				break;
			}
		}

		const auto region_end = region_address + region_size;
		if (region_end <= cursor) {
			std::printf("%s_layout_error=region_scan_did_not_advance at=0x%016" PRIx64 "\n",
			            label, static_cast<uint64_t>(cursor));
			break;
		}
		const auto occupied_end = std::min<mach_vm_address_t>(region_end, end);
		std::printf("%s_occupied=0x%016" PRIx64 " size=%" PRIu64 "\n",
		            label, static_cast<uint64_t>(cursor),
		            static_cast<uint64_t>(occupied_end - cursor));
		cursor = occupied_end;
		++entries;
	}
	if (cursor < end) {
		std::printf("%s_layout_truncated=0x%016" PRIx64 " remaining=%" PRIu64 "\n",
		            label, static_cast<uint64_t>(cursor),
		            static_cast<uint64_t>(end - cursor));
	}
	std::printf("%s_layout_end\n", label);
}

int RunCandidateRegionProbes(uint64_t page_size) {
	// These boundaries mirror Kyty's existing macOS x86_64 linker layout.
	// First inspect the existing VM map; then probe only individual pages.
	constexpr mach_vm_address_t managed_start = 0x0000040000ull;
	constexpr mach_vm_address_t managed_end = 0x07ffffc000ull;
	constexpr mach_vm_address_t reserved_start = managed_end;
	constexpr mach_vm_address_t reserved_end = 0x0fc0000000ull;
	constexpr mach_vm_address_t user_start = 0x7000000000ull;
	constexpr mach_vm_address_t user_end = 0xfc00000000ull;

	PrintMachRangeLayout("candidate_system_managed", managed_start, managed_end);
	PrintMachRangeLayout("candidate_system_reserved", reserved_start, reserved_end);
	PrintMachRangeLayout("candidate_user", user_start, user_end);

	const mach_vm_address_t samples[] = {
	    managed_start,
	    managed_start + ((managed_end - managed_start) / (2 * page_size)) * page_size,
	    managed_end - page_size,
	    reserved_start,
	    reserved_start + ((reserved_end - reserved_start) / (2 * page_size)) * page_size,
	    reserved_end - page_size,
	    user_start,
	    user_start + (user_end - user_start) / 2,
	    user_end - page_size,
	};
	const char* labels[] = {
	    "candidate_managed_first_page",
	    "candidate_managed_middle_page",
	    "candidate_managed_last_page",
	    "candidate_reserved_first_page",
	    "candidate_reserved_middle_page",
	    "candidate_reserved_last_page",
	    "candidate_user_first_page",
	    "candidate_user_middle_page",
	    "candidate_user_last_page",
	};

	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
		const int result = ProbeMachFixedRange(labels[i], samples[i],
		                                       samples[i] + page_size, page_size);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}


int RunContiguousUserArenaProbes(uint64_t page_size) {
	// This interval was reported as a large free user-address gap by the M4
	// probe. Every request is fixed and non-overwriting; successful mappings
	// are immediately released before the next (larger) request.
	constexpr mach_vm_address_t arena_start = 0x0000007bec000000ull;
	constexpr mach_vm_address_t arena_end = 0x000000fc00000000ull;
	constexpr uint64_t gib = 1024ull * 1024ull * 1024ull;
	const uint64_t sizes_gib[] = {1, 4, 16, 64, 128, 256, 512};

	std::printf("contiguous_user_arena_start=0x%016" PRIx64
	            " end=0x%016" PRIx64 " window_bytes=%" PRIu64 "\n",
	            static_cast<uint64_t>(arena_start),
	            static_cast<uint64_t>(arena_end),
	            static_cast<uint64_t>(arena_end - arena_start));

	for (const uint64_t size_gib : sizes_gib) {
		const mach_vm_size_t size = static_cast<mach_vm_size_t>(size_gib * gib);
		if (size > arena_end - arena_start) {
			std::printf("contiguous_user_arena_%" PRIu64 "_gib=skipped_window_too_small\n",
			            size_gib);
			continue;
		}

		char label[64] {};
		const int label_size = std::snprintf(
		    label, sizeof(label), "contiguous_user_arena_%llu_gib",
		    static_cast<unsigned long long>(size_gib));
		if (label_size < 0 || static_cast<size_t>(label_size) >= sizeof(label)) {
			std::fprintf(stderr, "could not format contiguous reservation probe label\n");
			return 2;
		}

		const int result = ProbeMachFixedRange(label, arena_start,
		                                       arena_start + size, page_size);
		if (result != 0) {
			return result;
		}
	}
	return 0;
}


int RunArenaBackingAliasProbe(uint64_t page_size) {
	constexpr uint64_t gib = 1024ull * 1024ull * 1024ull;
	constexpr mach_vm_size_t arena_size = 512ull * gib;
	constexpr uint64_t first_offset = 0x01000000ull;
	constexpr uint64_t second_offset = 0x02000000ull;

	if (page_size == 0 || first_offset % page_size != 0 ||
	    second_offset % page_size != 0 || second_offset + page_size > arena_size) {
		std::fprintf(stderr, "arena backing-alias probe has invalid page alignment\n");
		return 2;
	}

	// Prefer a dynamically selected host VA. This is the behavior wanted by
	// the ARM64 backend: reserve host space without assuming it equals a guest VA.
	mach_vm_address_t arena = 0;
	const kern_return_t reserve_result =
	    mach_vm_allocate(mach_task_self(), &arena, arena_size, VM_FLAGS_ANYWHERE);
	if (reserve_result != KERN_SUCCESS || arena == 0 || arena % page_size != 0) {
		std::printf("arena_backing_alias_probe=skipped_reservation_unavailable\n");
		std::printf("arena_backing_alias_reserve_result=%d message=%s\n",
		            reserve_result, mach_error_string(reserve_result));
		if (reserve_result == KERN_SUCCESS && arena != 0) {
			(void)mach_vm_deallocate(mach_task_self(), arena, arena_size);
		}
		return 0;
	}
	std::printf("arena_backing_alias_arena=reserved_anywhere start=0x%016" PRIx64
	            " size=%" PRIu64 "\n",
	            static_cast<uint64_t>(arena), static_cast<uint64_t>(arena_size));

	char shm_name[64] {};
	const int name_size = std::snprintf(
	    shm_name, sizeof(shm_name), "/ka-%d-%x", static_cast<int>(getpid()),
	    static_cast<unsigned int>(reinterpret_cast<uintptr_t>(&arena) & 0xffffffffu));
	if (name_size < 0 || static_cast<size_t>(name_size) >= sizeof(shm_name)) {
		(void)mach_vm_deallocate(mach_task_self(), arena, arena_size);
		std::fprintf(stderr, "could not format shared-memory name\n");
		return 2;
	}

	const int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		std::printf("arena_backing_alias_probe=failed_shm_open errno=%d\n", errno);
		(void)mach_vm_deallocate(mach_task_self(), arena, arena_size);
		return 2;
	}
	(void)shm_unlink(shm_name);

	int result = 0;
	uint8_t* backing = nullptr;
	void* view_a = MAP_FAILED;
	void* view_b = MAP_FAILED;

	if (ftruncate(fd, static_cast<off_t>(page_size)) != 0) {
		std::printf("arena_backing_alias_probe=failed_ftruncate errno=%d\n", errno);
		result = 2;
	} else {
		void* mapped_backing = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (mapped_backing == MAP_FAILED) {
			std::printf("arena_backing_alias_probe=failed_backing_map errno=%d\n", errno);
			result = 2;
		} else {
			backing = static_cast<uint8_t*>(mapped_backing);
			const auto map_view = [&](uint64_t offset) -> void* {
				void* address = reinterpret_cast<void*>(arena + offset);
				void* mapped = mmap(address, page_size, PROT_READ | PROT_WRITE,
				                    MAP_SHARED | MAP_FIXED, fd, 0);
				if (mapped == MAP_FAILED || mapped != address) {
					std::printf("arena_backing_alias_view=unavailable offset=0x%016" PRIx64
					            " errno=%d\n",
					            offset, errno);
					return MAP_FAILED;
				}
				return mapped;
			};

			view_a = map_view(first_offset);
			if (view_a != MAP_FAILED) {
				view_b = map_view(second_offset);
			}
			if (view_a != MAP_FAILED && view_b != MAP_FAILED) {
				static constexpr char pattern[] = "KytyPS5 shared backing alias probe";
				std::memcpy(view_a, pattern, sizeof(pattern));
				const bool alias_a_matches = std::memcmp(backing, pattern, sizeof(pattern)) == 0;
				const bool alias_b_matches = std::memcmp(view_b, pattern, sizeof(pattern)) == 0;
				std::printf("arena_backing_alias_coherence=%s first=%s second=%s\n",
				            alias_a_matches && alias_b_matches ? "success" : "failed",
				            alias_a_matches ? "success" : "failed",
				            alias_b_matches ? "success" : "failed");
				if (!alias_a_matches || !alias_b_matches) {
					result = 1;
				}

				const int protect_result = mprotect(view_a, page_size, PROT_READ);
				const int restore_protect_result =
				    protect_result == 0 ? mprotect(view_a, page_size, PROT_READ | PROT_WRITE) : -1;
				std::printf("arena_backing_alias_protect=%s restore=%s\n",
				            protect_result == 0 ? "success" : "failed",
				            restore_protect_result == 0 ? "success" : "failed");
				if (protect_result != 0 || restore_protect_result != 0) {
					result = 1;
				}
			} else {
				result = 1;
			}
		}
	}

	const auto restore_placeholder = [&](void* view, const char* label) {
		if (view == MAP_FAILED) {
			return;
		}
		void* restored = mmap(view, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		if (restored == MAP_FAILED || restored != view) {
			std::printf("%s=failed errno=%d\n", label, errno);
			result = 2;
		} else {
			std::printf("%s=success\n", label);
		}
	};
	restore_placeholder(view_a, "arena_backing_alias_restore_first_placeholder");
	restore_placeholder(view_b, "arena_backing_alias_restore_second_placeholder");

	if (backing != nullptr && munmap(backing, page_size) != 0) {
		std::printf("arena_backing_alias_unmap_backing=failed errno=%d\n", errno);
		result = 2;
	}
	if (close(fd) != 0) {
		std::printf("arena_backing_alias_close_fd=failed errno=%d\n", errno);
		result = 2;
	}
	const kern_return_t release_result = mach_vm_deallocate(mach_task_self(), arena, arena_size);
	std::printf("arena_backing_alias_release=%s\n",
	            release_result == KERN_SUCCESS ? "success" : "failed");
	if (release_result != KERN_SUCCESS) {
		std::printf("arena_backing_alias_release_error=%d message=%s\n",
		            release_result, mach_error_string(release_result));
		result = 2;
	}
	return result;
}

} // namespace

int main() {
	utsname system_name {};
	if (uname(&system_name) == 0) {
		std::printf("system=%s release=%s machine=%s\n",
		            system_name.sysname, system_name.release, system_name.machine);
	}
	PrintArchitecture();

	const uint64_t sysctl_page_size = ReadSysctlPageSize();
	// host_page_size() is the authoritative granularity for Mach VM operations.
	// hw.pagesize is reported separately because it can differ on some macOS
	// runner configurations (for example, x86_64 CI).
	vm_size_t mach_page_size = 0;
	const kern_return_t mach_page_result = host_page_size(mach_host_self(), &mach_page_size);
	std::printf("sysctl_hw_pagesize=%" PRIu64 "\n", sysctl_page_size);
	if (mach_page_result == KERN_SUCCESS) {
		std::printf("mach_host_page_size=%" PRIu64 "\n",
		            static_cast<uint64_t>(mach_page_size));
	} else {
		std::printf("mach_host_page_size_error=%s (%d)\n",
		            mach_error_string(mach_page_result), mach_page_result);
	}
	if (mach_page_result != KERN_SUCCESS || mach_page_size == 0) {
		std::fprintf(stderr, "Mach host page size is unavailable\n");
		return 2;
	}
	if (sysctl_page_size == 0) {
		std::printf("sysctl_page_size_status=unavailable; using Mach host page size\n");
	} else if (sysctl_page_size != static_cast<uint64_t>(mach_page_size)) {
		std::printf("page_size_sources_differ=using_mach_host_page_size\n");
	}

	std::fflush(stdout);
	const pid_t child = fork();
	if (child < 0) {
		std::perror("fork");
		return 2;
	}
	if (child == 0) {
		int result = RunFixedAddressProbe(static_cast<uint64_t>(mach_page_size));
		if (result == 0) {
			result = RunCandidateRegionProbes(static_cast<uint64_t>(mach_page_size));
		}
		if (result == 0) {
			result = RunContiguousUserArenaProbes(static_cast<uint64_t>(mach_page_size));
		}
		if (result == 0) {
			result = RunArenaBackingAliasProbe(static_cast<uint64_t>(mach_page_size));
		}
		std::fflush(stdout);
		_exit(result);
	}

	int status = 0;
	if (waitpid(child, &status, 0) != child) {
		std::perror("waitpid");
		return 2;
	}
	if (!WIFEXITED(status)) {
		std::fprintf(stderr, "probe child did not exit normally\n");
		return 2;
	}
	const int child_status = WEXITSTATUS(status);
	std::printf("probe_child_exit=%d\n", child_status);
	return child_status == 0 ? 0 : 2;
}
