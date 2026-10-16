#pragma once

#include "native_arm64/translator_interface.h"

#include <cstddef>
#include <cstdint>

namespace Kyty::NativeArm64 {

// This is deliberately a small bring-up backend, not a complete x86-64 CPU.
// Unsupported instructions stop at their original RIP so the caller can route
// them to a future translator or report a precise compatibility failure.
class X64Interpreter final : public GuestTranslator {
public:
	[[nodiscard]] const char* BackendName() const noexcept override;
	[[nodiscard]] bool IsSupported() const noexcept override;

	BlockExit RunBlock(GuestCpuState& state, GuestMemory& memory,
	                   std::uint64_t instruction_budget) override;

	void InvalidateCodeRange(std::uint64_t guest_address,
	                         std::uint64_t size) override;

	// Execute one instruction from GuestCpuState::rip. On unsupported opcodes
	// or memory faults, the architectural state is left at the faulting RIP.
	InstructionStep ExecuteOne(GuestCpuState& state, GuestMemory& memory) const;
};

} // namespace Kyty::NativeArm64
