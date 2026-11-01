#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#if defined(__arm64__) || defined(__aarch64__)

#include "common/abi.h"
#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

SrtRuntime SrtReadCapture::ObservedRuntime() {
	auto runtime = m_source;
	runtime.userdata = this;
	runtime.read_specialization_memory = m_source.read_specialization_memory != nullptr
	                                         ? Read : nullptr;
	return runtime;
}

bool SrtReadCapture::Read(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<SrtReadCapture*>(userdata);
	if (!capture.m_source.read_specialization_memory(capture.m_source.userdata, address, values)) {
		return false;
	}
	capture.m_ranges.emplace_back(address, values.size_bytes());
	return true;
}

namespace {

bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer &&
	    op != ValueOpcode::LoadBufferU32) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto& memory = values.memory_info[index];
	if (op != ValueOpcode::LoadBufferU32) {
		return (op == ValueOpcode::LoadAddressU32 && memory.kind == ResourceKind::ScalarAddress) ||
		       (op == ValueOpcode::ReadConstBuffer && memory.kind == ResourceKind::ScalarBuffer);
	}
	if (inst.NumArgs() != 5u || memory.kind != ResourceKind::Buffer || memory.typed ||
	    memory.formatted || memory.coherent || memory.data_bits != 32u ||
	    memory.data_dwords != 1u || memory.idxen || memory.offen || memory.offset != 0u ||
	    inst.Arg(4).GetType() != Type::U1) {
		return false;
	}
	for (uint32_t arg = 1; arg <= 3; ++arg) {
		if (inst.Arg(arg).Resolve() != Value(0u)) return false;
	}
	return true;
}

bool IsDescriptorHandle(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::GetImageResource:
		case ValueOpcode::GetSamplerResource: return true;
		default: return false;
	}
}

bool IsRuntimeSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32;
}

bool IsRuntimeUniformOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ConditionRef:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertS32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::UMulHi:
		case ValueOpcode::UMin32:
		case ValueOpcode::UMax32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

bool ImmediateBits(Value value, uint64_t& bits) {
	switch (value.GetType()) {
		case Type::U1: bits = value.U1(); return true;
		case Type::U8: bits = value.U8(); return true;
		case Type::U16: bits = value.U16(); return true;
		case Type::U32: bits = value.U32(); return true;
		case Type::U64: bits = value.U64(); return true;
		case Type::F32: bits = std::bit_cast<uint32_t>(value.F32Value()); return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type,
	                          bool wave_uniform = false)
	    : m_program(program), m_type(type), m_wave_uniform(wave_uniform) {}

	bool Run(Value value) { return Validate(value); }

private:
	bool ValidateArguments(const Inst& inst, bool require_uniform) {
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			if (!Validate(inst.Arg(index), require_uniform)) return false;
		}
		return true;
	}

	bool Validate(Value value, bool require_uniform = true) {
		value = value.Resolve();
		if (m_type != RuntimeValueType::Any &&
		    TypesOverlap(value.GetType(), Type::F16 | Type::F32 | Type::F32x2)) {
			return false;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			if (!require_uniform) return true;
			switch (value.GetType()) {
				case Type::U1:
				case Type::U8:
				case Type::U16:
				case Type::U32:
				case Type::U64:
				case Type::F32: return true;
				default: return false;
			}
		}
		if (require_uniform && !m_active_mask.IsEmpty() && value == m_active_mask) return true;
		if (!require_uniform && m_validated_dependencies.contains(inst)) return true;
		if (!m_visiting.insert(inst).second) {
			return !require_uniform;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			if (valid && !require_uniform) m_validated_dependencies.insert(inst);
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (m_type == RuntimeValueType::ImmutableInteger &&
		    (op == ValueOpcode::ReadConst || BufferAccessOf(op) != BufferAccess::None ||
		     AddressOpcodeInfoOf(op).access != AddressAccess::None ||
		     ImageOpcodeInfoOf(op).access != ImageAccess::None ||
		     SharedAccessOf(op) != SharedAccess::None)) {
			return finish(false);
		}
		if (op == ValueOpcode::ReadConst) {
			const auto slot = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (inst->NumArgs() != 2 || inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetSrtResource ||
			    !slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return finish(false);
			}
			if (m_type != RuntimeValueType::Any) {
				const auto active_mask = m_active_mask;
				m_active_mask          = {};
				const bool valid       = Validate(m_program.srt_reads[slot.U32()].value);
				m_active_mask          = active_mask;
				if (!valid) return finish(false);
			}
		}
		if (!require_uniform) return finish(ValidateArguments(*inst, false));
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
			if (m_type != RuntimeValueType::Any && !Validate(inst->Arg(2), false)) {
				return finish(false);
			}
			return finish(Validate(inst->Arg(1)));
		}
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU32 || op == ValueOpcode::Void) {
			return finish(false);
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				return finish(false);
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			return finish(inst->NumArgs() == 0);
		}
		if (m_wave_uniform && op == ValueOpcode::GetBuiltin) {
			const auto axis = inst->NumArgs() == 2u ? inst->Arg(1).Resolve() : Value {};
			return finish(m_program.stage == ShaderType::Compute && inst->NumArgs() == 2u &&
			              inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)) &&
			              axis.IsImmediate() && axis.GetType() == Type::U32 && axis.U32() < 3u);
		}
		if (op == ValueOpcode::Phi) {
			if (m_type != RuntimeValueType::Any && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) return finish(false);
			return finish(Validate(invariant));
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
			}
			if (m_type != RuntimeValueType::Any && !Validate(inst->Arg(1), false)) {
				return finish(false);
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = inst->Arg(1).Resolve();
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::GetSrtResource) {
			return finish(inst->NumArgs() == 0);
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer ||
		    op == ValueOpcode::LoadBufferU32) {
			const auto expected = op == ValueOpcode::LoadAddressU32
			                          ? ValueOpcode::GetAddressResource
			                          : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				return finish(false);
			}
			if (op == ValueOpcode::LoadBufferU32) {
				const auto guard = inst->Arg(4).Resolve();
				return finish(Validate(inst->Arg(0)) &&
				              ((!m_active_mask.IsEmpty() && guard == m_active_mask) || Validate(guard)));
			}
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u || source->GetOpcode() != ValueOpcode::CompositeConstructU32x2) {
				return finish(false);
			}
		}
		if (IsDescriptorHandle(op)) {
			size_t expected = 4u;
			if (op == ValueOpcode::GetImageResource) expected = 8u;
			else if (op == ValueOpcode::GetAddressResource) expected = 2u;
			if (inst->NumArgs() != expected) return finish(false);
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeUniformOp(op)) {
			return finish(false);
		}
		return finish(ValidateArguments(*inst, true));
	}

	const ResourcePlan& m_program;
	RuntimeValueType m_type;
	bool m_wave_uniform;
	Value m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
};

