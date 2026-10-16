#pragma once

#include "guest_cpu_state.h"

#include <cstdint>

namespace Kyty::NativeArm64 {

// A native translator must return to the emulator at observable guest
// boundaries. In particular, it must not silently execute a guest syscall or
// swallow a guest fault inside a translated block.
enum class BlockExitReason : std::uint8_t {
	BlockCompleted,
	SystemCall,
	GuestException,
	MemoryFault,
	UnsupportedInstruction,
	InstructionBudget,
};

struct BlockExit {
	BlockExitReason reason = BlockExitReason::BlockCompleted;
	std::uint64_t guest_rip = 0;
	std::uint64_t detail = 0;
};

// Contract for a future x86-64 guest -> AArch64 host backend.
//
// Implementations are responsible for translating guest instructions, caching
// host blocks, preserving x86 architectural state, and returning at syscalls,
// exceptions, and memory faults so Kyty's existing kernel/loader can handle
// those events. Executable guest-memory writes must invalidate overlapping
// cached blocks.
//
// No implementation is provided in this scaffold. Do not route live guest
// execution through this interface until a tested backend is available.
class GuestTranslator {
public:
	virtual ~GuestTranslator() = default;

	[[nodiscard]] virtual const char* BackendName() const noexcept = 0;
	[[nodiscard]] virtual bool IsSupported() const noexcept = 0;

	virtual BlockExit RunBlock(GuestCpuState& state,
	                           std::uint64_t instruction_budget) = 0;

	virtual void InvalidateCodeRange(std::uint64_t guest_address,
	                                 std::uint64_t size) = 0;
};

} // namespace Kyty::NativeArm64
