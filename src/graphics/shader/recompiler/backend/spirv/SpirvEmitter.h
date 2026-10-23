#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/capturedVertexLayout.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <optional>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// Host device capabilities that change the emitted SPIR-V. Built once per device.
struct Profile {
	bool maintenance8         = false; // Dynamic texel offsets on image samples.
	bool native_float_min_max = false; // NMin/NMax give the guest's NaN and signed-zero results.
};

inline constexpr Profile DefaultProfile {};

// Resets and writes backend IDs and descriptor indices; do not emit the same program concurrently.
std::vector<uint32_t> EmitProgram(IR::Program& program, ShaderStageInputInfo input_info,
                                  uint32_t       push_data_start_dword = 0,
                                  const Profile& profile               = DefaultProfile,
                                  bool           emit_debug_info       = false);

// The record of a mesh program's outputs on a host without mesh shaders: the program runs as a
// compute shader (EmitProgram with an emulated mesh program), which stores one record per
// workgroup.
[[nodiscard]] CapturedVertexLayout MeshCaptureLayout(const IR::Program&         program,
                                                     const ShaderMeshInputInfo& mesh);

// The vertex shader that draws what the compute shader stored: one triangle per primitive of
// every record. A primitive that is absent, that the program culled, or that a cull distance
// rejects gets one position for its three corners, so it covers no sample. The pixel shader is
// unchanged.
struct MeshEmulationVertexProgram {
	std::vector<uint32_t> spirv;
	uint32_t              slot_words = 0;
};
[[nodiscard]] MeshEmulationVertexProgram
EmitMeshEmulationVertexProgram(const IR::Program& program, ShaderStageInputInfo input_info);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_ */
