#include "native_arm64/translator_interface.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

using Kyty::NativeArm64::BlockExit;
using Kyty::NativeArm64::BlockExitReason;
using Kyty::NativeArm64::GuestCpuState;
using Kyty::NativeArm64::GuestGpr;

static_assert(static_cast<std::size_t>(GuestGpr::Rax) == 0);
static_assert(static_cast<std::size_t>(GuestGpr::Rsp) == 4);
static_assert(static_cast<std::size_t>(GuestGpr::R15) == 15);

int main() {
	GuestCpuState state{};

	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x0000000123456000ULL;
	state.rip = 0x0000000140001000ULL;
	state.rflags = 1ULL << 1;
	state.fs_base = 0x0000000120000000ULL;
	state.ymm[3].qword[2] = 0x0123456789abcdefULL;
	state.mxcsr = 0x1f80;

	assert(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x0000000123456000ULL);
	assert(state.rip == 0x0000000140001000ULL);
	assert(state.rflags == (1ULL << 1));
	assert(state.fs_base == 0x0000000120000000ULL);
	assert(state.ymm[3].qword[2] == 0x0123456789abcdefULL);
	assert(state.mxcsr == 0x1f80);

	const BlockExit exit{BlockExitReason::SystemCall, state.rip, 0x80};
	assert(exit.reason == BlockExitReason::SystemCall);
	assert(exit.guest_rip == state.rip);
	assert(exit.detail == 0x80);

	return 0;
}
