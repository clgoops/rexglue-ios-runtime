// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_state.h"
namespace rex::native {
namespace gfx=graphics;
namespace {
BlendFactor Factor(gfx::xenos::BlendFactor f) {
  using F=gfx::xenos::BlendFactor;
  switch(f) {
    case F::kOne:return BlendFactor::One;
    case F::kSrcColor:return BlendFactor::SourceColor;case F::kOneMinusSrcColor:return BlendFactor::OneMinusSourceColor;
    case F::kSrcAlpha:return BlendFactor::SourceAlpha;case F::kOneMinusSrcAlpha:return BlendFactor::OneMinusSourceAlpha;
    case F::kDstColor:return BlendFactor::DestinationColor;case F::kOneMinusDstColor:return BlendFactor::OneMinusDestinationColor;
    case F::kDstAlpha:return BlendFactor::DestinationAlpha;case F::kOneMinusDstAlpha:return BlendFactor::OneMinusDestinationAlpha;
    case F::kConstantColor:return BlendFactor::ConstantColor;case F::kOneMinusConstantColor:return BlendFactor::OneMinusConstantColor;
    case F::kConstantAlpha:return BlendFactor::ConstantAlpha;case F::kOneMinusConstantAlpha:return BlendFactor::OneMinusConstantAlpha;
    case F::kSrcAlphaSaturate:return BlendFactor::SourceAlphaSaturated;default:return BlendFactor::Zero;
  }
}
BlendOperation Operation(gfx::xenos::BlendOp op) {
  switch(op) {
    case gfx::xenos::BlendOp::kSubtract:return BlendOperation::Subtract;
    case gfx::xenos::BlendOp::kRevSubtract:return BlendOperation::ReverseSubtract;
    case gfx::xenos::BlendOp::kMin:return BlendOperation::Min;
    case gfx::xenos::BlendOp::kMax:return BlendOperation::Max;default:return BlendOperation::Add;
  }
}
Address Clamp(gfx::xenos::ClampMode c) {
  using C=gfx::xenos::ClampMode;
  switch(c) {
    case C::kRepeat:return Address::Repeat;case C::kMirroredRepeat:return Address::Mirror;
    case C::kMirrorClampToEdge:case C::kMirrorClampToHalfway:case C::kMirrorClampToBorder:return Address::MirrorClamp;
    case C::kClampToBorder:return Address::ClampBorder;default:return Address::Clamp;
  }
}
}
Sampler RexSamplerState(const gfx::RegisterFile& regs,const gfx::SpirvShader::SamplerBinding& binding) {
  auto fetch=regs.GetTextureFetch(binding.fetch_constant);Sampler sampler;
  gfx::xenos::ClampMode u,v,w;gfx::texture_util::GetClampModesForDimension(fetch,u,v,w);
  sampler.u=Clamp(u);sampler.v=Clamp(v);sampler.w=Clamp(w);
  sampler.border=fetch.border_color==gfx::xenos::BorderColor::k_ABGR_White?Border::OpaqueWhite:Border::OpaqueBlack;
  using F=gfx::xenos::TextureFilter;
  auto mag=binding.mag_filter==F::kUseFetchConst?fetch.mag_filter:binding.mag_filter;
  auto min=binding.min_filter==F::kUseFetchConst?fetch.min_filter:binding.min_filter;
  auto mip=binding.mip_filter==F::kUseFetchConst?fetch.mip_filter:binding.mip_filter;
  sampler.mag=mag==F::kLinear?Filter::Linear:Filter::Nearest;sampler.min=min==F::kLinear?Filter::Linear:Filter::Nearest;
  sampler.mip=mip==F::kLinear?Filter::Linear:Filter::Nearest;
  auto aniso=binding.aniso_filter==gfx::xenos::AnisoFilter::kUseFetchConst?fetch.aniso_filter:binding.aniso_filter;
  if(aniso!=gfx::xenos::AnisoFilter::kDisabled) {
    sampler.anisotropy=1u<<std::min(4u,uint32_t(aniso)-uint32_t(gfx::xenos::AnisoFilter::kMax_1_1));
    sampler.mag=sampler.min=sampler.mip=Filter::Linear;
  }
  uint32_t first,last;gfx::texture_util::GetSubresourcesFromFetchConstant(fetch,nullptr,nullptr,nullptr,nullptr,nullptr,&first,&last);
  sampler.min_lod=float(first);sampler.max_lod=mip==F::kBaseMap?float(first)+.25f:1000;
  return sampler;
}
bool RexPipelineState(const gfx::RegisterFile& regs,gfx::reg::RB_DEPTHCONTROL depth,uint32_t mask,bool polygonal,
    const gfx::draw_util::ViewportInfo& viewport,const Pass& pass,const Frame& frame,Identity vertex,Identity fragment,
    Pipeline& p,Draw& d,std::string& error) {
  p={};p.vertex=vertex;p.fragment=fragment;p.samples=pass.samples;d={};
  auto format=[&](const SurfaceView& v) {
    if(v.format!=Format::Invalid)return v.format;
    auto i=std::find_if(frame.surfaces.begin(),frame.surfaces.end(),[&](const Surface& s){return s.id==v.surface;});
    return i==frame.surfaces.end()?Format::Invalid:i->format;
  };
  for(unsigned i=0;i<4;++i) {
    p.blend[i].write_mask=(mask>>(4*i))&15;
    if(!pass.colors[i])continue;
    p.colors[i]=format(pass.colors[i]->view);
    auto blend=regs.Get<gfx::reg::RB_BLENDCONTROL>(gfx::reg::RB_BLENDCONTROL::rt_register_indices[i]);auto& b=p.blend[i];
    b.source_rgb=Factor(blend.color_srcblend);b.destination_rgb=Factor(blend.color_destblend);b.rgb=Operation(blend.color_comb_fcn);
    b.source_alpha=Factor(blend.alpha_srcblend);b.destination_alpha=Factor(blend.alpha_destblend);b.alpha=Operation(blend.alpha_comb_fcn);
    b.enabled=!(b.source_rgb==BlendFactor::One && b.destination_rgb==BlendFactor::Zero && b.rgb==BlendOperation::Add &&
      b.source_alpha==BlendFactor::One && b.destination_alpha==BlendFactor::Zero && b.alpha==BlendOperation::Add);
  }
  auto mode=regs.Get<gfx::reg::PA_SU_SC_MODE_CNTL>();
  if(polygonal) {
    if(mode.cull_front && mode.cull_back) {error="both polygon faces culled";return false;}
    d.cull=mode.cull_front?Cull::Front:mode.cull_back?Cull::Back:Cull::None;d.clockwise=mode.face!=0;
  }
  d.depth_clamp=regs.Get<gfx::reg::PA_CL_CLIP_CNTL>().clip_disable;
  if(pass.depth) {p.depth=format(pass.depth->view);p.depth_compare=depth.z_enable?Compare(depth.zfunc):Compare::Always;p.depth_write=depth.z_enable&&depth.z_write_enable;}
  if(pass.stencil)p.stencil=format(pass.stencil->view);
  p.stencil_enabled=pass.stencil.has_value()&&depth.stencil_enable;
  if(p.stencil_enabled) {
    auto front=regs.Get<gfx::reg::RB_STENCILREFMASK>();
    auto back=polygonal&&depth.backface_enable?regs.Get<gfx::reg::RB_STENCILREFMASK>(gfx::XE_GPU_REG_RB_STENCILREFMASK_BF):front;
    p.front={Compare(depth.stencilfunc),StencilOperation(depth.stencilfail),StencilOperation(depth.stencilzfail),StencilOperation(depth.stencilzpass),front.stencilmask,front.stencilwritemask};
    p.back=polygonal&&depth.backface_enable?
      Stencil{Compare(depth.stencilfunc_bf),StencilOperation(depth.stencilfail_bf),StencilOperation(depth.stencilzfail_bf),StencilOperation(depth.stencilzpass_bf),back.stencilmask,back.stencilwritemask}:p.front;
    d.stencil_front=front.stencilref;d.stencil_back=back.stencilref;
  }
  d.viewport={double(viewport.xy_offset[0]),double(viewport.xy_offset[1]),double(viewport.xy_extent[0]),double(viewport.xy_extent[1]),viewport.z_min,viewport.z_max};
  gfx::draw_util::Scissor scissor;gfx::draw_util::GetScissor(regs,scissor);
  d.scissor.x=std::min(scissor.offset[0],pass.width);d.scissor.y=std::min(scissor.offset[1],pass.height);
  d.scissor.width=std::min(scissor.extent[0],pass.width-d.scissor.x);d.scissor.height=std::min(scissor.extent[1],pass.height-d.scissor.y);
  d.blend_color={regs.Get<float>(gfx::XE_GPU_REG_RB_BLEND_RED),regs.Get<float>(gfx::XE_GPU_REG_RB_BLEND_GREEN),
    regs.Get<float>(gfx::XE_GPU_REG_RB_BLEND_BLUE),regs.Get<float>(gfx::XE_GPU_REG_RB_BLEND_ALPHA)};
  gfx::draw_util::GetPreferredFacePolygonOffset(regs,polygonal,d.slope_bias,d.depth_bias);
  d.depth_bias*=regs.Get<gfx::reg::RB_DEPTH_INFO>().depth_format==gfx::xenos::DepthRenderTargetFormat::kD24S8?
    gfx::draw_util::kD3D10PolygonOffsetFactorUnorm24:gfx::draw_util::kD3D10PolygonOffsetFactorFloat24;
  d.slope_bias*=gfx::xenos::kPolygonOffsetScaleSubpixelUnit;return true;
}
} // namespace rex::native
