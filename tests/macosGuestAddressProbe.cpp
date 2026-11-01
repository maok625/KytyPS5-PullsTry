#include <mach/error.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdint>
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

	while (cursor < end && entries < 64) {
		mach_vm_address_t region_address = cursor;
		mach_vm_size_t   region_size = 0;
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t object_name = MACH_PORT_NULL;

		const kern_return_t result = mach_vm_region(
		    mach_task_self(), &region_address, &region_size, VM_REGION_BASIC_INFO_64,
		    reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);
		if (object_name != MACH_PORT_NULL) {
			mach_port_deallocate(mach_task_self(), object_name);
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
