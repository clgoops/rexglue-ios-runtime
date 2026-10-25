/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "rex/native/rex_textures.h"
namespace rex::native {
namespace shaders {
#include "texture_load_8bpb_cs.h"
#include "texture_load_16bpb_cs.h"
#include "texture_load_32bpb_cs.h"
#include "texture_load_64bpb_cs.h"
#include "texture_load_128bpb_cs.h"
#include "texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "texture_load_r5g6b5_b5g6r5_cs.h"
#include "texture_load_r5g6b5_rgba8_cs.h"
#include "texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "texture_load_r4g4b4a4_a4r4g4b4_cs.h"
#include "texture_load_r4g4b4a4_rgba8_cs.h"
#include "texture_load_gbgr8_rgb8_cs.h"
#include "texture_load_bgrg8_rgb8_cs.h"
#include "texture_load_r10g11b11_rgba16_cs.h"
#include "texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "texture_load_r11g11b10_rgba16_cs.h"
#include "texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "texture_load_r16_unorm_float_cs.h"
#include "texture_load_r16_snorm_float_cs.h"
#include "texture_load_rg16_unorm_float_cs.h"
#include "texture_load_rg16_snorm_float_cs.h"
#include "texture_load_rgba16_unorm_float_cs.h"
#include "texture_load_rgba16_snorm_float_cs.h"
#include "texture_load_dxt1_rgba8_cs.h"
#include "texture_load_dxt3_rgba8_cs.h"
#include "texture_load_dxt5_rgba8_cs.h"
#include "texture_load_dxn_rg8_cs.h"
#include "texture_load_dxt3a_cs.h"
#include "texture_load_dxt3aas1111_argb4_cs.h"
#include "texture_load_dxt5a_r8_cs.h"
#include "texture_load_ctx1_cs.h"
#include "texture_load_depth_unorm_cs.h"
#include "texture_load_depth_float_cs.h"
}
std::span<const uint32_t> RexTextures::LoadKernel(LoadIndex index) {
  switch(index) {
    case kLoadShaderIndex8bpb:return shaders::texture_load_8bpb_cs;
    case kLoadShaderIndex16bpb:return shaders::texture_load_16bpb_cs;
    case kLoadShaderIndex32bpb:return shaders::texture_load_32bpb_cs;
    case kLoadShaderIndex64bpb:return shaders::texture_load_64bpb_cs;
    case kLoadShaderIndex128bpb:return shaders::texture_load_128bpb_cs;
    case kLoadShaderIndexR5G5B5A1ToB5G5R5A1:return shaders::texture_load_r5g5b5a1_b5g5r5a1_cs;
    case kLoadShaderIndexR5G6B5ToB5G6R5:return shaders::texture_load_r5g6b5_b5g6r5_cs;
    case kLoadShaderIndexR5G6B5ToRGBA8:return shaders::texture_load_r5g6b5_rgba8_cs;
    case kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle:return shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs;
    case kLoadShaderIndexRGBA4ToARGB4:return shaders::texture_load_r4g4b4a4_a4r4g4b4_cs;
    case kLoadShaderIndexRGBA4ToRGBA8:return shaders::texture_load_r4g4b4a4_rgba8_cs;
    case kLoadShaderIndexGBGR8ToRGB8:return shaders::texture_load_gbgr8_rgb8_cs;
    case kLoadShaderIndexBGRG8ToRGB8:return shaders::texture_load_bgrg8_rgb8_cs;
    case kLoadShaderIndexR10G11B11ToRGBA16:return shaders::texture_load_r10g11b11_rgba16_cs;
    case kLoadShaderIndexR10G11B11ToRGBA16SNorm:return shaders::texture_load_r10g11b11_rgba16_snorm_cs;
    case kLoadShaderIndexR11G11B10ToRGBA16:return shaders::texture_load_r11g11b10_rgba16_cs;
    case kLoadShaderIndexR11G11B10ToRGBA16SNorm:return shaders::texture_load_r11g11b10_rgba16_snorm_cs;
    case kLoadShaderIndexR16UNormToFloat:return shaders::texture_load_r16_unorm_float_cs;
    case kLoadShaderIndexR16SNormToFloat:return shaders::texture_load_r16_snorm_float_cs;
    case kLoadShaderIndexRG16UNormToFloat:return shaders::texture_load_rg16_unorm_float_cs;
    case kLoadShaderIndexRG16SNormToFloat:return shaders::texture_load_rg16_snorm_float_cs;
    case kLoadShaderIndexRGBA16UNormToFloat:return shaders::texture_load_rgba16_unorm_float_cs;
    case kLoadShaderIndexRGBA16SNormToFloat:return shaders::texture_load_rgba16_snorm_float_cs;
    case kLoadShaderIndexDXT1ToRGBA8:return shaders::texture_load_dxt1_rgba8_cs;
    case kLoadShaderIndexDXT3ToRGBA8:return shaders::texture_load_dxt3_rgba8_cs;
    case kLoadShaderIndexDXT5ToRGBA8:return shaders::texture_load_dxt5_rgba8_cs;
    case kLoadShaderIndexDXNToRG8:return shaders::texture_load_dxn_rg8_cs;
    case kLoadShaderIndexDXT3A:return shaders::texture_load_dxt3a_cs;
    case kLoadShaderIndexDXT3AAs1111ToARGB4:return shaders::texture_load_dxt3aas1111_argb4_cs;
    case kLoadShaderIndexDXT5AToR8:return shaders::texture_load_dxt5a_r8_cs;
    case kLoadShaderIndexCTX1:return shaders::texture_load_ctx1_cs;
    case kLoadShaderIndexDepthUnorm:return shaders::texture_load_depth_unorm_cs;
    case kLoadShaderIndexDepthFloat:return shaders::texture_load_depth_float_cs;
    default:return {};
  }
}
}