constexpr uint32_t DescriptorStride = 9u * sizeof(uint32_t);

uint32_t ActiveOffset(const ResourcePlan& plan) {
	return static_cast<uint32_t>(plan.descriptor_sources.size()) * DescriptorStride;
}

uint32_t OutputOffset(const ResourcePlan& plan) {
	const auto active = plan.descriptor_sources.size() + plan.info.images.size();
	return (ActiveOffset(plan) + static_cast<uint32_t>(active) + 7u) & ~7u;
}

const uint8_t* Scratch(const ResourcePlan& plan) {
	return reinterpret_cast<const uint8_t*>(plan.srt_scratch.data());
}

struct WalkerOutput {
	Value value;
	std::vector<uint32_t> gates;
};

std::vector<WalkerOutput> CollectOutputs(const ResourcePlan& plan, std::span<const Value> values) {
	std::vector<WalkerOutput> outputs;
	std::unordered_map<Inst*, uint32_t> indices;
	const auto add_output = [&](Value value, uint32_t gate) {
		auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr) return;
		const auto found = indices.find(inst);
		uint32_t index = 0;
		if (found == indices.end()) {
			index = static_cast<uint32_t>(outputs.size());
			indices.emplace(inst, index);
			outputs.push_back({value.Resolve(), {}});
			inst->SetWalkOutput(index);
		} else {
			index = found->second;
		}
		outputs[index].gates.push_back(gate);
	};
	const auto& sources = plan.descriptor_sources;
	const auto consume = [&](uint32_t root) {
		if (root >= sources.size() || !sources[root].indirect_descriptor) return;
		const auto& indirect = *sources[root].indirect_descriptor;
		add_output(indirect.key_count, root);
		add_output(indirect.selector_first, root);
		add_output(indirect.selector_mask, root);
	};
	for (const auto& buffer: plan.info.buffers) consume(buffer.source);
	for (const auto& image: plan.info.images) consume(image.source);
	for (const auto& sampler: plan.info.samplers) {
		const auto* indirect =
		    sampler.source < sources.size() && sources[sampler.source].indirect_descriptor
		        ? &*sources[sampler.source].indirect_descriptor : nullptr;
		if (indirect == nullptr || indirect->sources.empty()) {
			consume(sampler.source);
		} else if (indirect->sources[0] < sources.size()) {
			unused_gates[indirect->sources[0]].push_back(indirect->sources[0]);
		}
	}
	const auto& fill = plan.uniform_fill;
	if (fill.fill.words != 0) {
		add_output(fill.pattern_size, UINT32_MAX);
		add_output(fill.element_count, UINT32_MAX);
		for (const auto value: fill.values) add_output(value, UINT32_MAX);
	}
	for (const auto value: values) add_output(value, UINT32_MAX);
	return outputs;
}

bool GateActive(const std::vector<uint32_t>& gates, const std::vector<uint8_t>& active) {
	if (gates.empty()) return true;
	for (const auto gate: gates) {
		if (gate == UINT32_MAX) return true;
		if (gate < active.size() && active[gate] != 0) return true;
	}
	return false;
}

