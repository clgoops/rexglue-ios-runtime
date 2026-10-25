// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex_targets.h"
#include "rex_shared_memory.h"
#include "lru.h"
namespace rex::native {
class RexResolve {
 public:
  explicit RexResolve(std::function<Identity()> next):next_(std::move(next)) {}
  bool Dump(Frame&,RexRenderTargets::Key,const Surface&,const BufferStorage& edram,
      uint32_t dest_pitch,uint32_t first_tile,uint32_t width_tiles,uint32_t height_tiles,std::string&);
  bool Copy(Frame&,graphics::draw_util::ResolveCopyShaderIndex,
      const graphics::draw_util::ResolveCopyShaderConstants&,const BufferStorage& edram,BufferView destination,
      uint32_t groups_x,uint32_t groups_y,std::string&);
  static bool DumpShader(RexRenderTargets::Key,Identity,ConvertedShader&,std::string&);
  static std::span<const uint32_t> CopyKernel(graphics::draw_util::ResolveCopyShaderIndex);
 private:
  bool Encode(Frame&,const ConvertedShader&,std::vector<DescriptorResources>,uint32_t,uint32_t,std::string&);
  std::function<Identity()> next_;
  Lru<uint32_t,ConvertedShader> dumps_,copies_;
};
} // namespace rex::native
