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

enum class InstructionStepStatus : std::uint8_t {
	Continue,
	ControlFlow,
	SystemCall,
	GuestException,
	MemoryFault,
	UnsupportedInstruction,
};

struct InstructionStep {
	InstructionStepStatus status = InstructionStepStatus::Continue;
	std::uint64_t guest_rip = 0;
	std::uint64_t detail = 0;
};

// Guest memory is addressed in PS5 guest virtual-address space, never through
// a host pointer assumed to match that address. Implementations must check the
// guest mapping and return false for unmapped or inaccessible ranges.
class GuestMemory {
public:
	virtual ~GuestMemory() = default;
	[[nodiscard]] virtual bool Read(std::uint64_t guest_address, void* destination,
	                                std::size_t size) const noexcept = 0;
	[[nodiscard]] virtual bool Write(std::uint64_t guest_address, const void* source,
	                                 std::size_t size) noexcept = 0;

	// Native ARM64 execution must never jump into a host AArch64 function as if
	// its bytes were x86-64 instructions. The runtime can claim a call target
	// only when it knows that target is a registered host import or callback.
	// tail_call means the x86 instruction is JMP through an import thunk, so
	// the current guest return address is already at [RSP].
	virtual bool TryDispatchHostCall(std::uint64_t target, GuestCpuState& state,
	                                 bool tail_call) noexcept {
		(void)target;
		(void)state;
		(void)tail_call;
		return false;
	}
};

// Interface for an x86-64 guest execution backend. The first implementation is
// a deliberately small interpreter subset; a later AArch64 DBT can implement
// this same contract. Every backend must preserve guest state and return at
// syscalls, exceptions, unsupported instructions, and memory faults.
// Executable guest-memory writes must invalidate overlapping cached blocks.
class GuestTranslator {
public:
	virtual ~GuestTranslator() = default;

	[[nodiscard]] virtual const char* BackendName() const noexcept = 0;
	[[nodiscard]] virtual bool IsSupported() const noexcept = 0;

	virtual BlockExit RunBlock(GuestCpuState& state, GuestMemory& memory,
	                           std::uint64_t instruction_budget) = 0;

	virtual void InvalidateCodeRange(std::uint64_t guest_address,
	                                 std::uint64_t size) = 0;
};

} // namespace Kyty::NativeArm64
