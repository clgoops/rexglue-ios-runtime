// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex_transfer_shader.h"
#include "lru.h"
namespace rex::native {
// Encodes SDK tile ownership changes as native render passes. All surfaces
// involved remain owned by the submitted frame through GPU completion.
class RexTargetTransfers {
 public:
  RexTargetTransfers(std::function<Identity()>,uint32_t scale_x=1,uint32_t scale_y=1);
  bool Encode(Frame&,const RexRenderTargets::OwnershipTransfer&,std::string&);
 private:
  struct Entry {ConvertedShader fragment;Pipeline pipeline;};
  bool SnapshotDepth(Frame&,const Surface&,SurfaceView&,std::string&);
  std::function<Identity()> next_;
  uint32_t scale_x_,scale_y_;
  Shader fullscreen_;
  Lru<std::array<uint32_t,8>,Entry> cache_;
  std::array<std::optional<Shader>,2> snapshot_shaders_;
  std::array<std::optional<Pipeline>,4> snapshot_pipelines_;
  Lru<std::array<uint32_t,3>,Surface> snapshot_surfaces_;
};
} // namespace rex::native
