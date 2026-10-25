// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_target_transfers.h"
#include <algorithm>
#include <bit>
#include <cstring>
namespace rex::native {
namespace {
template<class T> void Add(std::vector<T>& list,const T& value) {
  if(std::none_of(list.begin(),list.end(),[&](const T& i){return i.id==value.id;}))list.push_back(value);
}
Bytes Data(std::span<const uint32_t> words) {
  // MSL constant structures may include trailing alignment; retain an owned,
  // zero-filled 256-byte allocation independent of the next draw's registers.
  auto data=std::make_shared<std::vector<std::byte>>(256);
  std::memcpy(data->data(),words.data(),words.size_bytes());return data;
}
Attachment Attach(SurfaceView view) {Attachment result;result.view=view;return result;}
Draw Fullscreen(Identity pipeline,uint32_t w,uint32_t h) {
  Draw d;d.pipeline=pipeline;d.vertices=3;d.viewport={0,0,double(w),double(h),0,1};d.scissor={0,0,w,h};return d;
}
uint32_t PackAddress(RexRenderTargets::Key dest,RexRenderTargets::Key source) {
  // Same packed ABI as the SDK's TransferAddressConstant. The signed tile
  // delta includes one extra sign bit and is deliberately masked to 12 bits.
  return dest.GetPitchTiles() | (source.GetPitchTiles()<<graphics::xenos::kEdramPitchTilesBits) |
    ((uint32_t(int32_t(dest.base_tiles)-int32_t(source.base_tiles))&0xfffu) <<
      (2*graphics::xenos::kEdramPitchTilesBits));
}
}
RexTargetTransfers::RexTargetTransfers(std::function<Identity()> next,uint32_t sx,uint32_t sy)
    :next_(std::move(next)),scale_x_(sx),scale_y_(sy) {
  fullscreen_.id=next_();fullscreen_.stage=Stage::Vertex;fullscreen_.entry="native_transfer_vs";
  fullscreen_.msl="#include <metal_stdlib>\nusing namespace metal;vertex float4 native_transfer_vs(uint i[[vertex_id]]){"
    "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}";
}
bool RexTargetTransfers::SnapshotDepth(Frame& frame,const Surface& source,SurfaceView& output,std::string& error) {
  const unsigned ms=source.samples>1;
  auto& shader=snapshot_shaders_[ms];
  if(!shader) {
    Shader s;s.id=next_();s.stage=Stage::Fragment;s.entry="native_depth_snapshot";
    s.arguments={{ArgumentKind::Texture,0,1,0,1,TextureKind::TwoD,bool(ms)}};
    s.msl="#include <metal_stdlib>\nusing namespace metal;fragment float native_depth_snapshot(float4 p[[position]],";
    if(ms)s.msl+="uint sample[[sample_id]],texture2d_ms<float> depth[[texture(0)]])[[depth(any)]]{return depth.read(uint2(p.xy),sample).x;}";
    else s.msl+="texture2d<float> depth[[texture(0)]])[[depth(any)]]{return depth.read(uint2(p.xy)).x;}";
    shader=std::move(s);
  }
  const unsigned index=std::countr_zero(source.samples);
  if(index>=snapshot_pipelines_.size()) {error="unsupported depth snapshot sample count";return false;}
  auto& pipeline=snapshot_pipelines_[index];
  if(!pipeline) {
    Pipeline p;p.id=next_();p.vertex=fullscreen_.id;p.fragment=shader->id;p.samples=source.samples;
    p.depth=Format::Depth32Float;p.depth_write=true;p.depth_compare=Compare::Always;pipeline=p;
  }
  Surface snapshot=source;snapshot.id=next_();snapshot.format=Format::Depth32Float;
  snapshot.owner=std::make_shared<const Identity>(snapshot.id);Add(frame.surfaces,snapshot);
  Add(frame.shaders,fullscreen_);Add(frame.shaders,*shader);Add(frame.pipelines,*pipeline);
  Pass pass;pass.width=source.width;pass.height=source.height;pass.samples=source.samples;
  output={snapshot.id};pass.depth=Attach(output);pass.depth->load=Load::Discard;
  auto draw=Fullscreen(pipeline->id,pass.width,pass.height);
  draw.bindings[1].textures.push_back({0,{source.id}});pass.commands.push_back(draw);
  AppendPass(frame,std::move(pass));return true;
}
bool RexTargetTransfers::Encode(Frame& frame,const RexRenderTargets::OwnershipTransfer& t,std::string& error) {
  error.clear();
  if(t.rectangles.empty() || t.source_image.id==t.destination_image.id ||
     bool(t.host_depth_source)!=bool(t.host_depth_image)) {
    error="invalid native EDRAM ownership transfer";return false;
  }
  auto matches=[&](RexRenderTargets::Key k,const Surface& s) {
    return k.pitch_tiles_at_32bpp && uint32_t(k.msaa_samples)<=2 && s.width==k.GetWidth()*scale_x_ &&
      s.samples==(1u<<uint32_t(k.msaa_samples)) && s.format==(k.is_depth?
      Format::Depth32Stencil8:RexRenderTargets::NativeColorFormat(k.GetColorFormat()));
  };
  if(!matches(t.source,t.source_image) || !matches(t.destination,t.destination_image) ||
     (t.host_depth_source && !matches(*t.host_depth_source,*t.host_depth_image))) {
    error="native ownership surface does not match its SDK key";return false;
  }
  for(const Rect& rect:t.rectangles)if(!rect.width || !rect.height || rect.x>t.destination_image.width ||
    rect.width>t.destination_image.width-rect.x || rect.y>t.destination_image.height ||
    rect.height>t.destination_image.height-rect.y) {
    error="SDK ownership rectangle exceeds its native destination";return false;
  }
  RexTransferShader::Request r;r.source=t.source;r.destination=t.destination;
  r.scale_x=scale_x_;r.scale_y=scale_y_;r.host_depth=t.host_depth_source;
  using M=RexTransferShader::Mode;
  if(t.destination.is_depth)r.mode=t.source.is_depth?
    (r.host_depth?M::DepthAndHostDepthToDepth:M::DepthToDepth):
    (r.host_depth?M::ColorAndHostDepthToDepth:M::ColorToDepth);
  else {
    if(r.host_depth) {error="host depth provided for a color ownership destination";return false;}
    r.mode=t.source.is_depth?M::DepthToColor:M::ColorToColor;
  }
  // Shader/pipeline keys omit base and pitch: those are per-draw constants.
  std::array<uint32_t,8> key={uint32_t(r.mode),uint32_t(t.source.resource_format),uint32_t(t.destination.resource_format),
    uint32_t(t.source.msaa_samples),uint32_t(t.destination.msaa_samples),
    r.host_depth?uint32_t(r.host_depth->msaa_samples):0u,scale_x_,scale_y_};
  auto* entry=cache_.Find(key);
  if(!entry) {
    Entry value;if(!RexTransferShader::Generate(r,next_(),value.fragment,error))return false;
    Pipeline& p=value.pipeline;p.id=next_();p.vertex=fullscreen_.id;p.fragment=value.fragment.shader.id;
    p.samples=t.destination_image.samples;
    if(t.destination.is_depth) {
      p.depth=p.stencil=Format::Depth32Stencil8;p.depth_write=true;p.depth_compare=Compare::Always;
      p.stencil_enabled=true;p.front.compare=Compare::Always;p.front.pass=StencilOperation::Replace;p.back=p.front;
    } else p.colors[0]=RexTransferShader::OwnershipFormat(t.destination.GetColorFormat());
    entry=&cache_.Insert(key,std::move(value));cache_.Trim(512);
  }
  Add(frame.surfaces,t.source_image);Add(frame.surfaces,t.destination_image);
  if(t.host_depth_image)Add(frame.surfaces,*t.host_depth_image);
  SurfaceView host_depth;
  if(t.host_depth_image) {
    host_depth={t.host_depth_image->id};
    if(host_depth.surface==t.destination_image.id && !SnapshotDepth(frame,*t.host_depth_image,host_depth,error))return false;
  }
  Add(frame.shaders,fullscreen_);Add(frame.shaders,entry->fragment.shader);Add(frame.pipelines,entry->pipeline);
  Pass pass;pass.width=t.destination_image.width;pass.height=t.destination_image.height;pass.samples=t.destination_image.samples;
  SurfaceView dest{t.destination_image.id};
  if(t.destination.is_depth)pass.depth=pass.stencil=Attach(dest);
  else {dest.format=entry->pipeline.colors[0];pass.colors[0]=Attach(dest);}
  const uint32_t values[]={PackAddress(t.destination,t.source)};
  std::array<uint32_t,2> host_values={r.host_depth?PackAddress(t.destination,*r.host_depth):0,values[0]};
  Buffer constants{next_(),r.host_depth?Data(host_values):Data(values),true};frame.buffers.push_back(constants);
  std::vector<DescriptorResources> resources;
  resources.push_back({ResourceSource::PushConstants,0,0,{{constants.id,0,256}},{},{}});
  unsigned set=0;
  if(r.host_depth)resources.push_back({ResourceSource::Descriptor,set++,0,{}, {host_depth},{}});
  if(t.source.is_depth) {
    resources.push_back({ResourceSource::Descriptor,set,0,{}, {{t.source_image.id}},{}});
    SurfaceView stencil{t.source_image.id};stencil.format=Format::Stencil8View;
    resources.push_back({ResourceSource::Descriptor,set,1,{}, {stencil},{}});
  } else {
    SurfaceView color{t.source_image.id};color.format=RexTransferShader::OwnershipFormat(t.source.GetColorFormat());
    resources.push_back({ResourceSource::Descriptor,set,0,{}, {color},{}});
  }
  Bindings bindings;std::optional<Buffer> auxiliary;
  if(!BindConverted(entry->fragment,resources,next_(),bindings,auxiliary,error))return false;
  if(auxiliary)frame.buffers.push_back(*auxiliary);
  for(const Rect& rect:t.rectangles) {
    auto draw=Fullscreen(entry->pipeline.id,pass.width,pass.height);draw.scissor=rect;draw.bindings[1]=bindings;
    pass.commands.push_back(std::move(draw));
  }
  AppendPass(frame,std::move(pass));return true;
}
} // namespace rex::native