class RuntimeEvaluator {
public:
	RuntimeEvaluator(const ResourcePlan& plan, const SrtRuntime& runtime,
	                 const std::vector<uint8_t>& reachable,
	                 std::vector<std::pair<uint64_t, uint64_t>>* reads)
	    : m_plan(plan), m_runtime(runtime), m_reachable(reachable), m_reads(reads) {
		m_masks.emplace_back();
	}

	void Reset() { m_cache.clear(); }

	std::optional<uint64_t> Evaluate(Value value, uint32_t context = 0) {
		value = value.Resolve();
		if (value.IsEmpty()) return {};
		if (value.IsImmediate()) {
			uint64_t bits = 0;
			if (!ImmediateBits(value, bits)) return {};
			return bits;
		}
		if (context != 0 && context < m_masks.size() && value == m_masks[context]) return 1u;
		auto* inst = value.TryInstruction();
		if (inst == nullptr) return {};
		const auto key = std::pair {static_cast<const Inst*>(inst), context};
		const auto found = m_cache.find(key);
		if (found != m_cache.end()) {
			if (found->second.evaluating) return {};
			return found->second.value;
		}
		auto [entry, inserted] = m_cache.try_emplace(key);
		(void)inserted;
		entry->second.evaluating = true;
		entry->second.value = EvaluateInstruction(*inst, context);
		entry->second.evaluating = false;
		return entry->second.value;
	}

private:
	uint32_t Context(Value mask) {
		mask = mask.Resolve();
		for (uint32_t index = 1; index < m_masks.size(); ++index) {
			if (m_masks[index] == mask) return index;
		}
		m_masks.push_back(mask);
		return static_cast<uint32_t>(m_masks.size() - 1u);
	}

