#pragma once

#include "graphics/shader/recompiler/ir/Block.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

<<<<<<< ours
void ConstantPropagationPass(const BlockList& blocks, uint32_t wave_size = 64);
=======
void ConstantPropagationPass(const BlockList& blocks);
struct Program;
uint32_t SimplifyBoundedLoopRegisters(Program& program);
uint32_t SimplifyLocalAddressStores(Program& program);
>>>>>>> theirs

} // namespace Libs::Graphics::ShaderRecompiler::IR
