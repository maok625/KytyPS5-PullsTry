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

std::uint64_t ReadLittleEndianU64(const std::uint8_t* bytes) noexcept;

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

std::size_t ByteRegisterIndex(std::size_t decoded_index, std::uint8_t encoded_low,
                              bool rex_present, bool* high_byte) noexcept {
	*high_byte = !rex_present && encoded_low >= 4 && encoded_low <= 7;
	return *high_byte ? static_cast<std::size_t>(encoded_low - 4) : decoded_index;
}

std::uint8_t ReadByteRegister(const GuestCpuState& state, std::size_t decoded_index,
                              std::uint8_t encoded_low, bool rex_present) noexcept {
	bool high_byte = false;
	const auto index = ByteRegisterIndex(decoded_index, encoded_low, rex_present, &high_byte);
	return static_cast<std::uint8_t>(state.gpr[index] >> (high_byte ? 8 : 0));
}

void WriteByteRegister(GuestCpuState& state, std::size_t decoded_index,
                       std::uint8_t encoded_low, bool rex_present, std::uint8_t value) noexcept {
	bool high_byte = false;
	const auto index = ByteRegisterIndex(decoded_index, encoded_low, rex_present, &high_byte);
	const unsigned shift = high_byte ? 8 : 0;
	const std::uint64_t mask = static_cast<std::uint64_t>(0xff) << shift;
	state.gpr[index] = (state.gpr[index] & ~mask) | (static_cast<std::uint64_t>(value) << shift);
}

bool ReadRmByte(const RmOperand& operand, const GuestCpuState& state, const GuestMemory& memory,
                std::uint64_t next_rip, std::uint8_t segment_override, bool rex_present,
                std::uint8_t encoded_rm, std::uint8_t& value,
                std::uint64_t& fault_address) noexcept {
	if (operand.is_register) {
		value = ReadByteRegister(state, operand.register_index, encoded_rm, rex_present);
		return true;
	}
	const auto address = EffectiveAddress(operand, next_rip, state, segment_override, true);
	if (!memory.Read(address, &value, sizeof(value))) {
		fault_address = address;
		return false;
	}
	return true;
}

bool WriteRmByte(const RmOperand& operand, GuestCpuState& state, GuestMemory& memory,
                 std::uint64_t next_rip, std::uint8_t segment_override, bool rex_present,
                 std::uint8_t encoded_rm, std::uint8_t value,
                 std::uint64_t& fault_address) noexcept {
	if (operand.is_register) {
		WriteByteRegister(state, operand.register_index, encoded_rm, rex_present, value);
		return true;
	}
	const auto address = EffectiveAddress(operand, next_rip, state, segment_override, true);
	if (!memory.Write(address, &value, sizeof(value))) {
		fault_address = address;
		return false;
	}
	return true;
}

std::uint64_t MaskForBits(unsigned bits) noexcept {
	return bits == 64 ? UINT64_MAX : ((1ULL << bits) - 1ULL);
}

std::uint64_t ReadGprSized(const GuestCpuState& state, std::size_t index, unsigned bits) noexcept {
	return state.gpr[index] & MaskForBits(bits);
}

void WriteGprSized(GuestCpuState& state, std::size_t index, unsigned bits,
                   std::uint64_t value) noexcept {
	const std::uint64_t mask = MaskForBits(bits);
	if (bits == 64 || bits == 32) {
		// x86-64 32-bit writes zero-extend; 64-bit writes replace the full register.
		state.gpr[index] = value & mask;
	} else {
		state.gpr[index] = (state.gpr[index] & ~mask) | (value & mask);
	}
}

bool ReadRmSized(const RmOperand& operand, const GuestCpuState& state, const GuestMemory& memory,
                 unsigned bits, std::uint64_t next_rip, std::uint8_t segment_override,
                 std::uint64_t& value, std::uint64_t& fault_address) noexcept {
	if (operand.is_register) {
		value = ReadGprSized(state, operand.register_index, bits);
		return true;
	}
	const auto address = EffectiveAddress(operand, next_rip, state, segment_override, true);
	std::uint8_t bytes[sizeof(std::uint64_t)]{};
	const std::size_t size = bits / 8;
	if (!memory.Read(address, bytes, size)) {
		fault_address = address;
		return false;
	}
	value = ReadLittleEndianU64(bytes) & MaskForBits(bits);
	return true;
}

bool WriteRmSized(const RmOperand& operand, GuestCpuState& state, GuestMemory& memory,
                  unsigned bits, std::uint64_t next_rip, std::uint8_t segment_override,
                  std::uint64_t value, std::uint64_t& fault_address) noexcept {
	if (operand.is_register) {
		WriteGprSized(state, operand.register_index, bits, value);
		return true;
	}
	const auto address = EffectiveAddress(operand, next_rip, state, segment_override, true);
	std::uint8_t bytes[sizeof(std::uint64_t)]{};
	const std::size_t size = bits / 8;
	for (std::size_t i = 0; i < size; ++i) {
		bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
	}
	if (!memory.Write(address, bytes, size)) {
		fault_address = address;
		return false;
	}
	return true;
}

void UpdateArithmeticFlagsSized(GuestCpuState& state, ArithmeticOperation operation,
                                std::uint64_t lhs, std::uint64_t rhs,
                                std::uint64_t result, unsigned bits) noexcept {
	const std::uint64_t mask = bits == 64 ? UINT64_MAX : ((1ULL << bits) - 1ULL);
	const std::uint64_t sign = 1ULL << (bits - 1);
	lhs &= mask;
	rhs &= mask;
	result &= mask;

	std::uint64_t updated = 0;
	bool carry = false;
	bool overflow = false;
	bool auxiliary = false;
	switch (operation) {
		case ArithmeticOperation::Add:
			carry = bits == 64 ? result < lhs : (lhs + rhs) > mask;
			overflow = ((~(lhs ^ rhs) & (lhs ^ result) & sign) != 0);
			auxiliary = ((lhs ^ rhs ^ result) & 0x10) != 0;
			break;
		case ArithmeticOperation::Subtract:
			carry = lhs < rhs;
			overflow = (((lhs ^ rhs) & (lhs ^ result) & sign) != 0);
			auxiliary = ((lhs ^ rhs ^ result) & 0x10) != 0;
			break;
		case ArithmeticOperation::Xor:
			break;
	}
	if (carry) updated |= kFlagCarry;
	if ((std::popcount(static_cast<std::uint8_t>(result)) & 1) == 0) updated |= kFlagParity;
	if (auxiliary) updated |= kFlagAuxiliary;
	if (result == 0) updated |= kFlagZero;
	if ((result & sign) != 0) updated |= kFlagSign;
	if (overflow) updated |= kFlagOverflow;
	state.rflags = (state.rflags & ~kArithmeticFlags) | updated | kRflagsReservedOne;
}