	std::optional<uint64_t> EvaluateInstruction(const Inst& inst, uint32_t context) {
		const auto op = inst.GetOpcode();
		if (context != 0 && context < m_masks.size() && IsRuntimeSelect(op) &&
		    inst.NumArgs() == 3 && inst.Arg(0).Resolve() == m_masks[context]) {
			return Evaluate(inst.Arg(1), context);
		}
		switch (op) {
			case ValueOpcode::GetUserData: {
				if (inst.NumArgs() != 1 || inst.Arg(0).GetType() != Type::ScalarReg) return {};
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_plan.user_data_base) return {};
				const auto index = reg - m_plan.user_data_base;
				if (index >= m_plan.user_data_count || index >= m_runtime.user_data.size()) return {};
				return m_runtime.user_data[index];
			}
			case ValueOpcode::GetShaderBase:
				if (inst.NumArgs() != 0) return {};
				return m_runtime.shader_base;
			case ValueOpcode::GetSrtResource:
				return {};
			case ValueOpcode::ReadConst: {
				if (inst.NumArgs() != 2) return {};
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_plan.srt_reads.size()) return {};
				return Evaluate(m_plan.srt_reads[slot.U32()].value, context);
			}
			case ValueOpcode::ReadFirstLane:
				if (inst.NumArgs() != 2) return {};
				return Evaluate(inst.Arg(0), Context(inst.Arg(1)));
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
			case ValueOpcode::LoadBufferU32:
				return EvaluateRead(inst, context);
			case ValueOpcode::Phi:
				return EvaluatePhi(inst, context);
			case ValueOpcode::CompositeExtractU32x2: {
				if (inst.NumArgs() != 2) return {};
				const auto* source = inst.Arg(0).ResolveInstruction();
				const auto index = inst.Arg(1).Resolve();
				if (source == nullptr || source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 ||
				    !index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) return {};
				return Evaluate(source->Arg(index.U32()), context);
			}
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectF32: {
				if (inst.NumArgs() != 3) return {};
				const auto predicate = Evaluate(inst.Arg(0), context);
				if (!predicate) return {};
				return Evaluate(inst.Arg(*predicate != 0 ? 1u : 2u), context);
			}
			case ValueOpcode::LogicalAnd:
			case ValueOpcode::LogicalOr: {
				if (inst.NumArgs() != 2) return {};
				const bool conjunction = op == ValueOpcode::LogicalAnd;
				const auto left = Evaluate(inst.Arg(0), context);
				if (left && ((*left == 0u) == conjunction)) return uint64_t {!conjunction};
				const auto right = Evaluate(inst.Arg(1), context);
				if (right && ((*right == 0u) == conjunction)) return uint64_t {!conjunction};
				if (!left) {
					if (conjunction && right && *right != 0u) {
						const auto refined = Evaluate(inst.Arg(1), Context(inst.Arg(0)));
						if (refined && *refined == 0u) return 0u;
					}
					return {};
				}
				if (!right) return {};
				return uint64_t {conjunction ? (*left != 0u && *right != 0u)
				                             : (*left != 0u || *right != 0u)};
			}
			default:
				break;
		}

		if (op == ValueOpcode::ULessThan32 || op == ValueOpcode::ULessThanEqual32) {
			const auto left = Evaluate(inst.Arg(0), context);
			const auto right = Evaluate(inst.Arg(1), context);
			if (op == ValueOpcode::ULessThan32 && right && static_cast<uint32_t>(*right) == 0u)
				return 0u;
			if (op == ValueOpcode::ULessThanEqual32 && left && static_cast<uint32_t>(*left) == 0u)
				return 1u;
			if (!left || !right) return {};
			return uint64_t {op == ValueOpcode::ULessThan32
			                     ? static_cast<uint32_t>(*left) < static_cast<uint32_t>(*right)
			                     : static_cast<uint32_t>(*left) <= static_cast<uint32_t>(*right)};
		}
		if (!IsRuntimeUniformOp(op) || inst.NumArgs() > 4u) return {};
		std::array<uint64_t, 4> args {};
		for (size_t index = 0; index < inst.NumArgs(); ++index) {
			const auto value = Evaluate(inst.Arg(index), context);
			if (!value) return {};
			args[index] = *value;
		}
		return EvaluateOperation(inst, std::span(args).first(inst.NumArgs()));
	}

	std::optional<uint64_t> EvaluatePhi(const Inst& inst, uint32_t context) {
		const auto found = m_plan.control_phi_predecessors.find(&inst);
		if (found == m_plan.control_phi_predecessors.end()) {
			const auto invariant = ResolveInvariantPhi(m_plan, Value(const_cast<Inst*>(&inst)));
			if (invariant.IsEmpty() || invariant == Value(const_cast<Inst*>(&inst))) return {};
			return Evaluate(invariant, context);
		}
		if (found->second.size() != inst.NumArgs()) return {};
		bool have_value = false;
		uint64_t result = 0;
		for (size_t index = 0; index < inst.NumArgs(); ++index) {
			const auto predecessor = found->second[index];
			if (predecessor >= m_reachable.size() || m_reachable[predecessor] == 0) continue;
			const auto value = Evaluate(inst.Arg(index), context);
			if (!value) return {};
			if (!have_value) {
				result = *value;
				have_value = true;
			} else if (result != *value) {
				return {};
			}
		}
		if (!have_value) return {};
		return result;
	}

	std::optional<uint64_t> EvaluateRead(const Inst& inst, uint32_t context) {
		if (!IsRawRead(m_plan, inst) || m_runtime.read_specialization_memory == nullptr) return {};
		const auto op = inst.GetOpcode();
		if (op == ValueOpcode::LoadBufferU32) {
			const auto guard = Evaluate(inst.Arg(4), context);
			if (!guard) return {};
			if (*guard == 0u) return 0u;
		}
		const auto* handle = inst.NumArgs() != 0 ? inst.Arg(0).ResolveInstruction() : nullptr;
		const auto expected = op == ValueOpcode::LoadAddressU32
		                          ? ValueOpcode::GetAddressResource
		                          : ValueOpcode::GetBufferResource;
		if (handle == nullptr || handle->GetOpcode() != expected) return {};
		if (handle->NumArgs() < (op == ValueOpcode::LoadAddressU32 ? 2u : 4u)) return {};
		const auto low = Evaluate(handle->Arg(0), context);
		const auto high = Evaluate(handle->Arg(1), context);
		const auto offset_value = Evaluate(inst.Arg(1), context);
		if (!low || !high || !offset_value) return {};
		const auto low32 = static_cast<uint32_t>(*low);
		const auto high32 = static_cast<uint32_t>(*high);
		const uint64_t base = (static_cast<uint64_t>(high32 & 0xffffu) << 32u) |
		                      (static_cast<uint64_t>(low32) & ~uint64_t {3});
		const auto& memory = m_plan.memory_info[inst.Flags<MemoryFlags>().index];
		const auto immediate =
		    static_cast<int64_t>(static_cast<int32_t>(memory.offset)) & ~int64_t {3};
		const uint64_t delta = (static_cast<uint64_t>(static_cast<uint32_t>(*offset_value) & ~3u)) +
		                       static_cast<uint64_t>(immediate);
		const uint64_t address = base + delta;

		if (op != ValueOpcode::LoadAddressU32) {
			const uint32_t stride = (high32 >> 16u) & 0x3fffu;
			const auto length = Evaluate(handle->Arg(2), context);
			const auto flags = Evaluate(handle->Arg(3), context);
			if (!length || !flags) return {};
			if (op == ValueOpcode::LoadBufferU32) {
				const auto guard = Evaluate(inst.Arg(4), context);
				if (!guard) return {};
				if (*guard == 0u) return 0u;
				if ((high32 & 0x80000000u) != 0u ||
				    (static_cast<uint32_t>(*flags) & ((1u << 23u) | 0xf0000000u)) != 0u) {
					return {};
				}
				if (stride == 0u ||
				    (static_cast<uint32_t>(*flags) & (0x7fu << 12u)) == 0u || *length == 0u) {
					return 0u;
				}
			}
			const uint64_t size = stride == 0u ? *length : *length * stride;
			const uint64_t end = delta + sizeof(uint32_t);
			if (end > size) return 0u;
		} else if ((address >> 48u) != 0u) {
			return {};
		}

		std::array<uint32_t, 1> word {};
		// The x86 walker treats a guest load fault as a zero read and resumes after the load.
		// A failed backing-store probe is the native ARM64 equivalent.
		const bool available = m_runtime.read_specialization_memory(m_runtime.userdata, address, word);
		if (m_reads != nullptr) m_reads->emplace_back(address, sizeof(uint32_t));
		return available ? std::optional<uint64_t>(word[0]) : std::optional<uint64_t>(0u);
	}

	std::optional<uint64_t> EvaluateOperation(const Inst& inst, std::span<const uint64_t> a) {
		const auto op = inst.GetOpcode();
		const auto arg = [&](size_t index) { return a[index]; };
		const auto boolean = [](bool value) -> std::optional<uint64_t> { return uint64_t {value}; };
		switch (op) {
			case ValueOpcode::ConditionRef:
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return arg(0);
			case ValueOpcode::CompositeConstructU64:
				return (arg(0) << 32u) | static_cast<uint32_t>(arg(1));
			case ValueOpcode::CompositeExtractU64: {
				const auto index = inst.Arg(1).Resolve().U32();
				return static_cast<uint32_t>(arg(0) >> (index * 32u));
			}
			case ValueOpcode::BitFieldUExtract:
			case ValueOpcode::BitFieldSExtract: {
				const auto offset = static_cast<uint32_t>(arg(1));
				const auto width = static_cast<uint32_t>(arg(2));
				if (static_cast<uint64_t>(offset) + width > 32u) return {};
				const uint32_t mask = width == 32u ? UINT32_MAX
				                                    : (width == 0u ? 0u : (uint32_t {1} << width) - 1u);
				uint32_t result = width == 0u ? 0u : (static_cast<uint32_t>(arg(0)) >> offset) & mask;
				if (op == ValueOpcode::BitFieldSExtract && width != 0u &&
				    (result & (uint32_t {1} << (width - 1u))) != 0u) {
					result |= ~mask;
				}
				return result;
			}
			case ValueOpcode::BitFieldInsert: {
				const auto offset = static_cast<uint32_t>(arg(2));
				const auto width = static_cast<uint32_t>(arg(3));
				if (static_cast<uint64_t>(offset) + width > 32u) return {};
				const uint32_t mask = width == 32u ? UINT32_MAX
				                                    : (width == 0u ? 0u : (uint32_t {1} << width) - 1u);
				if (width == 0u) return static_cast<uint32_t>(arg(0));
				const uint32_t shifted = offset == 32u ? 0u : mask << offset;
				const uint32_t inserted = (static_cast<uint32_t>(arg(1)) & mask) << offset;
				return static_cast<uint32_t>((static_cast<uint32_t>(arg(0)) & ~shifted) | inserted);
			}
			case ValueOpcode::IAdd32: return static_cast<uint32_t>(arg(0) + arg(1));
			case ValueOpcode::IAdd64: return arg(0) + arg(1);
			case ValueOpcode::ISub32: return static_cast<uint32_t>(arg(0) - arg(1));
			case ValueOpcode::ISub64: return arg(0) - arg(1);
			case ValueOpcode::IMul32: return static_cast<uint32_t>(arg(0) * arg(1));
			case ValueOpcode::UMulHi:
				return static_cast<uint32_t>((static_cast<uint64_t>(static_cast<uint32_t>(arg(0))) *
				                              static_cast<uint32_t>(arg(1))) >> 32u);
			case ValueOpcode::UMin32:
				return std::min(static_cast<uint32_t>(arg(0)), static_cast<uint32_t>(arg(1)));
			case ValueOpcode::UMax32:
				return std::max(static_cast<uint32_t>(arg(0)), static_cast<uint32_t>(arg(1)));
			case ValueOpcode::ShiftLeftLogical32:
				return static_cast<uint32_t>(arg(0)) << (static_cast<uint32_t>(arg(1)) & 31u);
			case ValueOpcode::ShiftLeftLogical64: return arg(0) << (static_cast<uint32_t>(arg(1)) & 63u);
			case ValueOpcode::ShiftRightLogical32:
				return static_cast<uint32_t>(arg(0)) >> (static_cast<uint32_t>(arg(1)) & 31u);
			case ValueOpcode::ShiftRightLogical64: return arg(0) >> (static_cast<uint32_t>(arg(1)) & 63u);
			case ValueOpcode::ShiftRightArithmetic32:
				return static_cast<uint32_t>(static_cast<int32_t>(arg(0)) >>
				                             (static_cast<uint32_t>(arg(1)) & 31u));
			case ValueOpcode::ShiftRightArithmetic64:
				return static_cast<uint64_t>(static_cast<int64_t>(arg(0)) >>
				                             (static_cast<uint32_t>(arg(1)) & 63u));
			case ValueOpcode::BitwiseAnd32:
				return static_cast<uint32_t>(arg(0)) & static_cast<uint32_t>(arg(1));
			case ValueOpcode::BitwiseAnd64: return arg(0) & arg(1);
			case ValueOpcode::BitwiseOr32:
				return static_cast<uint32_t>(arg(0)) | static_cast<uint32_t>(arg(1));
			case ValueOpcode::BitwiseXor32:
				return static_cast<uint32_t>(arg(0)) ^ static_cast<uint32_t>(arg(1));
			case ValueOpcode::BitwiseNot32: return static_cast<uint32_t>(~static_cast<uint32_t>(arg(0)));
			case ValueOpcode::ULessThan32: return boolean(static_cast<uint32_t>(arg(0)) < static_cast<uint32_t>(arg(1)));
			case ValueOpcode::ULessThanEqual32:
				return boolean(static_cast<uint32_t>(arg(0)) <= static_cast<uint32_t>(arg(1)));
			case ValueOpcode::IEqual32:
				return boolean(static_cast<uint32_t>(arg(0)) == static_cast<uint32_t>(arg(1)));
			case ValueOpcode::IEqual64: return boolean(arg(0) == arg(1));
			case ValueOpcode::INotEqual32:
				return boolean(static_cast<uint32_t>(arg(0)) != static_cast<uint32_t>(arg(1)));
			case ValueOpcode::INotEqual64: return boolean(arg(0) != arg(1));
			case ValueOpcode::UGreaterThan32:
				return boolean(static_cast<uint32_t>(arg(0)) > static_cast<uint32_t>(arg(1)));
			case ValueOpcode::SGreaterThanEqual32:
				return boolean(static_cast<int32_t>(arg(0)) >= static_cast<int32_t>(arg(1)));
			case ValueOpcode::LogicalXor: return boolean((arg(0) != 0u) != (arg(1) != 0u));
			case ValueOpcode::LogicalNot: return boolean(arg(0) == 0u);
			case ValueOpcode::ConvertF32U32: {
				const auto number = static_cast<float>(static_cast<int64_t>(arg(0)));
				return std::bit_cast<uint32_t>(number);
			}
			case ValueOpcode::ConvertU32F32: {
				const float number = std::bit_cast<float>(static_cast<uint32_t>(arg(0)));
				if (std::isnan(number) || number <= 0.0f) return 0u;
				if (number >= 4294967296.0f) return UINT32_MAX;
				return static_cast<uint32_t>(number);
			}
			case ValueOpcode::ConvertS32F32: {
				const float number = std::bit_cast<float>(static_cast<uint32_t>(arg(0)));
				if (std::isnan(number)) return 0u;
				if (number >= 2147483648.0f) return static_cast<uint32_t>(INT32_MAX);
				if (number <= -2147483648.0f) return static_cast<uint32_t>(INT32_MIN);
				return static_cast<uint32_t>(static_cast<int32_t>(number));
			}
			case ValueOpcode::FPMul32: {
				const float left = std::bit_cast<float>(static_cast<uint32_t>(arg(0)));
				const float right = std::bit_cast<float>(static_cast<uint32_t>(arg(1)));
				return std::bit_cast<uint32_t>(left * right);
			}
			case ValueOpcode::FPTrunc32: {
				const float value = std::bit_cast<float>(static_cast<uint32_t>(arg(0)));
				return std::bit_cast<uint32_t>(std::trunc(value));
			}
			case ValueOpcode::FPRecipIFlag32: {
				const auto bits = static_cast<uint32_t>(arg(0));
				const auto exponent = bits >> 23u;
				if ((bits & 0x807fffffu) != 0u || exponent == 0u || exponent >= 254u) return {};
				return (254u - exponent) << 23u;
			}
			case ValueOpcode::FPIsNan32: {
				const auto bits = static_cast<uint32_t>(arg(0)) & 0x7fffffffu;
				return boolean(bits > 0x7f800000u);
			}
			case ValueOpcode::FPOrdLessThanEqual32:
			case ValueOpcode::FPOrdGreaterThanEqual32: {
				auto left_bits = static_cast<uint32_t>(arg(0));
				auto right_bits = static_cast<uint32_t>(arg(1));
				if (inst.Flags<FPCompareFlags>().flush_input_denorms) {
					if ((left_bits & 0x7f800000u) == 0u) left_bits &= 0x80000000u;
					if ((right_bits & 0x7f800000u) == 0u) right_bits &= 0x80000000u;
				}
				const float left = std::bit_cast<float>(left_bits);
				const float right = std::bit_cast<float>(right_bits);
				if (std::isnan(left) || std::isnan(right)) return 0u;
				return boolean(op == ValueOpcode::FPOrdLessThanEqual32 ? left <= right : left >= right);
			}
			default: return {};
		}
	}

	const ResourcePlan& m_plan;
	const SrtRuntime& m_runtime;
	const std::vector<uint8_t>& m_reachable;
	std::vector<std::pair<uint64_t, uint64_t>>* m_reads;
	std::vector<Value> m_masks;
	struct CacheEntry {
		bool evaluating = false;
		std::optional<uint64_t> value;
	};
	std::map<std::pair<const Inst*, uint32_t>, CacheEntry> m_cache;
};

} // namespace

