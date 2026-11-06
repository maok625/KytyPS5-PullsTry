#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
void CompileSrtWalkerNative(ResourcePlan& plan, std::span<const Value> values = {});
bool WalkSrtNative(const ResourcePlan& plan, const SrtRuntime& runtime,
                   std::vector<uint32_t>& flat,
                   std::vector<std::pair<uint64_t, uint64_t>>* reads);
} // namespace Libs::Graphics::ShaderRecompiler::IR

namespace {
using namespace Libs::Graphics::ShaderRecompiler::IR;

void Check(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

Value Emit(ResourcePlan& plan, ValueOpcode opcode, std::initializer_list<Value> args = {},
           uint64_t flags = 0) {
	auto& inst = plan.value_storage.emplace_back(opcode, flags);
	size_t index = 0;
	for (const auto arg: args) inst.SetArg(index++, arg);
	return Value(&inst);
}

struct GuestMemory {
	std::unordered_map<uint64_t, uint32_t> words;
};

bool ReadGuestMemory(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& memory = *static_cast<GuestMemory*>(userdata);
	for (auto& value: values) {
		const auto found = memory.words.find(address);
		if (found == memory.words.end()) return false;
		value = found->second;
		address += sizeof(uint32_t);
	}
	return true;
}

void TestArithmeticAndUserData() {
	ResourcePlan plan;
	plan.user_data_base = 2u;
	const auto user = Emit(plan, ValueOpcode::GetUserData,
	                       {Value(static_cast<ScalarReg>(2u))});
	const auto sum = Emit(plan, ValueOpcode::IAdd32, {user, Value(5u)});
	const auto inserted = Emit(plan, ValueOpcode::BitFieldInsert,
	                           {Value(0u), sum, Value(4u), Value(8u)});
	const std::array values{sum, inserted};
	const std::array<uint32_t, 1> user_data{0x23u};
	const SrtRuntime runtime{.user_data = user_data};
	std::vector<uint32_t> flat;

	CompileSrtWalkerNative(plan, values);
	Check(WalkSrtNative(plan, runtime, flat, nullptr),
	      "native arithmetic walker rejected valid user data");
	uint32_t sum_result = 0, inserted_result = 0;
	Check(WalkedValue(plan, sum, sum_result) && sum_result == 0x28u,
	      "native IAdd32/GetUserData result is incorrect");
	Check(WalkedValue(plan, inserted, inserted_result) && inserted_result == 0x280u,
	      "native BitFieldInsert result is incorrect");
}

void TestGuestReadAndCapturedRange() {
	ResourcePlan plan;
	MemoryInfo memory_info;
	memory_info.kind = ResourceKind::ScalarAddress;
	memory_info.offset = 0x20u;
	plan.memory_info.push_back(memory_info);

	MemoryFlags memory_flags{.index = 0u, .pc = 0x80u};
	uint64_t flags = 0u;
	std::memcpy(&flags, &memory_flags, sizeof(memory_flags));
	const auto address = Emit(plan, ValueOpcode::GetAddressResource,
	                          {Value(0x1000u), Value(0u)});
	const auto read = Emit(plan, ValueOpcode::LoadAddressU32,
	                       {address, Value(0u), Value(0u), Value(true)}, flags);
	const std::array values{read};
	GuestMemory guest{{{0x1020u, 0xfeedbeefu}}};
	const SrtRuntime runtime{
	    .userdata = &guest,
	    .read_specialization_memory = ReadGuestMemory,
	};
	std::vector<uint32_t> flat;
	std::vector<std::pair<uint64_t, uint64_t>> reads;

	CompileSrtWalkerNative(plan, values);
	Check(WalkSrtNative(plan, runtime, flat, &reads),
	      "native walker failed to read guest memory");
	uint32_t result = 0;
	Check(WalkedValue(plan, read, result) && result == 0xfeedbeefu,
	      "native guest-memory load returned the wrong value");
	Check(reads == std::vector<std::pair<uint64_t, uint64_t>>{{0x1020u, 4u}},
	      "native guest-memory load did not capture its four-byte range");

	guest.words.clear();
	Check(WalkSrtNative(plan, runtime, flat, &reads),
	      "native walker failed on an inaccessible guest read");
	Check(WalkedValue(plan, read, result) && result == 0u,
	      "inaccessible guest read did not resolve to zero");
	Check(reads == std::vector<std::pair<uint64_t, uint64_t>>{{0x1020u, 4u}},
	      "inaccessible guest read was not captured");
}

void TestControlFlowAndPhi() {
	ResourcePlan plan;
	plan.user_data_base = 2u;
	const auto condition = Emit(plan, ValueOpcode::GetUserData,
	                            {Value(static_cast<ScalarReg>(2u))});
	auto& phi = plan.value_storage.emplace_back(ValueOpcode::Phi);
	phi.SetFlags(Type::U32);
	phi.AddPhiOperand(nullptr, Value(42u));
	phi.AddPhiOperand(nullptr, Value(9u));
	plan.control_phi_predecessors.emplace(&phi, std::vector<uint32_t>{1u, 2u});
	plan.srt_reads.push_back({.value = Value(&phi), .flat_offset = 0u});
	plan.control_flow.resize(4u);
	plan.control_flow[0].condition = condition;
	plan.control_flow[0].successors = {1u, 2u};
	plan.control_flow[1].successors = {3u};
	plan.control_flow[2].successors = {3u};
	plan.control_flow[3].srt_reads = {0u};

	CompileSrtWalkerNative(plan);
	std::vector<uint32_t> flat;
	const std::array<uint32_t, 1> taken_data{1u};
	const SrtRuntime taken{.user_data = taken_data};
	Check(WalkSrtNative(plan, taken, flat, nullptr),
	      "native walker failed to follow the true control-flow edge");
	Check(flat == std::vector<uint32_t>{42u},
	      "native Phi did not select the reached true predecessor");

	const std::array<uint32_t, 1> fallthrough_data{0u};
	const SrtRuntime fallthrough{.user_data = fallthrough_data};
	Check(WalkSrtNative(plan, fallthrough, flat, nullptr),
	      "native walker failed to follow the false control-flow edge");
	Check(flat == std::vector<uint32_t>{9u},
	      "native Phi did not select the reached false predecessor");
}
} // namespace

namespace Common {
void DbgExitIfHandler(const char*, const char*, int) { std::abort(); }
void DbgExitHandler(const char*, int, std::string_view message) {
	throw std::runtime_error(std::string(message));
}
} // namespace Common

int main() {
	try {
		TestArithmeticAndUserData();
		TestGuestReadAndCapturedRange();
		TestControlFlowAndPhi();
		std::cout << "NativeSrtWalkerTests: all cases passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "NativeSrtWalkerTests: failed: " << error.what() << '\n';
		return 1;
	}
}

#include "../src/graphics/shader/recompiler/ir/Block.cpp"
#include "../src/graphics/shader/recompiler/ir/Program.cpp"
#include "../src/graphics/shader/recompiler/ir/Type.cpp"
#include "../src/graphics/shader/recompiler/ir/Value.cpp"
#include "../src/graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