void UpdateLogicalFlagsSized(GuestCpuState& state, std::uint64_t result, unsigned bits) noexcept {
	const std::uint64_t mask = bits == 64 ? UINT64_MAX : ((1ULL << bits) - 1ULL);
	const std::uint64_t sign = 1ULL << (bits - 1);
	result &= mask;
	std::uint64_t updated = 0;
	if ((std::popcount(static_cast<std::uint8_t>(result)) & 1) == 0) updated |= kFlagParity;
	if (result == 0) updated |= kFlagZero;
	if ((result & sign) != 0) updated |= kFlagSign;
	state.rflags = (state.rflags & ~kArithmeticFlags) | updated | kRflagsReservedOne;
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
	bool rex_present = false;
	bool operand_size_override = false;
	std::uint8_t segment_override = 0;
	std::uint8_t opcode = 0;
	for (;;) {
		if (!reader.ReadByte(opcode)) {
			return ReaderFailure(reader);
		}
		if (opcode >= 0x40 && opcode <= 0x4f) {
			rex = opcode;
			rex_present = true;
			continue;
		}
		if (opcode == 0x66) { // Operand-size override (supported 16-bit subset below)
			operand_size_override = true;
			rex = 0;
			rex_present = false;
			continue;
		}
		if (opcode == 0x64 || opcode == 0x65) { // FS / GS segment override
			segment_override = opcode;
			rex = 0;
			rex_present = false; // A legacy prefix after REX makes that REX ineffective.
			continue;
		}
		if (opcode == 0x67 || opcode == 0xf0 || opcode == 0xf2 ||
		    opcode == 0xf3 || opcode == 0x26 || opcode == 0x2e ||
		    opcode == 0x36 || opcode == 0x3e) {
			return Unsupported(instruction_rip, opcode);
		}
		break;
	}

	const bool wide = (rex & 0x08) != 0;
	const std::uint8_t rex_r = (rex & 0x04) != 0 ? 8 : 0;
	const std::uint8_t rex_b = (rex & 0x01) != 0 ? 8 : 0;

	// MOV r8, imm8. Without any REX prefix encodings 4..7 are AH/CH/DH/BH;
	// with a REX prefix they refer to SPL/BPL/SIL/DIL.
	if (opcode >= 0xb0 && opcode <= 0xb7) {
		std::uint8_t immediate = 0;
		if (!ReadLittleEndian(reader, immediate)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t encoded_reg = static_cast<std::uint8_t>(opcode - 0xb0);
		const auto destination = RegisterIndex(encoded_reg, rex_b);
		WriteByteRegister(state, destination, encoded_reg, rex_present, immediate);
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	// Accumulator byte-immediate forms: ADD/OR/AND/SUB/XOR/CMP AL,imm8 and TEST AL,imm8.
	if (opcode == 0x04 || opcode == 0x0c || opcode == 0x24 ||
	    opcode == 0x2c || opcode == 0x34 || opcode == 0x3c || opcode == 0xa8) {
		std::uint8_t immediate = 0;
		if (!ReadLittleEndian(reader, immediate)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t lhs = ReadByteRegister(state, 0, 0, rex_present);
		std::uint8_t result = 0;
		bool write_result = opcode != 0x3c && opcode != 0xa8;
		bool logical = false;
		ArithmeticOperation operation = ArithmeticOperation::Add;
		switch (opcode) {
			case 0x04: result = static_cast<std::uint8_t>(lhs + immediate); break;
			case 0x0c: result = static_cast<std::uint8_t>(lhs | immediate); logical = true; break;
			case 0x24: result = static_cast<std::uint8_t>(lhs & immediate); logical = true; break;
			case 0x2c: result = static_cast<std::uint8_t>(lhs - immediate); operation = ArithmeticOperation::Subtract; break;
			case 0x34: result = static_cast<std::uint8_t>(lhs ^ immediate); operation = ArithmeticOperation::Xor; break;
			case 0x3c: result = static_cast<std::uint8_t>(lhs - immediate); operation = ArithmeticOperation::Subtract; break;
			case 0xa8: result = static_cast<std::uint8_t>(lhs & immediate); logical = true; break;
			default: return Unsupported(instruction_rip, opcode);
		}
		if (write_result) {
			WriteByteRegister(state, 0, 0, rex_present, result);
		}
		if (logical) {
			UpdateLogicalFlagsSized(state, result, 8);
		} else {
			UpdateArithmeticFlagsSized(state, operation, lhs, immediate, result, 8);
		}
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0xc6) { // MOV r/m8, imm8 (/0)
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		if (((modrm >> 3) & 0x07) != 0) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand destination{};
		if (!DecodeRmOperand(reader, state, modrm, rex, destination)) {
			return ReaderFailure(reader);
		}
		std::uint8_t immediate = 0;
		if (!ReadLittleEndian(reader, immediate)) {
			return ReaderFailure(reader);
		}
		std::uint64_t fault_address = 0;
		if (!WriteRmByte(destination, state, memory, reader.Cursor(), segment_override,
		                 rex_present, static_cast<std::uint8_t>(modrm & 0x07),
		                 immediate, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0xfe) { // INC/DEC r/m8
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t group = static_cast<std::uint8_t>((modrm >> 3) & 0x07);
		if (group != 0 && group != 1) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand operand{};
		if (!DecodeRmOperand(reader, state, modrm, rex, operand)) {
			return ReaderFailure(reader);
		}
		const auto next_rip = reader.Cursor();
		std::uint8_t value = 0;
		std::uint64_t fault_address = 0;
		const auto encoded_rm = static_cast<std::uint8_t>(modrm & 0x07);
		if (!ReadRmByte(operand, state, memory, next_rip, segment_override, rex_present,
		                encoded_rm, value, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		const bool decrement = group == 1;
		const auto result = static_cast<std::uint8_t>(decrement ? value - 1 : value + 1);
		if (!WriteRmByte(operand, state, memory, next_rip, segment_override, rex_present,
		                 encoded_rm, result, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		const std::uint64_t old_carry = state.rflags & kFlagCarry;
		UpdateArithmeticFlagsSized(state, decrement ? ArithmeticOperation::Subtract : ArithmeticOperation::Add,
		                           value, 1, result, 8);
		state.rflags = (state.rflags & ~kFlagCarry) | old_carry | kRflagsReservedOne;
		state.rip = next_rip;
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0xf6) { // TEST/NOT/NEG r/m8
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t group = static_cast<std::uint8_t>((modrm >> 3) & 0x07);
		if (!(group == 0 || group == 2 || group == 3)) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand operand{};
		if (!DecodeRmOperand(reader, state, modrm, rex, operand)) {
			return ReaderFailure(reader);
		}
		const auto encoded_rm = static_cast<std::uint8_t>(modrm & 0x07);
		if (group == 0) {
			std::uint8_t immediate = 0;
			if (!ReadLittleEndian(reader, immediate)) {
				return ReaderFailure(reader);
			}
			std::uint8_t value = 0;
			std::uint64_t fault_address = 0;
			if (!ReadRmByte(operand, state, memory, reader.Cursor(), segment_override,
			                rex_present, encoded_rm, value, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			UpdateLogicalFlagsSized(state, static_cast<std::uint8_t>(value & immediate), 8);
			state.rip = reader.Cursor();
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}
		const auto next_rip = reader.Cursor();
		std::uint8_t value = 0;
		std::uint64_t fault_address = 0;
		if (!ReadRmByte(operand, state, memory, next_rip, segment_override, rex_present,
		                encoded_rm, value, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		const auto result = static_cast<std::uint8_t>(group == 2 ? ~value : 0U - value);
		if (!WriteRmByte(operand, state, memory, next_rip, segment_override, rex_present,
		                 encoded_rm, result, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		if (group == 3) {
			UpdateArithmeticFlagsSized(state, ArithmeticOperation::Subtract, 0, value, result, 8);
		}
		state.rip = next_rip;
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	// Byte forms of MOV and the common integer ALU instructions. The odd opcode
	// in each pair writes the ModRM.reg operand; the even form writes r/m.
	if (opcode == 0x88 || opcode == 0x8a ||
	    opcode == 0x00 || opcode == 0x02 || opcode == 0x08 || opcode == 0x0a ||
	    opcode == 0x20 || opcode == 0x22 || opcode == 0x28 || opcode == 0x2a ||
	    opcode == 0x30 || opcode == 0x32 || opcode == 0x38 || opcode == 0x3a ||
	    opcode == 0x84) {
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		RmOperand rm{};
		if (!DecodeRmOperand(reader, state, modrm, rex, rm)) {
			return ReaderFailure(reader);
		}
		const auto next_rip = reader.Cursor();
		const auto encoded_rm = static_cast<std::uint8_t>(modrm & 0x07);
		const auto encoded_reg = static_cast<std::uint8_t>((modrm >> 3) & 0x07);
		const auto reg = RegisterIndex(encoded_reg, rex_r);
		const bool is_mov_rm_reg = opcode == 0x88;
		const bool is_mov_reg_rm = opcode == 0x8a;

		if (is_mov_rm_reg) {
			const auto source = ReadByteRegister(state, reg, encoded_reg, rex_present);
			std::uint64_t fault_address = 0;
			if (!WriteRmByte(rm, state, memory, next_rip, segment_override, rex_present,
			                 encoded_rm, source, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}
		if (is_mov_reg_rm) {
			std::uint8_t source = 0;
			std::uint64_t fault_address = 0;
			if (!ReadRmByte(rm, state, memory, next_rip, segment_override, rex_present,
			                encoded_rm, source, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			WriteByteRegister(state, reg, encoded_reg, rex_present, source);
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		std::uint8_t rm_value = 0;
		std::uint64_t fault_address = 0;
		if (!ReadRmByte(rm, state, memory, next_rip, segment_override, rex_present,
		                encoded_rm, rm_value, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		const std::uint8_t reg_value = ReadByteRegister(state, reg, encoded_reg, rex_present);
		const bool test = opcode == 0x84;
		const bool rm_is_destination = test || (opcode & 0x02) == 0;
		const std::uint8_t lhs = rm_is_destination ? rm_value : reg_value;
		const std::uint8_t rhs = rm_is_destination ? reg_value : rm_value;
		std::uint8_t result = 0;
		bool logical = false;
		bool compare = false;
		ArithmeticOperation operation = ArithmeticOperation::Add;
		switch (opcode) {
			case 0x00: case 0x02: result = static_cast<std::uint8_t>(lhs + rhs); break;
			case 0x08: case 0x0a: result = static_cast<std::uint8_t>(lhs | rhs); logical = true; break;
			case 0x20: case 0x22: result = static_cast<std::uint8_t>(lhs & rhs); logical = true; break;
			case 0x28: case 0x2a: result = static_cast<std::uint8_t>(lhs - rhs); operation = ArithmeticOperation::Subtract; break;
			case 0x30: case 0x32: result = static_cast<std::uint8_t>(lhs ^ rhs); operation = ArithmeticOperation::Xor; break;
			case 0x38: case 0x3a: result = static_cast<std::uint8_t>(lhs - rhs); operation = ArithmeticOperation::Subtract; compare = true; break;
			case 0x84: result = static_cast<std::uint8_t>(lhs & rhs); logical = true; break;
			default: return Unsupported(instruction_rip, opcode);
		}
		if (!compare && !test) {
			if (rm_is_destination) {
				if (!WriteRmByte(rm, state, memory, next_rip, segment_override, rex_present,
				                 encoded_rm, result, fault_address)) {
					return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
				}
			} else {
				WriteByteRegister(state, reg, encoded_reg, rex_present, result);
			}
		}
		if (logical) {
			UpdateLogicalFlagsSized(state, result, 8);
		} else {
			UpdateArithmeticFlagsSized(state, operation, lhs, rhs, result, 8);
		}
		state.rip = next_rip;
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	// Operand-size override selects 16-bit integer operations in long mode,
	// unless REX.W is present (which takes precedence over 66h).
	if (operand_size_override && !wide && opcode >= 0xb8 && opcode <= 0xbf) {
		std::uint16_t immediate = 0;
		if (!ReadLittleEndian(reader, immediate)) {
			return ReaderFailure(reader);
		}
		const auto destination = RegisterIndex(static_cast<std::uint8_t>(opcode - 0xb8), rex_b);
		WriteGprSized(state, destination, 16, immediate);
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (operand_size_override && !wide && opcode == 0xc7) { // MOV r/m16, imm16 (/0)
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		if (((modrm >> 3) & 0x07) != 0) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand destination{};
		if (!DecodeRmOperand(reader, state, modrm, rex, destination)) {
			return ReaderFailure(reader);
		}
		std::uint16_t immediate = 0;
		if (!ReadLittleEndian(reader, immediate)) {
			return ReaderFailure(reader);
		}
		std::uint64_t fault_address = 0;
		if (!WriteRmSized(destination, state, memory, 16, reader.Cursor(), segment_override,
		                  immediate, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (operand_size_override && !wide &&
	    (opcode == 0x89 || opcode == 0x8b || opcode == 0x8d ||
	     opcode == 0x01 || opcode == 0x03 || opcode == 0x09 || opcode == 0x0b ||
	     opcode == 0x21 || opcode == 0x23 || opcode == 0x29 || opcode == 0x2b ||
	     opcode == 0x31 || opcode == 0x33 || opcode == 0x39 || opcode == 0x3b ||
	     opcode == 0x85)) {
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		RmOperand rm{};
		if (!DecodeRmOperand(reader, state, modrm, rex, rm)) {
			return ReaderFailure(reader);
		}
		const auto reg_low = static_cast<std::uint8_t>((modrm >> 3) & 0x07);
		const auto reg = RegisterIndex(reg_low, rex_r);
		const auto next_rip = reader.Cursor();

		if (opcode == 0x89) { // MOV r/m16, r16
			std::uint64_t fault_address = 0;
			if (!WriteRmSized(rm, state, memory, 16, next_rip, segment_override,
			                  ReadGprSized(state, reg, 16), fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}
		if (opcode == 0x8b) { // MOV r16, r/m16
			std::uint64_t source = 0;
			std::uint64_t fault_address = 0;
			if (!ReadRmSized(rm, state, memory, 16, next_rip, segment_override,
			                 source, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			WriteGprSized(state, reg, 16, source);
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}
		if (opcode == 0x8d) { // LEA r16, m
			if (rm.is_register) {
				return Unsupported(instruction_rip, opcode);
			}
			WriteGprSized(state, reg, 16,
			              EffectiveAddress(rm, next_rip, state, 0, false));
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		std::uint64_t rm_value = 0;
		std::uint64_t fault_address = 0;
		if (!ReadRmSized(rm, state, memory, 16, next_rip, segment_override,
		                 rm_value, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		const std::uint64_t reg_value = ReadGprSized(state, reg, 16);
		const bool test = opcode == 0x85;
		const bool rm_is_destination = test || (opcode & 0x02) == 0;
		const std::uint64_t lhs = rm_is_destination ? rm_value : reg_value;
		const std::uint64_t rhs = rm_is_destination ? reg_value : rm_value;
		std::uint64_t result = 0;
		bool logical = false;
		bool compare = false;
		ArithmeticOperation operation = ArithmeticOperation::Add;
		switch (opcode) {
			case 0x01: case 0x03: result = lhs + rhs; break;
			case 0x09: case 0x0b: result = lhs | rhs; logical = true; break;
			case 0x21: case 0x23: result = lhs & rhs; logical = true; break;
			case 0x29: case 0x2b: result = lhs - rhs; operation = ArithmeticOperation::Subtract; break;
			case 0x31: case 0x33: result = lhs ^ rhs; operation = ArithmeticOperation::Xor; break;
			case 0x39: case 0x3b: result = lhs - rhs; operation = ArithmeticOperation::Subtract; compare = true; break;
			case 0x85: result = lhs & rhs; logical = true; break;
			default: return Unsupported(instruction_rip, opcode);
		}
		result &= 0xffff;
		if (!compare && !test) {
			if (rm_is_destination) {
				if (!WriteRmSized(rm, state, memory, 16, next_rip, segment_override,
				                  result, fault_address)) {
					return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
				}
			} else {
				WriteGprSized(state, reg, 16, result);
			}
		}
		if (logical) {
			UpdateLogicalFlagsSized(state, result, 16);
		} else {
			UpdateArithmeticFlagsSized(state, operation, lhs, rhs, result, 16);
		}
		state.rip = next_rip;
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (operand_size_override && !wide && (opcode == 0x81 || opcode == 0x83)) {
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t group = static_cast<std::uint8_t>((modrm >> 3) & 0x07);
		if (!(group == 0 || group == 1 || group == 4 || group == 5 ||
		      group == 6 || group == 7)) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand destination{};
		if (!DecodeRmOperand(reader, state, modrm, rex, destination)) {
			return ReaderFailure(reader);
		}
		std::uint16_t immediate = 0;
		if (opcode == 0x83) {
			std::uint8_t encoded = 0;
			if (!ReadLittleEndian(reader, encoded)) {
				return ReaderFailure(reader);
			}
			immediate = static_cast<std::uint16_t>(std::bit_cast<std::int8_t>(encoded));
		} else {
			if (!ReadLittleEndian(reader, immediate)) {
				return ReaderFailure(reader);
			}
		}
		const auto next_rip = reader.Cursor();
		std::uint64_t lhs = 0;
		std::uint64_t fault_address = 0;
		if (!ReadRmSized(destination, state, memory, 16, next_rip, segment_override,
		                 lhs, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		const std::uint64_t rhs = immediate;
		std::uint64_t result = 0;
		bool is_compare = false;
		bool is_logical = false;
		ArithmeticOperation operation = ArithmeticOperation::Add;
		switch (group) {
			case 0: result = lhs + rhs; break;
			case 1: result = lhs | rhs; is_logical = true; break;
			case 4: result = lhs & rhs; is_logical = true; break;
			case 5: result = lhs - rhs; operation = ArithmeticOperation::Subtract; break;
			case 6: result = lhs ^ rhs; operation = ArithmeticOperation::Xor; is_logical = true; break;
			case 7: result = lhs - rhs; operation = ArithmeticOperation::Subtract; is_compare = true; break;
			default: return Unsupported(instruction_rip, opcode);
		}
		result &= 0xffff;
		if (!is_compare && !WriteRmSized(destination, state, memory, 16, next_rip,
		                                segment_override, result, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		if (is_logical) {
			UpdateLogicalFlagsSized(state, result, 16);
		} else {
			UpdateArithmeticFlagsSized(state, operation, lhs, rhs, result, 16);
		}
		state.rip = next_rip;
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0x90) { // NOP
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	// Do not silently reinterpret an unimplemented 16-bit instruction as its
	// 32-bit counterpart merely because an operand-size prefix was decoded.
	if (operand_size_override && !wide) {
		return Unsupported(instruction_rip, opcode);
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

	if (opcode >= 0x70 && opcode <= 0x7f) { // Jcc rel8
		std::uint8_t encoded = 0;
		if (!ReadLittleEndian(reader, encoded)) {
			return ReaderFailure(reader);
		}
		const std::uint64_t next_rip = reader.Cursor();
		const auto displacement = std::bit_cast<std::int8_t>(encoded);
		state.rip = EvaluateCondition(state, static_cast<std::uint8_t>(opcode & 0x0f))
		                ? next_rip + static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement))
		                : next_rip;
		return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
	}

	if (opcode == 0x0f) {
		std::uint8_t second_opcode = 0;
		if (!reader.ReadByte(second_opcode)) {
			return ReaderFailure(reader);
		}
		if (second_opcode == 0xb6 || second_opcode == 0xb7 ||
		    second_opcode == 0xbe || second_opcode == 0xbf) {
			// MOVZX/MOVSX r16/r32/r64, r/m8/r/m16.
			const unsigned source_bits = (second_opcode == 0xb6 || second_opcode == 0xbe) ? 8 : 16;
			const unsigned destination_bits = wide ? 64 : (operand_size_override ? 16 : 32);
			std::uint8_t modrm = 0;
			if (!reader.ReadByte(modrm)) {
				return ReaderFailure(reader);
			}
			RmOperand source_operand{};
			if (!DecodeRmOperand(reader, state, modrm, rex, source_operand)) {
				return ReaderFailure(reader);
			}
			const auto next_rip = reader.Cursor();
			const auto destination = RegisterIndex(static_cast<std::uint8_t>((modrm >> 3) & 0x07), rex_r);
			std::uint64_t source = 0;
			std::uint64_t fault_address = 0;
			if (source_bits == 8) {
				std::uint8_t source_byte = 0;
				if (!ReadRmByte(source_operand, state, memory, next_rip, segment_override,
				                rex_present, static_cast<std::uint8_t>(modrm & 0x07),
				                source_byte, fault_address)) {
					return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
				}
				source = source_byte;
			} else if (!ReadRmSized(source_operand, state, memory, 16, next_rip,
			                        segment_override, source, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}

			const bool sign_extend = second_opcode == 0xbe || second_opcode == 0xbf;
			std::uint64_t result = source;
			if (sign_extend) {
				if (source_bits == 8) {
					result = static_cast<std::uint64_t>(
					    static_cast<std::int64_t>(std::bit_cast<std::int8_t>(
				        static_cast<std::uint8_t>(source))));
				} else {
					result = static_cast<std::uint64_t>(
					    static_cast<std::int64_t>(std::bit_cast<std::int16_t>(
				        static_cast<std::uint16_t>(source))));
				}
			}
			WriteGprSized(state, destination, destination_bits, result);
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip,
			        (static_cast<std::uint64_t>(opcode) << 8) | second_opcode};
		}

		if (second_opcode == 0x05) { // SYSCALL
			const std::uint64_t next_rip = reader.Cursor();
			state.gpr[static_cast<std::size_t>(GuestGpr::Rcx)] = next_rip;
			state.gpr[static_cast<std::size_t>(GuestGpr::R11)] = state.rflags;
			state.rip = next_rip;
			return {InstructionStepStatus::SystemCall, instruction_rip,
			        state.gpr[static_cast<std::size_t>(GuestGpr::Rax)]};
		}
		if (second_opcode == 0x0b) { // UD2 -> #UD
			return {InstructionStepStatus::GuestException, instruction_rip, 6};
		}
		if (second_opcode < 0x80 || second_opcode > 0x8f) {
			return Unsupported(instruction_rip,
			                   (static_cast<std::uint64_t>(opcode) << 8) | second_opcode);
		}
		std::uint32_t encoded = 0;
		if (!ReadLittleEndian(reader, encoded)) {
			return ReaderFailure(reader);
		}
		const std::uint64_t next_rip = reader.Cursor();
		const auto displacement = std::bit_cast<std::int32_t>(encoded);
		state.rip = EvaluateCondition(state, static_cast<std::uint8_t>(second_opcode & 0x0f))
		                ? next_rip + static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement))
		                : next_rip;
		return {InstructionStepStatus::ControlFlow, instruction_rip,
		        (static_cast<std::uint64_t>(opcode) << 8) | second_opcode};
	}

	if (opcode == 0xe8) { // CALL rel32
		std::uint32_t encoded = 0;
		if (!ReadLittleEndian(reader, encoded)) {
			return ReaderFailure(reader);
		}
		const std::uint64_t return_address = reader.Cursor();
		const auto displacement = std::bit_cast<std::int32_t>(encoded);
		const std::uint64_t target =
		    return_address + static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement));
		if (memory.TryDispatchHostCall(target, state, false)) {
			// A native import is invoked synchronously and returns directly to the
			// instruction after CALL; the guest RSP is unchanged by the call/ret pair.
			state.rip = return_address;
			return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
		}
		const auto old_stack = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
		const auto new_stack = old_stack - sizeof(return_address);
		std::uint8_t bytes[sizeof(return_address)]{};
		for (std::size_t i = 0; i < sizeof(return_address); ++i) {
			bytes[i] = static_cast<std::uint8_t>(return_address >> (i * 8));
		}
		if (!memory.Write(new_stack, bytes, sizeof(bytes))) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, new_stack};
		}
		state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = new_stack;
		state.rip = target;
		return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
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

	if (opcode == 0x68 || opcode == 0x6a) { // PUSH sign-extended imm32/imm8
		std::int64_t signed_immediate = 0;
		if (opcode == 0x6a) {
			std::uint8_t encoded = 0;
			if (!ReadLittleEndian(reader, encoded)) {
				return ReaderFailure(reader);
			}
			signed_immediate = std::bit_cast<std::int8_t>(encoded);
		} else {
			std::uint32_t encoded = 0;
			if (!ReadLittleEndian(reader, encoded)) {
				return ReaderFailure(reader);
			}
			signed_immediate = std::bit_cast<std::int32_t>(encoded);
		}
		const std::uint64_t value = static_cast<std::uint64_t>(signed_immediate);
		const auto old_stack = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
		const auto new_stack = old_stack - sizeof(value);
		std::uint8_t bytes[sizeof(value)]{};
		for (std::size_t i = 0; i < sizeof(value); ++i) {
			bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
		}
		if (!memory.Write(new_stack, bytes, sizeof(bytes))) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, new_stack};
		}
		state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = new_stack;
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode >= 0x50 && opcode <= 0x57) { // PUSH r64
		const auto source = RegisterIndex(static_cast<std::uint8_t>(opcode - 0x50), rex_b);
		const auto old_stack = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
		const auto new_stack = old_stack - sizeof(std::uint64_t);
		std::uint8_t bytes[sizeof(std::uint64_t)]{};
		const std::uint64_t value = state.gpr[source];
		for (std::size_t i = 0; i < sizeof(value); ++i) {
			bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
		}
		if (!memory.Write(new_stack, bytes, sizeof(bytes))) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, new_stack};
		}
		state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = new_stack;
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode >= 0x58 && opcode <= 0x5f) { // POP r64
		const auto destination = RegisterIndex(static_cast<std::uint8_t>(opcode - 0x58), rex_b);
		const auto old_stack = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
		std::uint8_t bytes[sizeof(std::uint64_t)]{};
		if (!memory.Read(old_stack, bytes, sizeof(bytes))) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, old_stack};
		}
		const std::uint64_t value = ReadLittleEndianU64(bytes);
		state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = old_stack + sizeof(bytes);
		WriteGpr(state, destination, true, value);
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0xc9) { // LEAVE: RSP = RBP; POP RBP
		const auto frame_pointer = state.gpr[static_cast<std::size_t>(GuestGpr::Rbp)];
		std::uint8_t bytes[sizeof(std::uint64_t)]{};
		if (!memory.Read(frame_pointer, bytes, sizeof(bytes))) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, frame_pointer};
		}
		const std::uint64_t restored_frame = ReadLittleEndianU64(bytes);
		state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = frame_pointer + sizeof(bytes);
		state.gpr[static_cast<std::size_t>(GuestGpr::Rbp)] = restored_frame;
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0xc2 || opcode == 0xc3) { // RET / RET imm16
		std::uint16_t stack_adjust = 0;
		if (opcode == 0xc2 && !ReadLittleEndian(reader, stack_adjust)) {
			return ReaderFailure(reader);
		}
		const auto stack_pointer = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
		std::uint8_t bytes[sizeof(std::uint64_t)]{};
		if (!memory.Read(stack_pointer, bytes, sizeof(bytes))) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, stack_pointer};
		}
		state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] =
		    stack_pointer + sizeof(bytes) + stack_adjust;
		state.rip = ReadLittleEndianU64(bytes);
		return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
	}

	// Register-direct forms only. Memory operands are deliberately rejected
	// until guest-address translation and page-fault behavior are implemented.
	if (opcode == 0xff) { // INC/DEC/PUSH and indirect CALL/JMP r/m
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t group = static_cast<std::uint8_t>((modrm >> 3) & 0x07);
		if (!(group == 0x00 || group == 0x01 || group == 0x02 ||
		      group == 0x04 || group == 0x06)) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand operand{};
		if (!DecodeRmOperand(reader, state, modrm, rex, operand)) {
			return ReaderFailure(reader);
		}
		std::uint64_t value = 0;
		std::uint64_t fault_address = 0;
		const bool stack_push = group == 0x06;
		const bool control_flow = group == 0x02 || group == 0x04;
		const bool operand_wide = stack_push || control_flow || wide;
		if (!ReadRmOperand(operand, state, memory, operand_wide, reader.Cursor(),
		                   segment_override, value, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}

		if (group == 0x00 || group == 0x01) { // INC / DEC r/m32 or r/m64
			const bool decrement = group == 0x01;
			const bool operation_wide = wide;
			const std::uint64_t result = decrement ? value - 1 : value + 1;
			const std::uint64_t masked_result =
			    operation_wide ? result : static_cast<std::uint32_t>(result);
			if (!WriteRmOperand(operand, state, memory, operation_wide, reader.Cursor(),
			                    segment_override, masked_result, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			const std::uint64_t old_carry = state.rflags & kFlagCarry;
			UpdateArithmeticFlags(state, decrement ? ArithmeticOperation::Subtract
			                                      : ArithmeticOperation::Add,
			                     value, 1, masked_result, operation_wide);
			state.rflags = (state.rflags & ~kFlagCarry) | old_carry | kRflagsReservedOne;
			state.rip = reader.Cursor();
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		if (stack_push) {
			const std::uint64_t old_stack = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
			const std::uint64_t new_stack = old_stack - sizeof(value);
			std::uint8_t bytes[sizeof(value)]{};
			for (std::size_t i = 0; i < sizeof(value); ++i) {
				bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
			}
			if (!memory.Write(new_stack, bytes, sizeof(bytes))) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, new_stack};
			}
			state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = new_stack;
			state.rip = reader.Cursor();
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		if (control_flow) {
			if (group == 0x02) { // CALL r/m64
				const std::uint64_t return_address = reader.Cursor();
				if (memory.TryDispatchHostCall(value, state, false)) {
					state.rip = return_address;
					return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
				}
				const auto old_stack = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
				const auto new_stack = old_stack - sizeof(return_address);
				std::uint8_t bytes[sizeof(return_address)]{};
				for (std::size_t i = 0; i < sizeof(return_address); ++i) {
					bytes[i] = static_cast<std::uint8_t>(return_address >> (i * 8));
				}
				if (!memory.Write(new_stack, bytes, sizeof(bytes))) {
					return {InstructionStepStatus::MemoryFault, instruction_rip, new_stack};
				}
				state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = new_stack;
			} else if (memory.TryDispatchHostCall(value, state, true)) { // JMP r/m64 tail-call thunk
				const auto stack_pointer = state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)];
				std::uint8_t bytes[sizeof(std::uint64_t)]{};
				if (!memory.Read(stack_pointer, bytes, sizeof(bytes))) {
					return {InstructionStepStatus::MemoryFault, instruction_rip, stack_pointer};
				}
				state.gpr[static_cast<std::size_t>(GuestGpr::Rsp)] = stack_pointer + sizeof(bytes);
				state.rip = ReadLittleEndianU64(bytes);
				return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
			}
			state.rip = value;
			return {InstructionStepStatus::ControlFlow, instruction_rip, opcode};
		}
	}

	if (opcode == 0xf7) { // TEST, NOT, NEG r/m32/r/m64
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t group = static_cast<std::uint8_t>((modrm >> 3) & 0x07);
		if (!(group == 0x00 || group == 0x02 || group == 0x03)) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand operand{};
		if (!DecodeRmOperand(reader, state, modrm, rex, operand)) {
			return ReaderFailure(reader);
		}

		if (group == 0x00) { // TEST r/m, imm32; RIP-relative base includes the immediate
			std::uint64_t immediate = 0;
			if (wide) {
				std::uint32_t encoded = 0;
				if (!ReadLittleEndian(reader, encoded)) {
					return ReaderFailure(reader);
				}
				immediate = static_cast<std::uint64_t>(static_cast<std::int64_t>(
				    std::bit_cast<std::int32_t>(encoded)));
			} else {
				std::uint32_t encoded = 0;
				if (!ReadLittleEndian(reader, encoded)) {
					return ReaderFailure(reader);
				}
				immediate = encoded;
			}
			std::uint64_t value = 0;
			std::uint64_t fault_address = 0;
			if (!ReadRmOperand(operand, state, memory, wide, reader.Cursor(), segment_override,
			                   value, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			const std::uint64_t result = value & (wide ? immediate : static_cast<std::uint32_t>(immediate));
			UpdateLogicalFlags(state, result, wide);
			state.rip = reader.Cursor();
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		const auto next_rip = reader.Cursor();
		std::uint64_t value = 0;
		std::uint64_t fault_address = 0;
		if (!ReadRmOperand(operand, state, memory, wide, next_rip, segment_override,
		                   value, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		if (group == 0x02) { // NOT does not change flags
			const std::uint64_t result = ~value;
			if (!WriteRmOperand(operand, state, memory, wide, next_rip,
			                    segment_override, result, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}
		// NEG is SUB 0, value. CF is set exactly when the operand is nonzero.
		const std::uint64_t result = std::uint64_t{0} - value;
		const std::uint64_t masked_result = wide ? result : static_cast<std::uint32_t>(result);
		if (!WriteRmOperand(operand, state, memory, wide, next_rip,
		                    segment_override, masked_result, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		UpdateArithmeticFlags(state, ArithmeticOperation::Subtract, 0, value,
		                      masked_result, wide);
		state.rip = next_rip;
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0x63) { // MOVSXD r64, r/m32
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		RmOperand source_operand{};
		if (!DecodeRmOperand(reader, state, modrm, rex, source_operand)) {
			return ReaderFailure(reader);
		}
		const auto destination = RegisterIndex(static_cast<std::uint8_t>((modrm >> 3) & 0x07), rex_r);
		std::uint64_t source = 0;
		std::uint64_t fault_address = 0;
		if (!ReadRmOperand(source_operand, state, memory, false, reader.Cursor(),
		                   segment_override, source, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		if (wide) {
			const auto signed_source = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(source));
			WriteGpr(state, destination, true,
			         static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_source)));
		} else {
			WriteGpr(state, destination, false, source);
		}
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0xc7) { // MOV r/m32/r/m64, imm32 (group /0)
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		if (((modrm >> 3) & 0x07) != 0) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand destination{};
		if (!DecodeRmOperand(reader, state, modrm, rex, destination)) {
			return ReaderFailure(reader);
		}
		std::uint32_t encoded = 0;
		if (!ReadLittleEndian(reader, encoded)) {
			return ReaderFailure(reader);
		}
		const auto signed_immediate = std::bit_cast<std::int32_t>(encoded);
		const std::uint64_t immediate = wide
		                                    ? static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_immediate))
		                                    : encoded;
		std::uint64_t fault_address = 0;
		if (!WriteRmOperand(destination, state, memory, wide, reader.Cursor(),
		                    segment_override, immediate, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		state.rip = reader.Cursor();
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	if (opcode == 0x89 || opcode == 0x8b || opcode == 0x8d ||
	    opcode == 0x01 || opcode == 0x03 || opcode == 0x09 || opcode == 0x0b ||
	    opcode == 0x21 || opcode == 0x23 || opcode == 0x29 || opcode == 0x2b ||
	    opcode == 0x31 || opcode == 0x33 ||
	    opcode == 0x39 || opcode == 0x3b || opcode == 0x85) {
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		RmOperand rm{};
		if (!DecodeRmOperand(reader, state, modrm, rex, rm)) {
			return ReaderFailure(reader);
		}
		const auto reg = RegisterIndex(static_cast<std::uint8_t>((modrm >> 3) & 0x07), rex_r);
		const std::uint64_t next_rip = reader.Cursor();

		if (opcode == 0x89) { // MOV r/m32/r/m64, r32/r64
			std::uint64_t fault_address = 0;
			if (!WriteRmOperand(rm, state, memory, wide, next_rip, segment_override,
			                    ReadGpr(state, reg, wide), fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		if (opcode == 0x8b) { // MOV r32/r64, r/m32/r/m64
			std::uint64_t source = 0;
			std::uint64_t fault_address = 0;
			if (!ReadRmOperand(rm, state, memory, wide, next_rip, segment_override,
			                   source, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			WriteGpr(state, reg, wide, source);
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		if (opcode == 0x8d) { // LEA r32/r64, m
			if (rm.is_register) {
				return Unsupported(instruction_rip, opcode);
			}
			WriteGpr(state, reg, wide, EffectiveAddress(rm, next_rip, state, 0, false));
			state.rip = next_rip;
			return {InstructionStepStatus::Continue, instruction_rip, opcode};
		}

		const bool destination_is_rm = opcode == 0x01 || opcode == 0x09 ||
		                               opcode == 0x21 || opcode == 0x29 ||
		                               opcode == 0x31 || opcode == 0x39 || opcode == 0x85;
		const bool is_compare = opcode == 0x39 || opcode == 0x3b;
		const bool is_test = opcode == 0x85;

		std::uint64_t lhs = 0;
		std::uint64_t rhs = 0;
		std::uint64_t fault_address = 0;
		if (destination_is_rm) {
			if (!ReadRmOperand(rm, state, memory, wide, next_rip, segment_override,
			                   lhs, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
			rhs = ReadGpr(state, reg, wide);
		} else {
			lhs = ReadGpr(state, reg, wide);
			if (!ReadRmOperand(rm, state, memory, wide, next_rip, segment_override,
			                   rhs, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
		}

		ArithmeticOperation operation = ArithmeticOperation::Add;
		std::uint64_t result = 0;
		bool is_logical = false;
		if (opcode == 0x01 || opcode == 0x03) {
			operation = ArithmeticOperation::Add;
			result = lhs + rhs;
		} else if (opcode == 0x09 || opcode == 0x0b) {
			result = lhs | rhs;
			is_logical = true;
		} else if (opcode == 0x21 || opcode == 0x23) {
			result = lhs & rhs;
			is_logical = true;
		} else if (opcode == 0x29 || opcode == 0x2b || is_compare) {
			operation = ArithmeticOperation::Subtract;
			result = lhs - rhs;
		} else if (opcode == 0x31 || opcode == 0x33) {
			operation = ArithmeticOperation::Xor;
			result = lhs ^ rhs;
			is_logical = true;
		} else {
			// TEST computes flags from AND without writing the destination.
			result = lhs & rhs;
			is_logical = true;
		}

		if (!wide) {
			result = static_cast<std::uint32_t>(result);
		}
		if (!is_compare && !is_test) {
			bool write_ok = true;
			if (destination_is_rm) {
				write_ok = WriteRmOperand(rm, state, memory, wide, next_rip,
				                          segment_override, result, fault_address);
			} else {
				WriteGpr(state, reg, wide, result);
			}
			if (!write_ok) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
		}
		if (is_logical || is_test) {
			UpdateLogicalFlags(state, result, wide);
		} else {
			UpdateArithmeticFlags(state, operation, lhs, rhs, result, wide);
		}
		state.rip = next_rip;
		return {InstructionStepStatus::Continue, instruction_rip, opcode};
	}

	// Immediate arithmetic/logical group forms used frequently in compiler
	// output: ADD, OR, AND, SUB, XOR, and CMP with imm8/imm32.
	if (opcode == 0x81 || opcode == 0x83) {
		std::uint8_t modrm = 0;
		if (!reader.ReadByte(modrm)) {
			return ReaderFailure(reader);
		}
		const std::uint8_t group = static_cast<std::uint8_t>((modrm >> 3) & 0x07);
		if (!(group == 0 || group == 1 || group == 4 || group == 5 ||
		      group == 6 || group == 7)) {
			return Unsupported(instruction_rip, opcode);
		}
		RmOperand destination{};
		if (!DecodeRmOperand(reader, state, modrm, rex, destination)) {
			return ReaderFailure(reader);
		}
		std::uint64_t immediate = 0;
		if (opcode == 0x83) {
			std::uint8_t encoded = 0;
			if (!ReadLittleEndian(reader, encoded)) {
				return ReaderFailure(reader);
			}
			const auto signed_immediate = std::bit_cast<std::int8_t>(encoded);
			immediate = wide
			                ? static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_immediate))
			                : static_cast<std::uint32_t>(static_cast<std::int32_t>(signed_immediate));
		} else {
			std::uint32_t encoded = 0;
			if (!ReadLittleEndian(reader, encoded)) {
				return ReaderFailure(reader);
			}
			immediate = wide
			                ? static_cast<std::uint64_t>(static_cast<std::int64_t>(
			                      std::bit_cast<std::int32_t>(encoded)))
			                : encoded;
		}

		const auto next_rip = reader.Cursor();
		std::uint64_t lhs = 0;
		std::uint64_t fault_address = 0;
		if (!ReadRmOperand(destination, state, memory, wide, next_rip, segment_override,
		                   lhs, fault_address)) {
			return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
		}
		const std::uint64_t rhs = wide ? immediate : static_cast<std::uint32_t>(immediate);
		std::uint64_t result = 0;
		bool is_compare = false;
		bool is_logical = false;
		ArithmeticOperation operation = ArithmeticOperation::Add;
		switch (group) {
			case 0: operation = ArithmeticOperation::Add; result = lhs + rhs; break;
			case 1: is_logical = true; result = lhs | rhs; break;
			case 4: is_logical = true; result = lhs & rhs; break;
			case 5: operation = ArithmeticOperation::Subtract; result = lhs - rhs; break;
			case 6: is_logical = true; result = lhs ^ rhs; break;
			case 7: operation = ArithmeticOperation::Subtract; result = lhs - rhs; is_compare = true; break;
			default: return Unsupported(instruction_rip, opcode);
		}
		if (!wide) {
			result = static_cast<std::uint32_t>(result);
		}
		if (!is_compare) {
			if (!WriteRmOperand(destination, state, memory, wide, next_rip,
			                    segment_override, result, fault_address)) {
				return {InstructionStepStatus::MemoryFault, instruction_rip, fault_address};
			}
		}
		if (is_logical) {
			UpdateLogicalFlags(state, result, wide);
		} else {
			UpdateArithmeticFlags(state, operation, lhs, rhs, result, wide);
		}
		state.rip = next_rip;
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
			case InstructionStepStatus::SystemCall:
				return {BlockExitReason::SystemCall, state.rip, step.detail};
			case InstructionStepStatus::GuestException:
				return {BlockExitReason::GuestException, step.guest_rip, step.detail};
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
