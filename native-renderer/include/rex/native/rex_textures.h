// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex_shared_memory.h"
#include "rex_draw_state.h"
#include "lru.h"
#include <rex/graphics/pipeline/texture/cache.h>
namespace rex::native {
class RexTextures final:public graphics::TextureCache {
 public:
  using Key=TextureKey;
  using LoadIndex=LoadShaderIndex;
  using LoadParams=LoadConstants;
  struct HostFormat {LoadIndex load_shader=kLoadShaderIndexUnknown;Format format=Format::Invalid;bool block_compressed=false,linear_filterable=true;};
  struct HostFormatPair {HostFormat format_unsigned,format_signed;uint32_t swizzle=0;bool unsigned_signed_compatible=false;};
  struct NativeTexture final:Texture {
    NativeTexture(RexTextures& c,Key k,Surface s):Texture(c,k),image(std::move(s)) {
      const auto f=Describe(image.format);uint64_t bytes=0;
      for(uint32_t level=0;level<image.levels;++level)bytes+=uint64_t(std::max(1u,image.width>>level))*
        std::max(1u,image.height>>level)*std::max(1u,image.depth>>level)*image.layers*f.bytes;
      SetHostMemoryUsage(bytes);
    }
    Surface image;
    std::optional<Surface> flat;
    uint64_t loaded=0,flat_loaded=UINT64_MAX;
  };
  RexTextures(const graphics::RegisterFile&,RexSharedMemory&,std::function<Identity()>);
  ~RexTextures() override;
  void BeginNativeFrame(Frame&,uint64_t submission,uint64_t completed);
  void EndNativeFrame();
  bool Prepare(uint32_t used_texture_mask,RexTextureState&,std::string&);
  bool Active(uint32_t fetch,graphics::xenos::FetchOpDimension,bool is_signed,SurfaceView&,std::string&);
  bool Swap(SurfaceView&,uint32_t& swizzle,std::string&);
  bool faulted() const {return !failure_.empty();}
  static HostFormatPair Formats(graphics::xenos::TextureFormat);
  static std::span<const uint32_t> LoadKernel(LoadIndex);
 protected:
  bool IsSignedVersionSeparateForFormat(Key) const override;
  uint32_t GetHostFormatSwizzle(Key) const override;
  uint32_t GetMaxHostTextureWidthHeight(graphics::xenos::DataDimension d) const override {return d==graphics::xenos::DataDimension::k3D?2048:16384;}
  uint32_t GetMaxHostTextureDepthOrArraySize(graphics::xenos::DataDimension d) const override {return d==graphics::xenos::DataDimension::k3D?2048:2048;}
  std::unique_ptr<Texture> CreateTexture(Key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture&,bool,bool) override;
 private:
  bool Null(graphics::xenos::FetchOpDimension,SurfaceView&,std::string&);
  bool Load(NativeTexture&,bool,bool);
  const ConvertedShader* Kernel(LoadIndex);
  bool Declare(const Surface&);
  RexSharedMemory& shared_;
  std::function<Identity()> next_;
  Frame* frame_=nullptr;
  std::string failure_;
  Lru<uint32_t,ConvertedShader> kernels_;
  std::array<std::optional<Surface>,3> nulls_;
  static const HostFormatPair best_formats[64];
};
} // namespace rex::native
