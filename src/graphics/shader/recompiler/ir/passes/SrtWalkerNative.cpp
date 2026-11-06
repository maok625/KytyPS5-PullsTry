#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/abi.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint32_t DescriptorStride = 9u * sizeof(uint32_t);

uint32_t ActiveOffset(const ResourcePlan& plan) {
	return static_cast<uint32_t>(plan.descriptor_sources.size()) * DescriptorStride;
}

uint32_t OutputOffset(const ResourcePlan& plan) {
	const auto active = plan.descriptor_sources.size() + plan.info.images.size();
	return (ActiveOffset(plan) + static_cast<uint32_t>(active) + 7u) & ~7u;
}

bool IsRawRead(const ResourcePlan& plan, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer &&
	    op != ValueOpcode::LoadBufferU32) return false;
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= plan.memory_info.size()) return false;
	const auto& memory = plan.memory_info[index];
	if (op != ValueOpcode::LoadBufferU32) {
		return (op == ValueOpcode::LoadAddressU32 && memory.kind == ResourceKind::ScalarAddress) ||
		       (op == ValueOpcode::ReadConstBuffer && memory.kind == ResourceKind::ScalarBuffer);
	}
	if (inst.NumArgs() != 5u || memory.kind != ResourceKind::Buffer || memory.typed ||
	    memory.formatted || memory.coherent || memory.data_bits != 32u ||
	    memory.data_dwords != 1u || memory.idxen || memory.offen || memory.offset != 0u ||
	    inst.Arg(4).GetType() != Type::U1) return false;
	for (uint32_t arg = 1; arg <= 3; ++arg) {
		if (inst.Arg(arg).Resolve() != Value(0u)) return false;
	}
	return true;
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
		case ValueOpcode::SMax32:
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

} // namespace

class NativeSrtWalker {
public:
	struct Result {
		uint64_t bits  = 0;
		bool     known = false;
	};

	NativeSrtWalker(ResourcePlan& plan, std::span<const Value> values)
	    : m_plan(&plan), m_resources(values.empty()) {
		Build(values);
	}

	bool Matches(const ResourcePlan& plan) const { return m_plan == &plan; }

	bool RunWithRuntime(const SrtRuntime& runtime, std::vector<uint32_t>& flat,
	                    std::vector<std::pair<uint64_t, uint64_t>>* reads) {
		m_runtime = &runtime;
		const bool result = Run(runtime, flat, reads);
		m_runtime = nullptr;
		return result;
	}