void CompileSrtWalker(ResourcePlan& plan, std::span<const Value> values) {
	const auto outputs = CollectOutputs(plan, values);
	const auto bytes = static_cast<size_t>(OutputOffset(plan)) + outputs.size() * sizeof(uint64_t);
	plan.srt_scratch.assign(std::max<size_t>(1u, (bytes + sizeof(uint64_t) - 1u) / sizeof(uint64_t)), 0u);
	plan.srt_user_data = 0u; // The interpreter checks each actual user-data access.
	plan.srt_captures = static_cast<uint32_t>(bytes);
}

bool WalkSrt(const ResourcePlan& plan, const SrtRuntime& runtime, std::vector<uint32_t>& flat,
             std::vector<std::pair<uint64_t, uint64_t>>* reads) {
	if (runtime.read_specialization_memory == nullptr && !plan.srt_reads.empty()) return false;
	flat.assign(plan.srt_reads.size(), 0u);
	const auto outputs = CollectOutputs(plan, {});
	const auto output_offset = OutputOffset(plan);
	const auto required_bytes = static_cast<size_t>(output_offset) + outputs.size() * sizeof(uint64_t);
	if (plan.srt_scratch.size() * sizeof(uint64_t) < required_bytes) return false;
	std::fill(plan.srt_scratch.begin(), plan.srt_scratch.end(), 0u);
	auto* scratch = reinterpret_cast<uint8_t*>(plan.srt_scratch.data());

	std::vector<uint8_t> reachable(plan.control_flow.size(), 0u);
	if (!reachable.empty()) {
		reachable[0] = 1u;
		for (size_t pass = 0; pass <= plan.control_flow.size(); ++pass) {
			std::vector<std::pair<uint64_t, uint64_t>> pass_reads;
			RuntimeEvaluator pass_evaluator(plan, runtime, reachable,
			                               reads != nullptr ? &pass_reads : nullptr);
			bool changed = false;
			for (uint32_t block = 0; block < plan.control_flow.size(); ++block) {
				if (reachable[block] == 0u) continue;
				const auto& successors = plan.control_flow[block].successors;
				if (successors.empty()) continue;
				const auto condition = pass_evaluator.Evaluate(plan.control_flow[block].condition);
				const auto mark = [&](uint32_t target) {
					if (target >= reachable.size()) return false;
					if (reachable[target] != 0u) return true;
					reachable[target] = 1u;
					changed = true;
					return true;
				};
				if (condition && successors.size() >= 2u) {
					if (!mark(successors[*condition != 0u ? 0u : 1u])) return false;
				} else {
					for (const auto target: successors) {
						if (!mark(target)) return false;
					}
				}
			}
			if (!changed) {
				// Match the JIT's reset behavior: only capture reads from the stable, final
				// reachability pass, not speculative reads from earlier passes.
				if (reads != nullptr) {
					reads->insert(reads->end(), pass_reads.begin(), pass_reads.end());
				}
				break;
			}
			if (pass == plan.control_flow.size()) return false;
		}
	}

	std::vector<uint8_t> source_active(plan.descriptor_sources.size(), 0u);
	std::vector<uint8_t> image_active(plan.info.images.size(), 0u);
	for (uint32_t source = 0; source < source_active.size(); ++source) {
		bool listed = false;
		for (uint32_t block = 0; block < plan.control_flow.size(); ++block) {
			const auto& sources = plan.control_flow[block].sources;
			if (std::ranges::find(sources, source) == sources.end()) continue;
			listed = true;
			source_active[source] |= reachable[block];
		}
		if (!listed) source_active[source] = 1u;
	}
	for (uint32_t image = 0; image < image_active.size(); ++image) {
		bool listed = false;
		for (uint32_t block = 0; block < plan.control_flow.size(); ++block) {
			const auto& images = plan.control_flow[block].images;
			if (std::ranges::find(images, image) == images.end()) continue;
			listed = true;
			image_active[image] |= reachable[block];
		}
		if (!listed) image_active[image] = 1u;
	}
	if (plan.control_flow.empty()) {
		std::fill(source_active.begin(), source_active.end(), 1u);
		std::fill(image_active.begin(), image_active.end(), 1u);
	}
	if (!source_active.empty()) {
		std::memcpy(scratch + ActiveOffset(plan), source_active.data(), source_active.size());
	}
	if (!image_active.empty()) {
		std::memcpy(scratch + ActiveOffset(plan) + source_active.size(),
		            image_active.data(), image_active.size());
	}

	RuntimeEvaluator evaluator(plan, runtime, reachable, reads);
	const auto read_slot = [&](uint32_t slot) {
		if (slot >= plan.srt_reads.size()) return false;
		const auto offset = plan.srt_reads[slot].flat_offset;
		if (offset >= flat.size()) return false;
		const auto value = evaluator.Evaluate(plan.srt_reads[slot].value);
		if (!value) return false;
		flat[offset] = static_cast<uint32_t>(*value);
		return true;
	};
	if (plan.control_flow.empty()) {
		for (uint32_t slot = 0; slot < plan.srt_reads.size(); ++slot) {
			if (!read_slot(slot)) return false;
		}
	} else {
		for (uint32_t block = 0; block < plan.control_flow.size(); ++block) {
			if (reachable[block] == 0u) continue;
			for (const auto slot: plan.control_flow[block].srt_reads) {
				if (!read_slot(slot)) return false;
			}
		}
	}

	std::vector<std::vector<uint32_t>> gates(plan.descriptor_sources.size());
	const auto add_source = [&](uint32_t source, uint32_t root) {
		if (source < gates.size()) gates[source].push_back(root);
	};
	const auto consume = [&](uint32_t root) {
		if (root >= plan.descriptor_sources.size()) return;
		const auto& indirect = plan.descriptor_sources[root].indirect_descriptor;
		if (!indirect) {
			add_source(root, root);
			return;
		}
		if (indirect->sources.empty()) {
			add_source(indirect->table_source, root);
			if (indirect->selector) {
				add_source(indirect->selector->source, root);
				if (indirect->selector->rows) add_source(indirect->selector->rows->source, root);
			}
		}
		for (const auto source: indirect->sources) add_source(source, root);
	};
	for (const auto& buffer: plan.info.buffers) consume(buffer.source);
	for (const auto& image: plan.info.images) consume(image.source);
	for (const auto& sampler: plan.info.samplers) {
		const auto* indirect =
		    sampler.source < plan.descriptor_sources.size() &&
		            plan.descriptor_sources[sampler.source].indirect_descriptor
		        ? &*plan.descriptor_sources[sampler.source].indirect_descriptor : nullptr;
		if (indirect == nullptr || indirect->sources.empty()) {
			consume(sampler.source);
		} else if (!indirect->sources.empty()) {
			add_source(indirect->sources[0], indirect->sources[0]);
		}
	}
	for (uint32_t source = 0; source < plan.descriptor_sources.size(); ++source) {
		if (gates[source].empty() || !GateActive(gates[source], source_active)) continue;
		const auto& descriptor = plan.descriptor_sources[source];
		if (descriptor.dword_count > 8u) continue;
		bool known = true;
		for (uint32_t word = 0; word < descriptor.dword_count; ++word) {
			const auto value = evaluator.Evaluate(descriptor.dwords[word]);
			if (!value) {
				known = false;
				break;
			}
			const auto dword = static_cast<uint32_t>(*value);
			std::memcpy(scratch + source * DescriptorStride + word * sizeof(uint32_t),
			            &dword, sizeof(dword));
		}
		if (known) {
			const uint32_t marker = 1u;
			std::memcpy(scratch + source * DescriptorStride + 8u * sizeof(uint32_t),
			            &marker, sizeof(marker));
		}
	}

	for (uint32_t index = 0; index < outputs.size(); ++index) {
		uint64_t bits = UINT64_MAX;
		if (GateActive(outputs[index].gates, source_active)) {
			const auto value = evaluator.Evaluate(outputs[index].value);
			if (value) bits = *value;
		}
		std::memcpy(scratch + output_offset + index * sizeof(uint64_t), &bits, sizeof(bits));
	}
	return true;
}

