#include "native_arm64/x64_interpreter.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Kyty::NativeArm64 {
namespace {

constexpr std::uint64_t kFlagCarry     = 1ULL << 0;
constexpr std::uint64_t kFlagParity    = 1ULL << 2;
constexpr std::uint64_t kFlagAuxiliary = 1ULL << 4;
constexpr std::uint64_t kFlagZero      = 1ULL << 6;
constexpr std::uint64_t kFlagSign      = 1ULL << 7;
constexpr std::uint64_t kFlagOverflow  = 1ULL << 11;
constexpr std::uint64_t kArithmeticFlags =
    kFlagCarry | kFlagParity | kFlagAuxiliary | kFlagZero | kFlagSign | kFlagOverflow;
constexpr std::uint64_t kRflagsReservedOne = 1ULL << 1;
constexpr std::size_t kMaxInstructionLength = 15;

class InstructionReader {
public:
	explicit InstructionReader(const GuestMemory& memory, std::uint64_t address) noexcept :
	    memory_(memory), start_(address), cursor_(address) {}

	bool ReadByte(std::uint8_t& value) noexcept {
		if (length_ >= kMaxInstructionLength) {
			too_long_ = true;
			return false;
		}
		if (!memory_.Read(cursor_, &value, sizeof(value))) {
			memory_fault_ = true;
			fault_address_ = cursor_;
			return false;
		}
		++cursor_;
		++length_;
		return true;
	}

	[[nodiscard]] std::uint64_t Cursor() const noexcept {
		return cursor_;
	}
	[[nodiscard]] std::uint64_t Start() const noexcept {
		return start_;
	}
	[[nodiscard]] bool HasMemoryFault() const noexcept {
		return memory_fault_;
	}
	[[nodiscard]] bool IsTooLong() const noexcept {
		return too_long_;
	}
	[[nodiscard]] std::uint64_t FaultAddress() const noexcept {
		return fault_address_;
	}

private:
	const GuestMemory& memory_;
	std::uint64_t start_ = 0;
	std::uint64_t cursor_ = 0;
	std::size_t length_ = 0;
	bool memory_fault_ = false;
	bool too_long_ = false;
	std::uint64_t fault_address_ = 0;
};

template <typename UInt>
bool ReadLittleEndian(InstructionReader& reader, UInt& value) noexcept {
	static_assert(std::is_unsigned_v<UInt>);
	value = 0;
	for (std::size_t i = 0; i < sizeof(UInt); ++i) {
		std::uint8_t byte = 0;
		if (!reader.ReadByte(byte)) {
			return false;
		}
		value |= static_cast<UInt>(byte) << (i * 8);
	}
	return true;
}

InstructionStep ReaderFailure(const InstructionReader& reader) noexcept {
	if (reader.HasMemoryFault()) {
		return {InstructionStepStatus::MemoryFault, reader.Start(), reader.FaultAddress()};
	}
	return {InstructionStepStatus::UnsupportedInstruction, reader.Start(),
	        reader.IsTooLong() ? kMaxInstructionLength : 0};
}

InstructionStep Unsupported(std::uint64_t rip, std::uint64_t opcode) noexcept {
	return {InstructionStepStatus::UnsupportedInstruction, rip, opcode};
}

std::size_t RegisterIndex(std::uint8_t encoded, std::uint8_t rex_extension) noexcept {
	return static_cast<std::size_t>(encoded | rex_extension);
}

std::uint64_t ReadGpr(const GuestCpuState& state, std::size_t index, bool wide) noexcept {
	const std::uint64_t value = state.gpr[index];
	return wide ? value : static_cast<std::uint32_t>(value);
}

void WriteGpr(GuestCpuState& state, std::size_t index, bool wide, std::uint64_t value) noexcept {
	// x86-64 writes to a 32-bit general-purpose register zero-extend to 64 bits.
	state.gpr[index] = wide ? value : static_cast<std::uint32_t>(value);
}

enum class ArithmeticOperation : std::uint8_t {
	Add,
	Subtract,
	Xor,
};

void UpdateArithmeticFlags(GuestCpuState& state, ArithmeticOperation operation,
                           std::uint64_t lhs, std::uint64_t rhs,
                           std::uint64_t result, bool wide) noexcept {
	const std::uint64_t mask = wide ? UINT64_MAX : 0xffffffffULL;
	const std::uint64_t sign = wide ? (1ULL << 63) : (1ULL << 31);
	lhs &= mask;
	rhs &= mask;
	result &= mask;

	std::uint64_t updated = 0;
	bool carry = false;
	bool overflow = false;
	bool auxiliary = false;

	switch (operation) {
		case ArithmeticOperation::Add:
			carry = wide ? result < lhs : (lhs + rhs) > mask;
			overflow = ((~(lhs ^ rhs) & (lhs ^ result) & sign) != 0);
			auxiliary = ((lhs ^ rhs ^ result) & 0x10) != 0;
			break;
		case ArithmeticOperation::Subtract:
			carry = lhs < rhs;
			overflow = (((lhs ^ rhs) & (lhs ^ result) & sign) != 0);
			auxiliary = ((lhs ^ rhs ^ result) & 0x10) != 0;
			break;
		case ArithmeticOperation::Xor:
			// AF is architecturally undefined for XOR; clear it deterministically.
			break;
	}

	if (carry) {
		updated |= kFlagCarry;
	}
	if ((std::popcount(static_cast<std::uint8_t>(result)) & 1) == 0) {
		updated |= kFlagParity;
	}
	if (auxiliary) {
		updated |= kFlagAuxiliary;
	}
	if (result == 0) {
		updated |= kFlagZero;
	}
	if ((result & sign) != 0) {
		updated |= kFlagSign;
	}
	if (overflow) {
		updated |= kFlagOverflow;
	}

	state.rflags = (state.rflags & ~kArithmeticFlags) | updated | kRflagsReservedOne;
}

struct RmOperand {
	bool is_register = false;
	std::size_t register_index = 0;
	std::uint64_t base = 0;
	std::uint64_t index = 0;
	std::int64_t displacement = 0;
	bool rip_relative = false;
};

bool DecodeRmOperand(InstructionReader& reader, const GuestCpuState& state,
                     std::uint8_t modrm, std::uint8_t rex, RmOperand& operand) noexcept {
	const std::uint8_t mod = static_cast<std::uint8_t>((modrm >> 6) & 0x03);
	const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 0x07);
	const std::uint8_t rex_b = (rex & 0x01) != 0 ? 8 : 0;
	const std::uint8_t rex_x = (rex & 0x02) != 0 ? 8 : 0;

