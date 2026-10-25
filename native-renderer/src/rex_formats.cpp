// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_targets.h"
namespace rex::native {
namespace gfx=rex::graphics;
Format RexRenderTargets::NativeColorFormat(gfx::xenos::ColorRenderTargetFormat f) {
  using F=gfx::xenos::ColorRenderTargetFormat;
  // Preserve the working SDK storage choices. 7e3 targets use float16; gamma
  // targets use the shader's explicit Xenos PWL conversion, not host sRGB.
  switch(f) {
    case F::k_8_8_8_8:case F::k_8_8_8_8_GAMMA:
    case F::k_2_10_10_10:case F::k_2_10_10_10_AS_10_10_10_10:return Format::RGBA8;
    case F::k_2_10_10_10_FLOAT:case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:return Format::RGBA16Float;
    case F::k_16_16:return Format::RG16Snorm;
    case F::k_16_16_16_16:return Format::RGBA16Snorm;
    case F::k_16_16_FLOAT:return Format::RG16Float;
    case F::k_32_FLOAT:return Format::R32Float;
    case F::k_32_32_FLOAT:return Format::RG32Float;
    default:return Format::Invalid;
  }
}
} // namespace rex::native
