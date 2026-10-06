#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Resolves native descriptor sources, plans their scalar reads, and assigns dense resource bindings.
// Returns false when the shader uses resource shapes the tracker cannot represent; the caller
// must skip the shader. Tracking failures surface as std::runtime_error (see Tracker::Fail),
// which also lets unit tests assert on them without aborting.
[[nodiscard]] bool TrackResources(Program& program, const Decoder::Program& decoded, const CFG::Graph& native_cfg);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCETRACKING_H_ */