	if (mod == 0x03) {
		operand.is_register = true;
		operand.register_index = RegisterIndex(rm_low, rex_b);
		return true;
	}

	if (rm_low == 0x04) { // SIB addressing
		std::uint8_t sib = 0;
		if (!reader.ReadByte(sib)) {
			return false;
		}
		const std::uint8_t scale = static_cast<std::uint8_t>(sib >> 6);
		const std::uint8_t index_low = static_cast<std::uint8_t>((sib >> 3) & 0x07);
		const std::uint8_t base_low = static_cast<std::uint8_t>(sib & 0x07);

		if (index_low != 0x04 || rex_x != 0) {
			const auto index_reg = RegisterIndex(index_low, rex_x);
			operand.index = state.gpr[index_reg] << scale;
		}
		const bool has_base = !(mod == 0 && base_low == 0x05);
		if (has_base) {
			operand.base = state.gpr[RegisterIndex(base_low, rex_b)];
		}
		if (!has_base) {
			std::uint32_t encoded = 0;
			if (!ReadLittleEndian(reader, encoded)) {
				return false;
			}
			operand.displacement = std::bit_cast<std::int32_t>(encoded);
		}
	} else if (mod == 0 && rm_low == 0x05) { // RIP-relative disp32
		std::uint32_t encoded = 0;
		if (!ReadLittleEndian(reader, encoded)) {
			return false;
		}
		operand.rip_relative = true;
		operand.displacement = std::bit_cast<std::int32_t>(encoded);
	} else {
		operand.base = state.gpr[RegisterIndex(rm_low, rex_b)];
	}

	if (mod == 0x01) {
		std::uint8_t encoded = 0;
		if (!ReadLittleEndian(reader, encoded)) {
			return false;
		}
		operand.displacement += std::bit_cast<std::int8_t>(encoded);
	} else if (mod == 0x02) {
		std::uint32_t encoded = 0;
		if (!ReadLittleEndian(reader, encoded)) {
			return false;
		}
		operand.displacement += std::bit_cast<std::int32_t>(encoded);
	}
	return true;
}

std::uint64_t EffectiveAddress(const RmOperand& operand, std::uint64_t next_rip,
                               const GuestCpuState& state, std::uint8_t segment_override,
                               bool apply_segment) noexcept {
	const std::uint64_t base = operand.rip_relative ? next_rip : operand.base;
	std::uint64_t address = base + operand.index +
	                        static_cast<std::uint64_t>(operand.displacement);
	if (apply_segment) {
		if (segment_override == 0x64) {
			address += state.fs_base;
		} else if (segment_override == 0x65) {
			address += state.gs_base;
		}
	}
	return address;
}

