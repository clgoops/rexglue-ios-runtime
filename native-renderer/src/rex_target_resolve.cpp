// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_resolve.h"
#include "rex/native/rex_transfer_shader.h"
#include <rex/graphics/pipeline/texture/cache.h>
namespace rex::native {
namespace gfx=graphics;
namespace {Attachment Attach(SurfaceView v) {Attachment a;a.view=v;return a;}}
bool RexRenderTargets::Resolve(Frame& frame,const memory::Memory& memory,RexSharedMemory& shared,gfx::TextureCache& textures,
    uint32_t& written_address,uint32_t& written_length,std::string& error) {
  written_address=written_length=0;
  auto fail=[&](std::string message) {error=std::move(message);faulted_=true;return false;};
  if(!initialized_ || faulted_ || !resolves_ || !edram_)return fail("native resolve cache is unavailable");
  if(IsDrawResolutionScaled())return fail("native resolve requires original internal resolution");
  gfx::draw_util::ResolveInfo info;
  if(!gfx::draw_util::GetResolveInfo(register_file(),memory,1,1,false,false,info))return fail("guest resolve registers are invalid");
  if(!info.coordinate_info.width_div_8 || !info.height_div_8)return true;
  if(info.copy_dest_extent_length) {
    gfx::draw_util::ResolveCopyShaderConstants c;uint32_t gx,gy;
    auto index=info.GetCopyShader(1,1,c,gx,gy);
    if(index==gfx::draw_util::ResolveCopyShaderIndex::kUnknown)return fail("guest resolve format has no native copy kernel");
    uint32_t base,row_length,rows,pitch;info.GetCopyEdramTileSpan(base,row_length,rows,pitch);
    std::vector<ResolveCopyDumpRectangle> rectangles;GetResolveCopyRectanglesToDump(base,row_length,rows,pitch,rectangles);
    for(const auto& rect:rectangles) {
      const auto& target=*static_cast<const NativeTarget*>(rect.render_target);
      ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
      const uint32_t count=rect.GetDispatches(pitch,row_length,dispatches);
      for(uint32_t i=0;i<count;++i)if(!resolves_->Dump(frame,target.key(),target.image,*edram_,pitch,
          base+dispatches[i].offset,dispatches[i].width_tiles,dispatches[i].height_tiles,error))return fail(error);
    }
    if(!shared.RequestRange(info.copy_dest_extent_start,info.copy_dest_extent_length))return fail("native resolve destination is unavailable");
    const uint32_t range=info.copy_dest_extent_start-info.copy_dest_base+info.copy_dest_extent_length;
    auto dest=shared.ContiguousView(info.copy_dest_base,range,next_id_,error);if(!dest)return fail(error);
    c.dest_base-=info.copy_dest_base;
    if(!resolves_->Copy(frame,index,c,*edram_,*dest,gx,gy,error))return fail(error);
    auto written=*dest;written.offset+=info.copy_dest_extent_start-info.copy_dest_base;
    written.length=info.copy_dest_extent_length;
    if(!shared.CopyBack(info.copy_dest_extent_start,info.copy_dest_extent_length,written,error))return fail(error);
    textures.MarkRangeAsResolved(info.copy_dest_extent_start,info.copy_dest_extent_length);
    written_address=info.copy_dest_extent_start;written_length=info.copy_dest_extent_length;
  }
  if(info.IsClearingColor() || info.IsClearingDepth()) {
    Transfer::Rectangle rect;RenderTarget *depth_target=nullptr,*color_target=nullptr;std::vector<Transfer> depth_transfers,color_transfers;
    if(PrepareHostRenderTargetsResolveClear(info,rect,depth_target,depth_transfers,color_target,color_transfers)) {
      for(unsigned which=0;which<2;++which) {
        auto* target=static_cast<NativeTarget*>(which?color_target:depth_target);if(!target)continue;
        const auto& transfers=which?color_transfers:depth_transfers;
        for(const auto& transfer:transfers)if(!EncodeTransfer(frame,*target,transfer,error))return fail(error);
        if(!Declare(frame,target->image,error))return fail(error);
        auto key=target->key();Pass pass;pass.width=target->image.width;pass.height=target->image.height;pass.samples=target->image.samples;
        RectClear clear;clear.rectangle={rect.x_pixels,rect.y_pixels,rect.width_pixels,rect.height_pixels};
        SurfaceView view{target->image.id};
        if(key.is_depth) {
          pass.depth=pass.stencil=Attach(view);clear.depth=clear.stencil=true;clear.stencil_value=info.rb_depth_clear&255;
          uint32_t depth_bits=info.rb_depth_clear>>8;
          clear.depth_value=key.GetDepthFormat()==gfx::xenos::DepthRenderTargetFormat::kD24S8?
            gfx::xenos::UNorm24To32(depth_bits):gfx::xenos::Float20e4To32(depth_bits)*.5f;
        } else {
          view.format=RexTransferShader::OwnershipFormat(key.GetColorFormat());pass.colors[0]=Attach(view);clear.colors=1;
          const uint64_t bits=info.rb_color_clear|(uint64_t(info.rb_color_clear_lo)<<32);
          using F=gfx::xenos::ColorRenderTargetFormat;
          switch(key.GetColorFormat()) {
            case F::k_8_8_8_8:case F::k_8_8_8_8_GAMMA:
              for(unsigned i=0;i<4;++i)clear.color[0][i]=double((bits>>(8*i))&255)/255;break;
            case F::k_2_10_10_10:case F::k_2_10_10_10_AS_10_10_10_10:
              for(unsigned i=0;i<3;++i)clear.color[0][i]=double((bits>>(10*i))&1023)/1023;
              clear.color[0][3]=double((bits>>30)&3)/3;break;
            case F::k_2_10_10_10_FLOAT:case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
              for(unsigned i=0;i<3;++i)clear.color[0][i]=gfx::xenos::Float7e3To32(uint32_t((bits>>(10*i))&1023));
              clear.color[0][3]=double((bits>>30)&3)/3;break;
            case F::k_16_16:case F::k_16_16_FLOAT:
              for(unsigned i=0;i<2;++i)clear.color[0][i]=(bits>>(16*i))&65535;break;
            case F::k_16_16_16_16:case F::k_16_16_16_16_FLOAT:
              for(unsigned i=0;i<4;++i)clear.color[0][i]=(bits>>(16*i))&65535;break;
            case F::k_32_FLOAT:clear.color[0][0]=uint32_t(bits);break;
            case F::k_32_32_FLOAT:clear.color[0][0]=uint32_t(bits);clear.color[0][1]=uint32_t(bits>>32);break;
            default:return fail("unsupported native resolve clear format");
          }
        }
        pass.commands.push_back(clear);AppendPass(frame,std::move(pass));
      }
    }
  }
  return true;
}
} // namespace rex::native
