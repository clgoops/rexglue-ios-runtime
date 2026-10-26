// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex_draw_state.h"
#include <rex/graphics/pipeline/render_target/cache.h>
namespace rex::graphics {class TextureCache;}
namespace rex::native {
class RexTargetTransfers;
class RexResolve;
class RexSharedMemory;
// Retains the SDK's EDRAM ownership tracker. Native images are allocated from
// its exact keys and dimensions; a plain framebuffer address is not an alias.
class RexRenderTargets final:public graphics::RenderTargetCache {
 public:
  using Key=RenderTargetKey;
  struct NativeTarget final:RenderTarget {
    NativeTarget(Key key,Surface image):RenderTarget(key),image(std::move(image)) {}
    Surface image;
  };
  struct OwnershipTransfer {
    Key source,destination;
    Surface source_image,destination_image;
    std::optional<Key> host_depth_source;
    std::optional<Surface> host_depth_image;
    uint32_t first_tile=0,end_tile=0;
    std::vector<Rect> rectangles; // Scaled native pixels, exact SDK tile ranges.
  };
  // The encoder must preserve the complete transfer, including format
  // reinterpretation and retained host-depth precision. Failure faults this
  // cache; ownership cannot be silently advanced past a missing GPU copy.
  using TransferEncoder=std::function<bool(Frame&,const OwnershipTransfer&,std::string&)>;
  RexRenderTargets(const graphics::RegisterFile&,const memory::Memory&,
                   std::function<Identity()> next_id,uint32_t scale_x=1,uint32_t scale_y=1);
  ~RexRenderTargets() override;
  void Initialize(TransferEncoder={});
  void ClearCache() override;
  Path GetPath() const override {return Path::kHostRenderTargets;}
  bool PrepareDraw(Frame&,bool rasterization,graphics::reg::RB_DEPTHCONTROL,uint32_t color_mask,
                   const graphics::Shader& vertex,Pass&,std::string& error);
  RexDrawProfile Profile() const;
  static Format NativeColorFormat(graphics::xenos::ColorRenderTargetFormat);
  bool faulted() const {return faulted_;}
  bool Resolve(Frame&,const memory::Memory&,RexSharedMemory&,graphics::TextureCache&,
      uint32_t& written_address,uint32_t& written_length,std::string&);
 protected:
  bool IsGammaFormatHostStorageSeparate() const override {return false;}
  uint32_t GetMaxRenderTargetWidth() const override {return 16384;}
  uint32_t GetMaxRenderTargetHeight() const override {return 16384;}
  RenderTarget* CreateRenderTarget(Key) override;
  bool IsHostDepthEncodingDifferent(graphics::xenos::DepthRenderTargetFormat) const override {return true;}
 private:
  bool Declare(Frame&,const Surface&,std::string& error);
  bool EncodeTransfer(Frame&,const NativeTarget&,const Transfer&,std::string& error);
  std::function<Identity()> next_id_;
  TransferEncoder encode_;
  std::unique_ptr<RexTargetTransfers> transfers_;
  std::unique_ptr<RexResolve> resolves_;
  std::optional<BufferStorage> edram_;
  bool initialized_=false,faulted_=false;
};
} // namespace rex::native