bool ReadRmOperand(const RmOperand& operand, const GuestCpuState& state,
                   const GuestMemory& memory, bool wide, std::uint64_t next_rip,
                   std::uint8_t segment_override, std::uint64_t& value,
                   std::uint64_t& fault_address) noexcept {
	if (operand.is_register) {
		value = ReadGpr(state, operand.register_index, wide);
		return true;
	}
	const auto address = EffectiveAddress(operand, next_rip, state, segment_override, true);
	std::uint8_t bytes[sizeof(std::uint64_t)]{};
	const std::size_t size = wide ? sizeof(std::uint64_t) : sizeof(std::uint32_t);
	if (!memory.Read(address, bytes, size)) {
		fault_address = address;
		return false;
	}
	value = ReadLittleEndianU64(bytes);
	if (!wide) {
		value = static_cast<std::uint32_t>(value);
	}
	return true;
}

bool WriteRmOperand(const RmOperand& operand, GuestCpuState& state, GuestMemory& memory,
                    bool wide, std::uint64_t next_rip, std::uint8_t segment_override,
                    std::uint64_t value, std::uint64_t& fault_address) noexcept {
	if (operand.is_register) {
		WriteGpr(state, operand.register_index, wide, value);
		return true;
	}
	const auto address = EffectiveAddress(operand, next_rip, state, segment_override, true);
	std::uint8_t bytes[sizeof(std::uint64_t)]{};
	const std::size_t size = wide ? sizeof(std::uint64_t) : sizeof(std::uint32_t);
	for (std::size_t i = 0; i < size; ++i) {
		bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
	}
	if (!memory.Write(address, bytes, size)) {
		fault_address = address;
		return false;
	}
	return true;
}

void UpdateLogicalFlags(GuestCpuState& state, std::uint64_t result, bool wide) noexcept {
	const std::uint64_t mask = wide ? UINT64_MAX : 0xffffffffULL;
	const std::uint64_t sign = wide ? (1ULL << 63) : (1ULL << 31);
	result &= mask;
	std::uint64_t updated = 0;
	if ((std::popcount(static_cast<std::uint8_t>(result)) & 1) == 0) {
		updated |= kFlagParity;
	}
	if (result == 0) {
		updated |= kFlagZero;
	}
	if ((result & sign) != 0) {
		updated |= kFlagSign;
	}
	// CF, OF, and AF are cleared deterministically for AND/TEST/XOR.
	state.rflags = (state.rflags & ~kArithmeticFlags) | updated | kRflagsReservedOne;
}

bool EvaluateCondition(const GuestCpuState& state, std::uint8_t condition) noexcept {
	const bool cf = (state.rflags & kFlagCarry) != 0;
	const bool pf = (state.rflags & kFlagParity) != 0;
	const bool zf = (state.rflags & kFlagZero) != 0;
	const bool sf = (state.rflags & kFlagSign) != 0;
	const bool of = (state.rflags & kFlagOverflow) != 0;
	switch (condition & 0x0f) {
		case 0x0: return of;
		case 0x1: return !of;
		case 0x2: return cf;
		case 0x3: return !cf;
		case 0x4: return zf;
		case 0x5: return !zf;
		case 0x6: return cf || zf;
		case 0x7: return !cf && !zf;
		case 0x8: return sf;
		case 0x9: return !sf;
		case 0xa: return pf;
		case 0xb: return !pf;
		case 0xc: return sf != of;
		case 0xd: return sf == of;
		case 0xe: return zf || sf != of;
		case 0xf: return !zf && sf == of;
	}
	return false;
}

std::uint64_t ReadLittleEndianU64(const std::uint8_t* bytes) noexcept {
	std::uint64_t value = 0;
	for (std::size_t i = 0; i < sizeof(value); ++i) {
		value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
	}
	return value;
}

} // namespace

const char* X64Interpreter::BackendName() const noexcept {
	return "x86-64-interpreter-subset";
}

bool X64Interpreter::IsSupported() const noexcept {
	// Interpretation uses ordinary host C++ and requires no Rosetta/JIT support.
	return true;
}