	bool Run(const SrtRuntime& runtime, std::vector<uint32_t>& flat,
	         std::vector<std::pair<uint64_t, uint64_t>>* reads) {
		auto& plan = *m_plan;
		const auto scratch_bytes = OutputOffset(plan) +
		    static_cast<uint32_t>(m_outputs.size()) * sizeof(uint64_t);
		plan.srt_scratch.assign((scratch_bytes + sizeof(uint64_t) - 1u) / sizeof(uint64_t), 0u);
		auto* scratch = reinterpret_cast<uint8_t*>(plan.srt_scratch.data());
		flat.assign(plan.srt_reads.size(), 0u);
		m_captured.clear();
		m_cache.clear();
		m_required_user_data = 0u;

		m_reachable.assign(plan.control_flow.size(), 0u);
		if (!m_resources) {
			std::ranges::fill(m_reachable, 1u);
		} else if (!plan.control_flow.empty()) {
			m_reachable[0] = 1u;
			// A newly reached predecessor can change a control Phi. Re-evaluate from a
			// clean memo table until reachability reaches a fixed point, like the x86 walker.
			for (size_t pass = 0; pass <= plan.control_flow.size(); ++pass) {
				bool changed = false;
				for (uint32_t block = 0; block < plan.control_flow.size(); ++block) {
					if (m_reachable[block] == 0u) continue;
					const auto& flow = plan.control_flow[block];
					const auto condition = Eval(flow.condition, 0u);
					if (condition.known && flow.successors.size() >= 2u) {
						const auto target = flow.successors[condition.bits != 0u ? 0u : 1u];
						changed |= MarkReachable(target);
					} else {
						for (const auto target: flow.successors) changed |= MarkReachable(target);
					}
				}
				if (!changed) break;
				m_cache.clear();
				m_captured.clear();
			}
		}

		if (m_resources) {
			const auto read_slot = [&](uint32_t slot) {
				if (slot >= plan.srt_reads.size()) return false;
				const auto value = Eval(plan.srt_reads[slot].value, 0u);
				if (!value.known || plan.srt_reads[slot].flat_offset >= flat.size()) return false;
				flat[plan.srt_reads[slot].flat_offset] = static_cast<uint32_t>(value.bits);
				return true;
			};
			if (plan.control_flow.empty()) {
				for (uint32_t slot = 0; slot < plan.srt_reads.size(); ++slot) {
					if (!read_slot(slot)) return false;
				}
			} else {
				for (uint32_t block = 0; block < plan.control_flow.size(); ++block) {
					if (m_reachable[block] == 0u) continue;
					for (const auto slot: plan.control_flow[block].srt_reads) {
						if (!read_slot(slot)) return false;
					}
				}
			}

			const auto is_active = [&](bool image, uint32_t id) {
				bool listed = false;
				for (uint32_t block = 0; block < plan.control_flow.size(); ++block) {
					const auto& entries = image ? plan.control_flow[block].images
					                            : plan.control_flow[block].sources;
					if (std::ranges::find(entries, id) == entries.end()) continue;
					listed = true;
					if (m_reachable[block] != 0u) return true;
				}
				return !listed;
			};
			for (uint32_t source = 0; source < plan.descriptor_sources.size(); ++source) {
				scratch[ActiveOffset(plan) + source] = is_active(false, source) ? 1u : 0u;
			}
			for (uint32_t image = 0; image < plan.info.images.size(); ++image) {
				scratch[ActiveOffset(plan) + plan.descriptor_sources.size() + image] =
				    is_active(true, image) ? 1u : 0u;
			}

			for (uint32_t source = 0; source < plan.descriptor_sources.size(); ++source) {
				if (m_gates[source].empty() || !GateActive(m_gates[source], scratch)) continue;
				bool known = true;
				const auto& descriptor = plan.descriptor_sources[source];
				for (uint32_t word = 0; word < descriptor.dword_count; ++word) {
					const auto value = Eval(descriptor.dwords[word], 0u);
					if (!value.known) {
						known = false;
						break;
					}
					const auto dword = static_cast<uint32_t>(value.bits);
					std::memcpy(scratch + source * DescriptorStride + word * sizeof(uint32_t),
					            &dword, sizeof(dword));
				}
				if (known) {
					const uint32_t marker = 1u;
					std::memcpy(scratch + source * DescriptorStride + 8u * sizeof(uint32_t),
					            &marker, sizeof(marker));
				}
			}
		}

		for (uint32_t index = 0; index < m_outputs.size(); ++index) {
			const uint64_t unknown = UINT64_MAX;
			auto* output = scratch + OutputOffset(plan) + index * sizeof(uint64_t);
			std::memcpy(output, &unknown, sizeof(unknown));
			if (!GateActive(m_output_gates[index], scratch)) continue;
			const auto value = Eval(m_outputs[index], 0u);
			if (value.known) std::memcpy(output, &value.bits, sizeof(value.bits));
		}

		if (runtime.user_data.size() < m_required_user_data) return false;
		plan.srt_user_data = m_required_user_data;
		if (reads != nullptr) {
			for (const auto address: m_captured) reads->emplace_back(address, sizeof(uint32_t));
		}
		return true;
	}

private:
	struct Memo {
		bool   visiting = false;
		bool   ready    = false;
		Result result;
	};

	bool MarkReachable(uint32_t block) {
		if (block >= m_reachable.size() || m_reachable[block] != 0u) return false;
		m_reachable[block] = 1u;
		return true;
	}

	uint32_t Context(Value mask) {
		mask = mask.Resolve();
		const auto found = std::ranges::find(m_masks, mask);
		if (found != m_masks.end()) return static_cast<uint32_t>(found - m_masks.begin());
		m_masks.push_back(mask);
		return static_cast<uint32_t>(m_masks.size() - 1u);
	}

	bool GateActive(const std::vector<uint32_t>& gates, const uint8_t* scratch) const {
		if (gates.empty()) return false;
		if (std::ranges::find(gates, UINT32_MAX) != gates.end()) return true;
		for (const auto gate: gates) {
			if (gate < m_plan->descriptor_sources.size() + m_plan->info.images.size() &&
			    scratch[ActiveOffset(*m_plan) + gate] != 0u) return true;
		}
		return false;
	}

