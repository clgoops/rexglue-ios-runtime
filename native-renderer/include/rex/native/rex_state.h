// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex_textures.h"
#include <rex/graphics/pipeline/shader/spirv.h>
namespace rex::native {
Sampler RexSamplerState(const graphics::RegisterFile&,const graphics::SpirvShader::SamplerBinding&);
bool RexPipelineState(const graphics::RegisterFile&,graphics::reg::RB_DEPTHCONTROL,uint32_t color_mask,
    bool polygonal,const graphics::draw_util::ViewportInfo&,const Pass&,const Frame&,
    Identity vertex,Identity fragment,Pipeline&,Draw&,std::string&);
} // namespace rex::native
