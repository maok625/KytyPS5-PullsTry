#include "native_arm64/x64_interpreter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <vector>

using Kyty::NativeArm64::BlockExit;
using Kyty::NativeArm64::BlockExitReason;
using Kyty::NativeArm64::GuestCpuState;
using Kyty::NativeArm64::GuestGpr;
using Kyty::NativeArm64::GuestMemory;
using Kyty::NativeArm64::X64Interpreter;

#define CHECK(condition)                                                                     \
	do {                                                                                       \
		if (!(condition)) {                                                                      \
			std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n';        \
			return 1;                                                                              \
		}                                                                                        \
	} while (false)

static_assert(static_cast<std::size_t>(GuestGpr::Rax) == 0);
static_assert(static_cast<std::size_t>(GuestGpr::Rsp) == 4);
static_assert(static_cast<std::size_t>(GuestGpr::R15) == 15);

class TestMemory final : public GuestMemory {
public:
	static constexpr std::uint64_t kBase = 0x1000;

	TestMemory() : bytes_(0x2000, 0) {}

	bool Read(std::uint64_t address, void* destination, std::size_t size) const noexcept override {
		if (address < kBase) {
			return false;
		}
		const auto offset64 = address - kBase;
		if (offset64 > bytes_.size()) {
			return false;
		}
		const auto offset = static_cast<std::size_t>(offset64);
		if (size > bytes_.size() - offset) {
			return false;
		}
		std::memcpy(destination, bytes_.data() + offset, size);
		return true;
	}

	bool Write(std::uint64_t address, const void* source, std::size_t size) noexcept override {
		if (address < kBase) {
			return false;
		}
		const auto offset64 = address - kBase;
		if (offset64 > bytes_.size()) {
			return false;
		}
		const auto offset = static_cast<std::size_t>(offset64);
		if (size > bytes_.size() - offset) {
			return false;
		}
		std::memcpy(bytes_.data() + offset, source, size);
		return true;
	}

	bool Load(std::uint64_t address, std::initializer_list<std::uint8_t> bytes) noexcept {
		return Write(address, bytes.begin(), bytes.size());
	}

	bool WriteU64(std::uint64_t address, std::uint64_t value) noexcept {
		std::array<std::uint8_t, sizeof(value)> bytes{};
		for (std::size_t i = 0; i < bytes.size(); ++i) {
			bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
		}
		return Write(address, bytes.data(), bytes.size());
	}

	bool ReadU64(std::uint64_t address, std::uint64_t& value) const noexcept {
		std::array<std::uint8_t, sizeof(value)> bytes{};
		if (!Read(address, bytes.data(), bytes.size())) {
			return false;
		}
		value = 0;
		for (std::size_t i = 0; i < bytes.size(); ++i) {
			value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
		}
		return true;
	}

private:
	std::vector<std::uint8_t> bytes_;
};

int main() {
	GuestCpuState state{};
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x0000000123456000ULL;
	state.rip = 0x0000000140001000ULL;
	state.rflags = 1ULL << 1;
	state.fs_base = 0x0000000120000000ULL;
	state.ymm[3].qword[2] = 0x0123456789abcdefULL;
	state.mxcsr = 0x1f80;

	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x0000000123456000ULL);
	CHECK(state.rip == 0x0000000140001000ULL);
	CHECK(state.rflags == (1ULL << 1));
	CHECK(state.fs_base == 0x0000000120000000ULL);
	CHECK(state.ymm[3].qword[2] == 0x0123456789abcdefULL);
	CHECK(state.mxcsr == 0x1f80);

	const BlockExit exit{BlockExitReason::SystemCall, state.rip, 0x80};
	CHECK(exit.reason == BlockExitReason::SystemCall);
	CHECK(exit.guest_rip == state.rip);
	CHECK(exit.detail == 0x80);

	X64Interpreter interpreter;
	CHECK(interpreter.IsSupported());
	CHECK(interpreter.BackendName() != nullptr);

	// MOV RAX, 1; MOV RBX, 2; ADD RAX, RBX; RET.
	// The interpreter runs as ordinary host C++: no x86-64 code is executed by
	// the host and no Rosetta/JIT facility is needed.
	TestMemory memory;
	CHECK(memory.Load(0x1000, {
	    0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x48, 0xbb, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x48, 0x01, 0xd8,
	    0xc3,
	}));
	CHECK(memory.WriteU64(0x1800, 0x2222));
	state = GuestCpuState{};
	state.rip = 0x1000;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit add_exit = interpreter.RunBlock(state, memory, 8);
	CHECK(add_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 3);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x1808);
	CHECK(state.rip == 0x2222);
	CHECK((state.rflags & (1ULL << 2)) != 0); // even parity of 0x03
	CHECK((state.rflags & (1ULL << 0)) == 0); // carry clear
	CHECK((state.rflags & (1ULL << 6)) == 0); // zero clear

	// A 32-bit write to EAX zero-extends RAX.
	CHECK(memory.Load(0x1100, {0xb8, 0xff, 0xff, 0xff, 0xff}));
	state = GuestCpuState{};
	state.rip = 0x1100;
	const BlockExit mov32_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(mov32_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 0xffffffffULL);
	CHECK(state.rip == 0x1105);

	// SUB RAX, RBX with 1 - 2 wraps, setting CF and SF.
	CHECK(memory.Load(0x1200, {
	    0x48, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x48, 0xbb, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x48, 0x29, 0xd8,
	}));
	state = GuestCpuState{};
	state.rip = 0x1200;
	const BlockExit sub_exit = interpreter.RunBlock(state, memory, 3);
	CHECK(sub_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == UINT64_MAX);
	CHECK((state.rflags & (1ULL << 0)) != 0);
	CHECK((state.rflags & (1ULL << 7)) != 0);

	// Memory MOV and immediate ADD exercise base-register effective addresses.
	CHECK(memory.Load(0x1600, {
	    0x48, 0xb8, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // RAX = 0x1700
	    0x48, 0xbb, 0xef, 0xbe, 0xad, 0xde, 0x00, 0x00, 0x00, 0x00, // RBX = 0xdeadbeef
	    0x48, 0x89, 0x18,             // MOV [RAX], RBX
	    0x48, 0x8b, 0x08,             // MOV RCX, [RAX]
	    0x48, 0x83, 0x00, 0x01,       // ADD qword [RAX], 1
	    0x48, 0x8b, 0x10,             // MOV RDX, [RAX]
	    0xc3,
	}));
	CHECK(memory.WriteU64(0x1800, 0x2222));
	state = GuestCpuState{};
	state.rip = 0x1600;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit memory_exit = interpreter.RunBlock(state, memory, 10);
	CHECK(memory_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 0x1700);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rbx)] == 0xdeadbeef);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rcx)] == 0xdeadbeef);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rdx)] == 0xdeadbef0);
	std::uint64_t stored_value = 0;
	CHECK(memory.ReadU64(0x1700, stored_value));
	CHECK(stored_value == 0xdeadbef0);

	// C7 sign-extends imm32 when writing an r/m64 destination.
	CHECK(memory.Load(0x1f30, {
	    0x48, 0xb8, 0x18, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x48, 0xc7, 0x00, 0xff, 0xff, 0xff, 0xff,
	    0x48, 0x8b, 0x08,
	    0xc3,
	}));
	CHECK(memory.WriteU64(0x1800, 0x2222));
	state = GuestCpuState{};
	state.rip = 0x1f30;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit c7_exit = interpreter.RunBlock(state, memory, 6);
	CHECK(c7_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rcx)] == UINT64_MAX);
	CHECK(memory.ReadU64(0x1718, stored_value));
	CHECK(stored_value == UINT64_MAX);

	// SIB addressing uses base + index*scale + signed displacement.
	CHECK(memory.WriteU64(0x1710, 0xabcdef0123456789ULL));
	CHECK(memory.Load(0x1f50, {0x48, 0x8b, 0x44, 0x88, 0x08}));
	state = GuestCpuState{};
	state.rip = 0x1f50;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] = 0x1700;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rcx)] = 2;
	const BlockExit sib_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(sib_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 0xabcdef0123456789ULL);

	// RIP-relative LEA uses the address after the complete instruction.
	CHECK(memory.Load(0x1900, {0x48, 0x8d, 0x05, 0x09, 0x00, 0x00, 0x00}));
	state = GuestCpuState{};
	state.rip = 0x1900;
	const BlockExit lea_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(lea_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 0x1910);
	CHECK(state.rip == 0x1907);

	// XOR EAX,EAX sets ZF; JZ then skips the MOV EAX,1 sequence.
	CHECK(memory.Load(0x1a00, {
	    0x31, 0xc0,
	    0x74, 0x05,
	    0xb8, 0x01, 0x00, 0x00, 0x00,
	    0xb8, 0x02, 0x00, 0x00, 0x00,
	}));
	state = GuestCpuState{};
	state.rip = 0x1a00;
	const BlockExit condition_exit = interpreter.RunBlock(state, memory, 4);
	CHECK(condition_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.rip == 0x1a09);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 0);
	const BlockExit branch_target_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(branch_target_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 2);

	// PUSH RAX / POP RBX round-trips a 64-bit value on guest stack memory.
	CHECK(memory.Load(0x1b00, {0x50, 0x5b}));
	state = GuestCpuState{};
	state.rip = 0x1b00;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] = 0x123456789abcdef0ULL;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit stack_exit = interpreter.RunBlock(state, memory, 2);
	CHECK(stack_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x1800);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rbx)] == 0x123456789abcdef0ULL);

	// PUSH imm8 sign-extends to a 64-bit stack value in long mode.
	CHECK(memory.Load(0x1f60, {0x6a, 0xff}));
	state = GuestCpuState{};
	state.rip = 0x1f60;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit push_imm_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(push_imm_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x17f8);
	CHECK(memory.ReadU64(0x17f8, stored_value));
	CHECK(stored_value == UINT64_MAX);

	// MOVSXD sign-extends a 32-bit source when REX.W is set.
	CHECK(memory.Load(0x1f70, {0x48, 0x63, 0xc0}));
	state = GuestCpuState{};
	state.rip = 0x1f70;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] = 0x80000000ULL;
	const BlockExit movsxd_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(movsxd_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 0xffffffff80000000ULL);

	// LEAVE restores RBP from the guest frame and advances RSP atomically.
	CHECK(memory.Load(0x1b10, {0xc9}));
	CHECK(memory.WriteU64(0x1700, 0x1999));
	state = GuestCpuState{};
	state.rip = 0x1b10;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rbp)] = 0x1700;
	const BlockExit leave_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(leave_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x1708);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rbp)] == 0x1999);
	CHECK(state.rip == 0x1b11);

	// RET imm16 adds the immediate cleanup to the popped return address.
	CHECK(memory.Load(0x1b20, {0xc2, 0x08, 0x00}));
	CHECK(memory.WriteU64(0x1800, 0x3333));
	state = GuestCpuState{};
	state.rip = 0x1b20;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit ret_imm_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(ret_imm_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.rip == 0x3333);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x1810);

	// Direct CALL rel32 pushes the address of the next guest instruction.
	CHECK(memory.Load(0x1f00, {0xe8, 0x05, 0x00, 0x00, 0x00}));
	state = GuestCpuState{};
	state.rip = 0x1f00;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit direct_call_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(direct_call_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.rip == 0x1f0a);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x17f8);
	CHECK(memory.ReadU64(0x17f8, stored_value));
	CHECK(stored_value == 0x1f05);

	// Indirect CALL RAX and JMP RAX honor the guest register target.
	CHECK(memory.Load(0x1f10, {0xff, 0xd0}));
	state = GuestCpuState{};
	state.rip = 0x1f10;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] = 0x2222;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit indirect_call_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(indirect_call_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.rip == 0x2222);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x17f8);
	CHECK(memory.ReadU64(0x17f8, stored_value));
	CHECK(stored_value == 0x1f12);

	CHECK(memory.Load(0x1f20, {0xff, 0xe0}));
	state = GuestCpuState{};
	state.rip = 0x1f20;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] = 0x3333;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x1800;
	const BlockExit indirect_jump_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(indirect_jump_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.rip == 0x3333);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x1800);

	// FS segment override is added to the effective address in long mode.
	CHECK(memory.WriteU64(0x1810, 0xfeedfacecafebeefULL));
	CHECK(memory.Load(0x1c00, {0x64, 0x48, 0x8b, 0x00})); // MOV RAX, FS:[RAX]
	state = GuestCpuState{};
	state.rip = 0x1c00;
	state.fs_base = 0x110;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] = 0x1700;
	const BlockExit fs_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(fs_exit.reason == BlockExitReason::InstructionBudget);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] == 0xfeedfacecafebeefULL);

	// JMP short has a signed displacement relative to the following instruction.
	CHECK(memory.Load(0x1300, {0xeb, 0x02, 0x90, 0x90, 0x90}));
	state = GuestCpuState{};
	state.rip = 0x1300;
	const BlockExit jump_exit = interpreter.RunBlock(state, memory, 4);
	CHECK(jump_exit.reason == BlockExitReason::BlockCompleted);
	CHECK(state.rip == 0x1304);

	// Unsupported instructions stop at the original RIP for a future backend.
	CHECK(memory.Load(0x1400, {0x0f}));
	state = GuestCpuState{};
	state.rip = 0x1400;
	const BlockExit unsupported_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(unsupported_exit.reason == BlockExitReason::UnsupportedInstruction);
	CHECK(unsupported_exit.guest_rip == 0x1400);
	CHECK(unsupported_exit.detail == 0x0f00); // 0F escape opcode plus decoded second byte
	CHECK(state.rip == 0x1400);

	// SYSCALL returns to the guest kernel/dispatcher with x86-64 RCX/R11 effects.
	CHECK(memory.Load(0x1d00, {0x0f, 0x05}));
	state = GuestCpuState{};
	state.rip = 0x1d00;
	state.rflags = 0x246;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rax)] = 0x1234;
	const BlockExit syscall_exit = interpreter.RunBlock(state, memory, 2);
	CHECK(syscall_exit.reason == BlockExitReason::SystemCall);
	CHECK(syscall_exit.guest_rip == 0x1d02);
	CHECK(syscall_exit.detail == 0x1234);
	CHECK(state.rip == 0x1d02);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rcx)] == 0x1d02);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::R11)] == 0x246);

	// UD2 reports the x86 invalid-opcode exception without advancing RIP.
	CHECK(memory.Load(0x1e00, {0x0f, 0x0b}));
	state = GuestCpuState{};
	state.rip = 0x1e00;
	const BlockExit exception_exit = interpreter.RunBlock(state, memory, 1);
	CHECK(exception_exit.reason == BlockExitReason::GuestException);
	CHECK(exception_exit.guest_rip == 0x1e00);
	CHECK(exception_exit.detail == 6); // #UD
	CHECK(state.rip == 0x1e00);

	// Instruction fetch faults report the exact inaccessible guest address.
	state = GuestCpuState{};
	state.rip = 0x4000;
	const BlockExit fetch_fault = interpreter.RunBlock(state, memory, 1);
	CHECK(fetch_fault.reason == BlockExitReason::MemoryFault);
	CHECK(fetch_fault.guest_rip == 0x4000);
	CHECK(fetch_fault.detail == 0x4000);
	CHECK(state.rip == 0x4000);

	// RET must not advance RSP or RIP when reading the return address faults.
	CHECK(memory.Load(0x1500, {0xc3}));
	state = GuestCpuState{};
	state.rip = 0x1500;
	state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = 0x4000;
	const BlockExit ret_fault = interpreter.RunBlock(state, memory, 1);
	CHECK(ret_fault.reason == BlockExitReason::MemoryFault);
	CHECK(ret_fault.guest_rip == 0x1500);
	CHECK(ret_fault.detail == 0x4000);
	CHECK(state.rip == 0x1500);
	CHECK(state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] == 0x4000);

	return 0;
}
