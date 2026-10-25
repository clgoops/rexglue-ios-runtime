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

#include "rex/native/rex_resolve.h"
namespace rex::native {
namespace shaders {
#include "resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "resolve_fast_32bpp_4xmsaa_cs.h"
#include "resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "resolve_fast_64bpp_4xmsaa_cs.h"
#include "resolve_full_8bpp_cs.h"
#include "resolve_full_16bpp_cs.h"
#include "resolve_full_32bpp_cs.h"
#include "resolve_full_64bpp_cs.h"
#include "resolve_full_128bpp_cs.h"
}
std::span<const uint32_t> RexResolve::CopyKernel(graphics::draw_util::ResolveCopyShaderIndex index) {
  switch(uint32_t(index)) {
    case 0:return shaders::resolve_fast_32bpp_1x2xmsaa_cs;
    case 1:return shaders::resolve_fast_32bpp_4xmsaa_cs;
    case 2:return shaders::resolve_fast_64bpp_1x2xmsaa_cs;
    case 3:return shaders::resolve_fast_64bpp_4xmsaa_cs;
    case 4:return shaders::resolve_full_8bpp_cs;
    case 5:return shaders::resolve_full_16bpp_cs;
    case 6:return shaders::resolve_full_32bpp_cs;
    case 7:return shaders::resolve_full_64bpp_cs;
    case 8:return shaders::resolve_full_128bpp_cs;
    default:return {};
  }
}
}
