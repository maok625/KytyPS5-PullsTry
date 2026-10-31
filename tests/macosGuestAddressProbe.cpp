#include <mach/mach.h>
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

uint64_t ReadHostPageSize() {
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
	mach_vm_size_t vm_page_size = 0;
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

} // namespace

int main() {
	utsname system_name {};
	if (uname(&system_name) == 0) {
		std::printf("system=%s release=%s machine=%s\n",
		            system_name.sysname, system_name.release, system_name.machine);
	}
	PrintArchitecture();

	const uint64_t sysctl_page_size = ReadHostPageSize();
	mach_vm_size_t mach_page_size = 0;
	const kern_return_t mach_page_result = host_page_size(mach_host_self(), &mach_page_size);
	std::printf("sysctl_hw_pagesize=%" PRIu64 "\n", sysctl_page_size);
	if (mach_page_result == KERN_SUCCESS) {
		std::printf("mach_host_page_size=%" PRIu64 "\n",
		            static_cast<uint64_t>(mach_page_size));
	} else {
		std::printf("mach_host_page_size_error=%s (%d)\n",
		            mach_error_string(mach_page_result), mach_page_result);
	}
	if (sysctl_page_size == 0 || mach_page_result != KERN_SUCCESS ||
	    sysctl_page_size != static_cast<uint64_t>(mach_page_size)) {
		std::fprintf(stderr, "host page-size sources disagree or are unavailable\n");
		return 2;
	}

	std::fflush(stdout);
	const pid_t child = fork();
	if (child < 0) {
		std::perror("fork");
		return 2;
	}
	if (child == 0) {
		const int result = RunFixedAddressProbe(sysctl_page_size);
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