bool WalkedValue(const ResourcePlan& plan, Value value, uint32_t& result) {
	value = value.Resolve();
	uint64_t bits = 0;
	if (value.IsImmediate()) {
		if (!ImmediateBits(value, bits)) return false;
		result = static_cast<uint32_t>(bits);
		return true;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || inst->WalkOutput() == UINT32_MAX) return false;
	const auto offset = static_cast<size_t>(OutputOffset(plan)) +
	                    static_cast<size_t>(inst->WalkOutput()) * sizeof(uint64_t);
	if (offset + sizeof(bits) > plan.srt_scratch.size() * sizeof(uint64_t)) return false;
	std::memcpy(&bits, Scratch(plan) + offset, sizeof(bits));
	result = static_cast<uint32_t>(bits);
	return bits <= UINT32_MAX;
}

bool WalkedDescriptor(const ResourcePlan& plan, uint32_t source, DescriptorValue& result) {
	if (source >= plan.descriptor_sources.size()) return false;
	std::array<uint32_t, 9> words {};
	std::memcpy(words.data(), Scratch(plan) + source * DescriptorStride, DescriptorStride);
	if (words[8] == 0u || plan.descriptor_sources[source].dword_count > result.dwords.size()) return false;
	result = {};
	result.dword_count = plan.descriptor_sources[source].dword_count;
	std::copy_n(words.begin(), result.dword_count, result.dwords.begin());
	return true;
}

bool WalkedActive(const ResourcePlan& plan, uint32_t source) {
	if (source >= plan.descriptor_sources.size()) return false;
	return Scratch(plan)[ActiveOffset(plan) + source] != 0u;
}

bool WalkedImageActive(const ResourcePlan& plan, uint32_t image) {
	if (image >= plan.info.images.size()) return false;
	return Scratch(plan)[ActiveOffset(plan) + plan.descriptor_sources.size() + image] != 0u;
}

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type) {
	return RuntimeValidator(program, type).Run(value);
}

bool ValidateWaveUniformValue(const ResourcePlan& program, Value value) {
	return RuntimeValidator(program, RuntimeValueType::Any, true).Run(value);
}

uint32_t SrtWalkerLoadLength(uint64_t) {
	// The native ARM64 evaluator reads specialization dependencies through the guest-memory
	// callback; it does not emit faultable x86 load instructions.
	return 0;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // __arm64__ || __aarch64__
