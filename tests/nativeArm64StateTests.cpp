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
			std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\\n';        \
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
	CHECK(unsupported_exit.detail == 0x0f);
	CHECK(state.rip == 0x1400);

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
