// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex_constants.h"
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/util/draw.h>
namespace rex::native {
struct RexDrawProfile {
  uint32_t scale_x=1,scale_y=1;
  bool edram_interlock=false,gamma_unorm16=false;
  bool fixed_rg16_truncated=true,fixed_rgba16_truncated=true;
  bool depth_float24_convert=true,depth_float24_round=false;
  bool clip_distance=true,cull_distance=false;
  bool image_view_swizzle=false;
};
struct RexTextureState {
  std::array<uint8_t,32> swizzled_signs{};
  std::array<uint32_t,32> host_swizzles{};
  uint32_t resolution_scaled=0;
};
// CPU semantics extracted from the working SDK command processor. All guest
// register interpretation is retained; native resources are bound separately.
bool UpdateRexSystemConstants(const graphics::RegisterFile&,const RexDrawProfile&,
    const RexTextureState&,RexConstantState::System&,bool primitive_polygonal,
    const graphics::PrimitiveProcessor::ProcessingResult&,bool shader_32bit_index_dma,
    uint32_t compute_memexport_vertex_count,const graphics::draw_util::ViewportInfo&,
    uint32_t used_texture_mask,graphics::reg::RB_DEPTHCONTROL,uint32_t normalized_color_mask);
graphics::SpirvShaderTranslator::Modification RexVertexModification(
    const graphics::RegisterFile&,const RexDrawProfile&,graphics::SpirvShaderTranslator&,
    const graphics::Shader&,graphics::Shader::HostVertexShaderType,uint32_t interpolator_mask,bool ps_param_gen);
graphics::SpirvShaderTranslator::Modification RexPixelModification(
    const graphics::RegisterFile&,const RexDrawProfile&,graphics::SpirvShaderTranslator&,
    const graphics::Shader&,uint32_t interpolator_mask,uint32_t param_gen_pos,graphics::reg::RB_DEPTHCONTROL);
} // namespace rex::native