InstructionStep X64Interpreter::ExecuteOne(GuestCpuState& state, GuestMemory& memory) const {
	const std::uint64_t instruction_rip = state.rip;
	InstructionReader reader(memory, instruction_rip);

	std::uint8_t rex = 0;
	std::uint8_t opcode = 0;
	for (;;) {
		if (!reader.ReadByte(opcode)) {
			return ReaderFailure(reader);
		}
		if (opcode < 0x40 || opcode > 0x4f) {
			break;
		}
		rex = opcode;
	}

	const bool wide = (rex & 0x08) != 0;
	const std::uint8_t rex_r = (rex & 0x04) != 0 ? 8 : 0;
	const std::uint8_t rex_b = (rex & 0x01) != 0 ? 8 : 0;

	if (opcode == 0x90) { // NOP
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode >= 0xb8 && opcode <= 0xbf) { // MOV r32/r64, imm32/imm64
		const auto destination = RegisterIndex(static_cast<std::uint8_t>(opcode - 0xb8), rex_b);
		if (wide) {
			std::uint64_t immediate = 0;
			if (!ReadLittleEndian(reader, immediate)) {
				return ReaderFailure(reader);
			}
			WriteGpr(state, destination, true, immediate);
		} else {
			std::uint32_t immediate = 0;
			if (!ReadLittleEndian(reader, immediate)) {
				return ReaderFailure(reader);
			}
			WriteGpr(state, destination, false, immediate);
		}
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0xeb || opcode == 0xe9) { // JMP rel8 / rel32
		std::int64_t displacement = 0;
		if (opcode == 0xeb) {
			std::uint8_t encoded = 0;
			if (!ReadLittleEndian(reader, encoded)) {
				return ReaderFailure(reader);
			}
			displacement = static_cast<std::int8_t>(encoded);
		} else {
			std::uint32_t encoded = 0;
			if (!ReadLittleEndian(reader, encoded)) {
				return ReaderFailure(reader);
			}
			displacement = static_cast<std::int32_t>(encoded);
		}
		state.rip = reader.Cursor() + static_cast<std::uint64_t>(displacement);
		return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
	}

	if (opcode == 0xc3) { // RET
		const auto stack_pointer = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
		std::uint8_t bytes[sizeof(std::uint64_t)]{};
		if (!memory.Read(stack_pointer, bytes, sizeof(bytes))) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, stack_pointer};
		}
		state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = stack_pointer + sizeof(bytes);
		state.rip = ReadLittleEndianU64(bytes);
		return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
	}

	// Register-direct forms only. Memory operands are deliberately rejected
	// until guest-address translation and page-fault behavior are implemented.
	if (opcode == 0x89 || opcode == 0x8b ||
	    opcode == 0x01 || opcode == 0x03 ||
	    opcode == 0x29 || opcode == 0x2b ||
	    opcode == 0x31 || opcode == 0x33) {
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t mod = static_cast<std::uint8_t>((modrm >> 6) & 0x03);
		if (mod != 0x03) {
			return Unsupported(instruction_rip, opcode);
		}

		const auto reg = RegisterIndex(static_cast<std::uint8_t>((modrm >> 3) & 0x07), rex_r);
		const auto rm = RegisterIndex(static_cast<std::uint8_t>(modrm & 0x07), rex_b);

		if (opcode == 0x89 || opcode == 0x8b) {
			const auto destination = opcode == 0x89 ? rm : reg;
			const auto source = opcode == 0x89 ? reg : rm;
			WriteGpr(state, destination, wide, ReadGpr(state, source, wide));
			state.rip = reader.Cursor();
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		const bool destination_is_rm = opcode == 0x01 || opcode == 0x29 || opcode == 0x31;
		const auto destination = destination_is_rm ? rm : reg;
		const auto source = destination_is_rm ? reg : rm;
		const std::uint64_t lhs = ReadGpr(state, destination, wide);
		const std::uint64_t rhs = ReadGpr(state, source, wide);
		ArithmeticOperation operation = ArithmeticOperation::Add;
		std::uint64_t result = 0;
		if (opcode == 0x01 || opcode == 0x03) {
			operation = ArithmeticOperation::Add;
			result = lhs + rhs;
		} else if (opcode == 0x29 || opcode == 0x2b) {
			operation = ArithmeticOperation::Subtract;
			result = lhs - rhs;
		} else {
			operation = ArithmeticOperation::Xor;
			result = lhs ^ rhs;
		}
		if (!wide) {
			result = static_cast<std::uint32_t>(result);
		}
		WriteGpr(state, destination, wide, result);
		UpdateArithmeticFlags(state, operation, lhs, rhs, result, wide);
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	return Unsupported(instruction_rip, opcode);
}

BlockExit X64Interpreter::RunBlock(GuestCpuState& state, GuestMemory& memory,
                                   std::uint64_t instruction_budget) {
	for (std::uint64_t executed = 0; executed < instruction_budget; ++executed) {
		const InstructionStep step = ExecuteOne(state, memory);
		switch (step.status) {
			case InstructionStepStatus::Continue:
				break;
			case InstructionStepStatus::ControlFlow:
				return {BlockExitReason::BlockCompleted, state.rip, step.detail};
			case InstructionStepStatus::MemoryFault:
				return {BlockExitReason::MemoryFault, step.guest_rip, step.detail};
			case InstructionStepStatus::UnsupportedInstruction:
				return {BlockExitReason::UnsupportedInstruction, step.guest_rip, step.detail};
		}
	}
	return {BlockExitReason::InstructionBudget, state.rip, instruction_budget};
}

void X64Interpreter::InvalidateCodeRange(std::uint64_t guest_address, std::uint64_t size) {
	// The interpreter has no translated-code cache. Keep this method as a no-op
	// so it conforms to the common backend interface.
	(void)guest_address;
	(void)size;
}

} // namespace Kyty::NativeArm64
