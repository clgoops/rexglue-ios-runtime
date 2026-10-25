// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_targets.h"
#include "rex/native/rex_target_transfers.h"
#include "rex/native/rex_resolve.h"
#include <algorithm>
namespace rex::native {
namespace gfx=rex::graphics;
RexRenderTargets::RexRenderTargets(const gfx::RegisterFile& regs,const memory::Memory& memory,
    std::function<Identity()> next,uint32_t sx,uint32_t sy):gfx::RenderTargetCache(regs,memory,sx,sy),next_id_(std::move(next)) {}
RexRenderTargets::~RexRenderTargets() {ShutdownCommon();}
void RexRenderTargets::Initialize(TransferEncoder encoder) {
  if(initialized_)return;
  if(encoder)encode_=std::move(encoder);
  else {
    transfers_=std::make_unique<RexTargetTransfers>(next_id_,draw_resolution_scale_x(),draw_resolution_scale_y());
    encode_=[this](Frame& f,const OwnershipTransfer& t,std::string& e){return transfers_->Encode(f,t,e);};
  }
  InitializeCommon();initialized_=true;
  resolves_=std::make_unique<RexResolve>(next_id_);
  BufferStorage edram{next_id_(),gfx::xenos::kEdramSizeBytes,true,{}};
  edram.owner=std::make_shared<const Identity>(edram.id);edram_=std::move(edram);
}
gfx::RenderTargetCache::RenderTarget* RexRenderTargets::CreateRenderTarget(Key key) {
  Surface image;image.id=next_id_();if(!image.id)return nullptr;
  image.width=key.GetWidth()*draw_resolution_scale_x();
  image.height=GetRenderTargetHeight(key.pitch_tiles_at_32bpp,key.msaa_samples)*draw_resolution_scale_y();
  image.samples=1u<<uint32_t(key.msaa_samples);image.renderable=true;image.sampled=true;
  image.format=key.is_depth?Format::Depth32Stencil8:NativeColorFormat(key.GetColorFormat());
  if(!image.width || !image.height || image.format==Format::Invalid)return nullptr;
  image.owner=std::make_shared<const Identity>(image.id);return new NativeTarget(key,std::move(image));
}
RexDrawProfile RexRenderTargets::Profile() const {
  RexDrawProfile profile;profile.scale_x=draw_resolution_scale_x();profile.scale_y=draw_resolution_scale_y();
  profile.fixed_rg16_truncated=profile.fixed_rgba16_truncated=false;return profile;
}
bool RexRenderTargets::Declare(Frame& frame,const Surface& image,std::string& error) {
  auto existing=std::find_if(frame.surfaces.begin(),frame.surfaces.end(),[&](const Surface& s){return s.id==image.id;});
  if(existing!=frame.surfaces.end()) {
    if(*existing!=image) {error="native target identity changed within a frame";return false;}
    return true;
  }
  frame.surfaces.push_back(image);return true;
}
bool RexRenderTargets::EncodeTransfer(Frame& frame,const NativeTarget& dest,const Transfer& transfer,std::string& error) {
  auto& source=*static_cast<const NativeTarget*>(transfer.source);
  if(!Declare(frame,source.image,error) || !Declare(frame,dest.image,error))return false;
  OwnershipTransfer request{source.key(),dest.key(),source.image,dest.image,{},{},transfer.start_tiles,transfer.end_tiles};
  if(transfer.host_depth_source) {
    auto& host=*static_cast<const NativeTarget*>(transfer.host_depth_source);
    request.host_depth_source=host.key();request.host_depth_image=host.image;
    if(!Declare(frame,host.image,error))return false;
  }
  Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithoutCutout];
  auto key=dest.key();
  const uint32_t count=transfer.GetRectangles(key.base_tiles,key.GetPitchTiles(),key.msaa_samples,
    !key.is_depth&&gfx::xenos::IsColorRenderTargetFormat64bpp(key.GetColorFormat()),rectangles);
  for(uint32_t i=0;i<count;++i)request.rectangles.push_back({rectangles[i].x_pixels*draw_resolution_scale_x(),
    rectangles[i].y_pixels*draw_resolution_scale_y(),rectangles[i].width_pixels*draw_resolution_scale_x(),
    rectangles[i].height_pixels*draw_resolution_scale_y()});
  if(!encode_) {error="native EDRAM ownership transfer encoder is not connected";return false;}
  return encode_(frame,request,error);
}
bool RexRenderTargets::PrepareDraw(Frame& frame,bool rasterization,gfx::reg::RB_DEPTHCONTROL depth,
    uint32_t color_mask,const gfx::Shader& vertex,Pass& pass,std::string& error) {
  error.clear();pass={};
  if(!initialized_ || faulted_) {error="native target cache is unavailable";return false;}
  if(!gfx::RenderTargetCache::Update(rasterization,depth,color_mask,vertex)) {
    error="SDK render-target ownership update failed";faulted_=true;return false;
  }
  auto targets=last_update_accumulated_render_targets();auto transfers=last_update_transfers();
  for(unsigned i=0;i<5;++i)if(targets[i]) {
    auto& target=*static_cast<const NativeTarget*>(targets[i]);
    if(!Declare(frame,target.image,error)) {faulted_=true;return false;}
    for(const auto& transfer:transfers[i])if(!EncodeTransfer(frame,target,transfer,error)) {faulted_=true;return false;}
  }
  auto surface=register_file().Get<gfx::reg::RB_SURFACE_INFO>();
  pass.samples=1u<<uint32_t(surface.msaa_samples);
  uint32_t pitch_tiles=((surface.surface_pitch<<uint32_t(surface.msaa_samples>=gfx::xenos::MsaaSamples::k4X))+79)/80;
  pass.width=Key::GetWidth(pitch_tiles,surface.msaa_samples)*draw_resolution_scale_x();
  pass.height=GetRenderTargetHeight(pitch_tiles,surface.msaa_samples)*draw_resolution_scale_y();
  for(unsigned i=0;i<5;++i)if(targets[i]) {
    const auto& image=static_cast<const NativeTarget*>(targets[i])->image;
    Attachment attachment;attachment.view.surface=image.id;attachment.load=Load::Load;attachment.store=Store::Store;
    if(i)pass.colors[i-1]=attachment;else {pass.depth=attachment;pass.stencil=attachment;}
  }
  return true;
}
} // namespace rex::native
