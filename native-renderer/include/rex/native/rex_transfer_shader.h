// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex_targets.h"
#include "spirv.h"
namespace rex::native {
// CPU-only lowering of the SDK's EDRAM ownership transfer shader. No Vulkan
// objects or driver calls are part of the native shader generation path.
class RexTransferShader {
 public:
  enum class Mode:uint8_t {
    ColorToDepth,ColorToColor,DepthToDepth,DepthToColor,
    ColorAndHostDepthToDepth,DepthAndHostDepthToDepth
  };
  struct Request {
    Mode mode=Mode::ColorToColor;
    RexRenderTargets::Key source,destination;
    std::optional<RexRenderTargets::Key> host_depth;
    uint32_t scale_x=1,scale_y=1;
    bool operator==(const Request&) const=default;
  };
  // Storage views preserve the original half/float/SNORM bit patterns when
  // guest memory is reinterpreted using a different render-target format.
  static Format OwnershipFormat(graphics::xenos::ColorRenderTargetFormat);
  static bool Generate(const Request&,Identity,ConvertedShader&,std::string&);
};
} // namespace rex::native