	void AddOutput(Value value, uint32_t gate) {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr) return;
		auto index = inst->WalkOutput();
		if (index >= m_outputs.size() || m_outputs[index].TryInstruction() != inst) {
			index = static_cast<uint32_t>(m_outputs.size());
			inst->SetWalkOutput(index);
			m_outputs.push_back(value.Resolve());
			m_output_gates.emplace_back();
		}
		m_output_gates[index].push_back(gate);
	}

	void Consume() {
		const auto& sources = m_plan->descriptor_sources;
		const auto consume = [&](uint32_t root) {
			if (root >= sources.size()) return;
			const auto& indirect = sources[root].indirect_descriptor;
			if (!indirect) {
				m_gates[root].push_back(root);
				return;
			}
			const auto add = [&](uint32_t source) {
				if (source < sources.size()) m_gates[source].push_back(root);
			};
			if (indirect->sources.empty()) {
				add(indirect->table_source);
				if (indirect->selector) {
					add(indirect->selector->source);
					if (indirect->selector->rows) add(indirect->selector->rows->source);
				}
			}
			for (const auto source: indirect->sources) add(source);
			AddOutput(indirect->key_count, root);
			AddOutput(indirect->selector_first, root);
			AddOutput(indirect->selector_mask, root);
		};
		for (const auto& buffer: m_plan->info.buffers) consume(buffer.source);
		for (const auto& image: m_plan->info.images) consume(image.source);
		for (const auto& sampler: m_plan->info.samplers) {
			const auto* indirect =
			    sampler.source < sources.size() && sources[sampler.source].indirect_descriptor
			        ? &*sources[sampler.source].indirect_descriptor : nullptr;
			if (indirect == nullptr || indirect->sources.empty()) {
				consume(sampler.source);
			} else if (indirect->sources[0] < sources.size()) {
				m_gates[indirect->sources[0]].push_back(indirect->sources[0]);
			}
		}
		const auto& fill = m_plan->uniform_fill;
		if (fill.fill.words != 0u) {
			AddOutput(fill.pattern_size, UINT32_MAX);
			AddOutput(fill.element_count, UINT32_MAX);
			for (const auto value: fill.values) AddOutput(value, UINT32_MAX);
		}
	}

	void Build(std::span<const Value> values) {
		m_gates.resize(m_plan->descriptor_sources.size());
		if (m_resources) Consume();
		for (const auto value: values) AddOutput(value, UINT32_MAX);
		m_plan->srt_user_data = 0u;
		m_plan->srt_captures = 0u;
	}

	Result Eval(Value value, uint32_t context) {
		value = value.Resolve();
		if (value.IsEmpty()) return {};
		if (value.IsImmediate()) {
			uint64_t bits = 0;
			return ImmediateBits(value, bits) ? Result {bits, true} : Result {};
		}
		const auto mask = m_masks[context];
		if (context != 0u && value == mask) return {1u, true};
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return {};
		const auto op = inst->GetOpcode();

		if (context != 0u && IsRuntimeSelect(op) && inst->NumArgs() == 3u &&
		    inst->Arg(0).Resolve() == mask) {
			return Eval(inst->Arg(1), context);
		}
		switch (op) {
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32:
			case ValueOpcode::ConditionRef: return Eval(inst->Arg(0), context);
			case ValueOpcode::CompositeExtractU32x2: {
				const auto* source = inst->Arg(0).ResolveInstruction();
				const auto  index  = inst->Arg(1).Resolve();
				if (source == nullptr || source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 ||
				    !index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u)
					return {};
				return Eval(source->Arg(index.U32()), context);
			}
			case ValueOpcode::ReadFirstLane:
				return Eval(inst->Arg(0), Context(inst->Arg(1)));
			case ValueOpcode::GetUserData: {
				if (inst->NumArgs() != 1u || inst->Arg(0).GetType() != Type::ScalarReg) return {};
				const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
				if (reg < m_plan->user_data_base ||
				    reg - m_plan->user_data_base >= m_plan->user_data_count) return {};
				const auto index = reg - m_plan->user_data_base;
				m_required_user_data = std::max(m_required_user_data, index + 1u);
				if (index >= m_runtime->user_data.size()) return {};
				return {m_runtime->user_data[index], true};
			}
			case ValueOpcode::GetShaderBase:
				return inst->NumArgs() == 0u ? Result {m_runtime->shader_base, true} : Result {};
			case ValueOpcode::Phi:
				if (!m_plan->control_phi_predecessors.contains(inst)) {
					const auto invariant = ResolveInvariantPhi(*m_plan, value);
					return invariant.IsEmpty() ? Result {} : Eval(invariant, context);
				}
				break;
			case ValueOpcode::ReadConst: {
				const auto slot = inst->NumArgs() == 2u ? inst->Arg(1).Resolve() : Value {};
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_plan->srt_reads.size()) return {};
				return Eval(m_plan->srt_reads[slot.U32()].value, context);
			}
			default: break;
		}

		const auto key = std::pair {inst, context};
		auto [entry, inserted] = m_cache.try_emplace(key);
		if (!inserted) {
			if (entry->second.visiting || !entry->second.ready) return {};
			return entry->second.result;
		}
		entry->second.visiting = true;
		const auto result = EvalInstruction(*inst, context);
		entry->second.visiting = false;
		entry->second.ready = true;
		entry->second.result = result;
		return result;
	}

	Result EvalRead(const Inst& inst, uint32_t context) {
		const auto op = inst.GetOpcode();
		const auto* handle = inst.NumArgs() != 0u ? inst.Arg(0).ResolveInstruction() : nullptr;
		const auto expected_handle = op == ValueOpcode::LoadAddressU32
		    ? ValueOpcode::GetAddressResource : ValueOpcode::GetBufferResource;
		if (!IsRawRead(*m_plan, inst) || handle == nullptr ||
		    handle->GetOpcode() != expected_handle) return {};
		const bool vector = op == ValueOpcode::LoadBufferU32;
		if ((op == ValueOpcode::LoadAddressU32 && handle->NumArgs() != 2u) ||
		    (op == ValueOpcode::ReadConstBuffer && inst.NumArgs() != 2u) ||
		    (op == ValueOpcode::LoadBufferU32 && inst.NumArgs() != 5u)) return {};
		if (vector) {
			const auto guard = Eval(inst.Arg(4), context);
			// Unknown guards cannot authorize a host-side read. A false guard reads zero.
			if (!guard.known) return {};
			if (guard.bits == 0u) return {0u, true};
		}
		const bool buffer = op != ValueOpcode::LoadAddressU32;
		if (buffer && handle->NumArgs() != 4u) return {};
		const auto& memory = m_plan->memory_info[inst.Flags<MemoryFlags>().index];
		const auto immediate_signed =
		    static_cast<int64_t>(static_cast<int32_t>(memory.offset)) & ~int64_t {3};
		// The JIT's unsigned buffer-bounds comparison routes a negative immediate
		// to its zero-read path; it is not an unknown address.
		if (buffer && immediate_signed < 0) return {0u, true};

		const auto low = Eval(handle->Arg(0), context);
		const auto high = Eval(handle->Arg(1), context);
		const auto dynamic = Eval(inst.Arg(1), context);
		if (!low.known || !high.known || !dynamic.known) return {};
		const uint64_t base = ((high.bits & 0xffffu) << 32u) | (low.bits & ~uint64_t {3});
		const uint64_t offset = (dynamic.bits & ~uint64_t {3}) +
		                        static_cast<uint64_t>(immediate_signed);
		const uint64_t address = base + offset;

		if (op == ValueOpcode::LoadAddressU32 && (address >> 48u) != 0u) return {};
		if (buffer) {
			const auto stride = Eval(handle->Arg(2), context);
			const auto flags = Eval(handle->Arg(3), context);
			if (!stride.known || !flags.known) return {};
			const auto descriptor_flags = static_cast<uint32_t>(high.bits) >> 16u;
			if (vector) {
				if ((descriptor_flags & (1u << 15u)) != 0u ||
				    (static_cast<uint32_t>(flags.bits) & ((1u << 23u) | 0xf0000000u)) != 0u)
					return {};
				if ((descriptor_flags & 0x3fffu) == 0u ||
				    (static_cast<uint32_t>(flags.bits) & (0x7fu << 12u)) == 0u ||
				    stride.bits == 0u) return {0u, true};
			}
			uint64_t size = stride.bits;
			const auto records = descriptor_flags & 0x3fffu;
			if (records != 0u) size *= records;
			const uint64_t end = offset + sizeof(uint32_t);
			if (end > size) return {0u, true};
		}

		m_captured.push_back(address);
		uint32_t word = 0u;
		const std::span<uint32_t> destination(&word, 1u);
		if (m_runtime->read_specialization_memory == nullptr ||
		    !m_runtime->read_specialization_memory(m_runtime->userdata, address, destination)) {
			// The x86 walker completes an inaccessible guest load with a zero value.
			word = 0u;
		}
		return {word, true};
	}

	Result EvalInstruction(const Inst& inst, uint32_t context) {
		const auto op = inst.GetOpcode();
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer ||
		    op == ValueOpcode::LoadBufferU32) return EvalRead(inst, context);

		if (op == ValueOpcode::Phi) {
			const auto found = m_plan->control_phi_predecessors.find(&inst);
			if (found == m_plan->control_phi_predecessors.end() ||
			    found->second.size() != inst.NumArgs()) return {};
			std::optional<uint64_t> common;
			for (size_t index = 0; index < found->second.size(); ++index) {
				const auto predecessor = found->second[index];
				if (predecessor >= m_reachable.size() || m_reachable[predecessor] == 0u) continue;
				const auto value = Eval(inst.Arg(index), context);
				if (!value.known) return {};
				if (common && *common != value.bits) return {};
				common = value.bits;
			}
			return common ? Result {*common, true} : Result {};
		}

		if (IsRuntimeSelect(op)) {
			if (inst.NumArgs() != 3u) return {};
			const auto predicate = Eval(inst.Arg(0), context);
			if (!predicate.known) return {};
			return Eval(inst.Arg(predicate.bits != 0u ? 1u : 2u), context);
		}

		if (op == ValueOpcode::ULessThan32 || op == ValueOpcode::ULessThanEqual32) {
			if (inst.NumArgs() != 2u) return {};
			const bool less = op == ValueOpcode::ULessThan32;
			const auto bound = Eval(inst.Arg(less ? 1u : 0u), context);
			if (!bound.known) return {};
			if (static_cast<uint32_t>(bound.bits) == 0u) return {static_cast<uint64_t>(!less), true};
			const auto other = Eval(inst.Arg(less ? 0u : 1u), context);
			if (!other.known) return {};
			const bool result = less ? static_cast<uint32_t>(other.bits) < static_cast<uint32_t>(bound.bits)
			                         : static_cast<uint32_t>(bound.bits) <= static_cast<uint32_t>(other.bits);
			return {static_cast<uint64_t>(result), true};
		}

		if (op == ValueOpcode::LogicalAnd || op == ValueOpcode::LogicalOr) {
			if (inst.NumArgs() != 2u) return {};
			const auto left = Eval(inst.Arg(0), context);
			if (left.known) {
				const bool truth = left.bits != 0u;
				if (op == ValueOpcode::LogicalAnd && !truth) return {0u, true};
				if (op == ValueOpcode::LogicalOr && truth) return {1u, true};
				const auto right = Eval(inst.Arg(1), context);
				return right.known ? Result {static_cast<uint64_t>(right.bits != 0u), true} : Result {};
			}
			const auto right = Eval(inst.Arg(1), context);
			const bool conjunction = op == ValueOpcode::LogicalAnd;
			if (right.known && ((conjunction && right.bits == 0u) ||
			                    (!conjunction && right.bits != 0u))) {
				return {static_cast<uint64_t>(!conjunction), true};
			}
			if (conjunction) {
				const auto refined = Eval(inst.Arg(1), Context(inst.Arg(0)));
				if (refined.known && refined.bits == 0u) return {0u, true};
			}
			return {};
		}

		if (!IsRuntimeUniformOp(op) || op == ValueOpcode::CompositeConstructU32x2) return {};
		std::array<uint64_t, 4> args {};
		if (inst.NumArgs() > args.size()) return {};
		for (size_t index = 0; index < inst.NumArgs(); ++index) {
			const auto value = Eval(inst.Arg(index), context);
			if (!value.known) return {};
			args[index] = value.bits;
		}
		const auto a = args[0];
		const auto b = args[1];
		const auto c = args[2];
		const auto d = args[3];
		const auto u32 = [](uint64_t value) { return static_cast<uint32_t>(value); };
		switch (op) {
			case ValueOpcode::CompositeConstructU64:
				return {(uint64_t {u32(a)} | (uint64_t {u32(b)} << 32u)), true};
			case ValueOpcode::CompositeExtractU64: {
				const auto index = inst.Arg(1).Resolve();
				if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) return {};
				return {static_cast<uint32_t>(a >> (index.U32() * 32u)), true};
			}
			case ValueOpcode::IAdd32: return {u32(a + b), true};
			case ValueOpcode::IAdd64: return {a + b, true};
			case ValueOpcode::ISub32: return {u32(a - b), true};
			case ValueOpcode::ISub64: return {a - b, true};
			case ValueOpcode::IMul32: return {u32(a * b), true};
			case ValueOpcode::UMulHi:
				return {(uint64_t {u32(a)} * uint64_t {u32(b)}) >> 32u, true};
			case ValueOpcode::UMin32: return {std::min(u32(a), u32(b)), true};
			case ValueOpcode::UMax32: return {std::max(u32(a), u32(b)), true};
			case ValueOpcode::SMax32:
				return {static_cast<uint32_t>(std::max(static_cast<int32_t>(u32(a)),
				                                       static_cast<int32_t>(u32(b)))), true};
			case ValueOpcode::ConvertF32U32: {
				const auto converted = static_cast<float>(static_cast<uint32_t>(a));
				return {std::bit_cast<uint32_t>(converted), true};
			}
			case ValueOpcode::ConvertU32F32: {
				const auto value = std::bit_cast<float>(u32(a));
				if (!(value > 0.0f)) return {0u, true};
				if (value >= 4294967296.0f) return {UINT32_MAX, true};
				return {static_cast<uint32_t>(value), true};
			}
			case ValueOpcode::ConvertS32F32: {
				const auto value = std::bit_cast<float>(u32(a));
				if (std::isnan(value)) return {0u, true};
				if (value >= 2147483648.0f) return {static_cast<uint32_t>(INT32_MAX), true};
				if (value < -2147483648.0f) return {static_cast<uint32_t>(INT32_MIN), true};
				return {static_cast<uint32_t>(static_cast<int32_t>(value)), true};
			}
			case ValueOpcode::FPMul32: {
				const volatile float left = std::bit_cast<float>(u32(a));
				const volatile float right = std::bit_cast<float>(u32(b));
				const float result = left * right;
				return {std::bit_cast<uint32_t>(result), true};
			}
			case ValueOpcode::FPTrunc32: {
				const auto value = std::bit_cast<float>(u32(a));
				return {std::bit_cast<uint32_t>(std::trunc(value)), true};
			}
			case ValueOpcode::FPRecipIFlag32: {
				const auto bits = u32(a);
				if ((bits & 0x807fffffu) != 0u) return {};
				const auto exponent = (bits >> 23u) & 0xffu;
				if (exponent == 0u || exponent >= 254u) return {};
				return {(254u - exponent) << 23u, true};
			}
			case ValueOpcode::FPIsNan32: {
				const auto bits = u32(a) & 0x7fffffffu;
				return {static_cast<uint64_t>(bits > 0x7f800000u), true};
			}
			case ValueOpcode::FPOrdLessThanEqual32:
			case ValueOpcode::FPOrdGreaterThanEqual32: {
				auto left_bits = u32(a);
				auto right_bits = u32(b);
				if (inst.Flags<FPCompareFlags>().flush_input_denorms) {
					if ((left_bits & 0x7fffffffu) < 0x00800000u) left_bits &= 0x80000000u;
					if ((right_bits & 0x7fffffffu) < 0x00800000u) right_bits &= 0x80000000u;
				}
				const auto left = std::bit_cast<float>(left_bits);
				const auto right = std::bit_cast<float>(right_bits);
				if (std::isnan(left) || std::isnan(right)) return {0u, true};
				const bool result = op == ValueOpcode::FPOrdLessThanEqual32 ? left <= right
				                                                           : left >= right;
				return {static_cast<uint64_t>(result), true};
			}
			case ValueOpcode::BitwiseAnd32: return {u32(a) & u32(b), true};
			case ValueOpcode::BitwiseAnd64: return {a & b, true};
			case ValueOpcode::BitwiseOr32: return {u32(a) | u32(b), true};
			case ValueOpcode::BitwiseXor32: return {u32(a) ^ u32(b), true};
			case ValueOpcode::BitwiseNot32: return {~u32(a), true};
			case ValueOpcode::ShiftLeftLogical32: return {u32(u32(a) << (u32(b) & 31u)), true};
			case ValueOpcode::ShiftLeftLogical64: return {a << (b & 63u), true};
			case ValueOpcode::ShiftRightLogical32: return {u32(a) >> (u32(b) & 31u), true};
			case ValueOpcode::ShiftRightLogical64: return {a >> (b & 63u), true};
			case ValueOpcode::ShiftRightArithmetic32:
				return {static_cast<uint32_t>(static_cast<int32_t>(u32(a)) >> (u32(b) & 31u)), true};
			case ValueOpcode::ShiftRightArithmetic64:
				return {static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 63u)), true};
			case ValueOpcode::BitFieldUExtract:
			case ValueOpcode::BitFieldSExtract: {
				const auto offset = u32(b);
				const auto width = u32(c);
				if (offset > 32u || width > 32u - offset) return {};
				if (width == 0u) return {0u, true};
				const uint32_t mask = width == 32u ? UINT32_MAX : (1u << width) - 1u;
				const uint32_t field = (u32(a) >> offset) & mask;
				if (op == ValueOpcode::BitFieldUExtract) return {field, true};
				const auto shift = 32u - width;
				return {static_cast<uint32_t>(static_cast<int32_t>(field << shift) >> shift), true};
			}
			case ValueOpcode::BitFieldInsert: {
				const auto offset = u32(c);
				const auto width = u32(d);
				if (offset > 32u || width > 32u - offset) return {};
				if (width == 0u) return {u32(a), true};
				const uint32_t field = width == 32u ? UINT32_MAX : (1u << width) - 1u;
				const uint32_t mask = field << offset;
				return {(u32(a) & ~mask) | ((u32(b) << offset) & mask), true};
			}
			case ValueOpcode::ULessThan32: return {static_cast<uint64_t>(u32(a) < u32(b)), true};
			case ValueOpcode::ULessThanEqual32: return {static_cast<uint64_t>(u32(a) <= u32(b)), true};
			case ValueOpcode::IEqual32: return {static_cast<uint64_t>(u32(a) == u32(b)), true};
			case ValueOpcode::IEqual64: return {static_cast<uint64_t>(a == b), true};
			case ValueOpcode::INotEqual32: return {static_cast<uint64_t>(u32(a) != u32(b)), true};
			case ValueOpcode::INotEqual64: return {static_cast<uint64_t>(a != b), true};
			case ValueOpcode::UGreaterThan32: return {static_cast<uint64_t>(u32(a) > u32(b)), true};
			case ValueOpcode::SGreaterThanEqual32:
				return {static_cast<uint64_t>(static_cast<int32_t>(u32(a)) >=
				                              static_cast<int32_t>(u32(b))), true};
			case ValueOpcode::LogicalXor:
				return {static_cast<uint64_t>((a != 0u) != (b != 0u)), true};
			case ValueOpcode::LogicalNot: return {static_cast<uint64_t>(a == 0u), true};
			default: return {};
		}
	}

	ResourcePlan* m_plan = nullptr;
	const SrtRuntime* m_runtime = nullptr;
	bool m_resources = true;
	std::vector<Value> m_outputs;
	std::vector<std::vector<uint32_t>> m_output_gates;
	std::vector<std::vector<uint32_t>> m_gates;
	std::vector<Value> m_masks {Value {}};
	std::vector<uint8_t> m_reachable;
	std::map<std::pair<const Inst*, uint32_t>, Memo> m_cache;
	std::vector<uint64_t> m_captured;
	uint32_t m_required_user_data = 0u;
};

void CompileSrtWalkerNative(ResourcePlan& plan, std::span<const Value> values) {
	plan.srt_native_values.assign(values.begin(), values.end());
	plan.srt_native = std::make_shared<NativeSrtWalker>(plan, plan.srt_native_values);
}

bool WalkSrtNative(const ResourcePlan& plan, const SrtRuntime& runtime, std::vector<uint32_t>& flat,
                   std::vector<std::pair<uint64_t, uint64_t>>* reads) {
	auto& mutable_plan = const_cast<ResourcePlan&>(plan);
	if (!mutable_plan.srt_native || !mutable_plan.srt_native->Matches(plan)) {
		mutable_plan.srt_native =
		    std::make_shared<NativeSrtWalker>(mutable_plan, mutable_plan.srt_native_values);
	}
	auto walker = mutable_plan.srt_native;
	// The runtime is supplied per dispatch; don't retain it in the cached program.
	// Run has a short-lived pointer only while evaluating this invocation.
	return walker->RunWithRuntime(runtime, flat, reads);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
