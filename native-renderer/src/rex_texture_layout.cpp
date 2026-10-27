// SPDX-License-Identifier: BSD-3-Clause
// Host staging layout follows the pinned SDK's block and packed-mip rules.
#include "rex/native/rex_textures.h"
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/math.h>
namespace rex::native {
namespace gfx=graphics;
bool RexTextures::PlanLoad(Key key,const gfx::texture_util::TextureGuestLayout& layout,
    const LoadInfo& info,HostFormat format,bool base_data,bool mip_data,LoadPlan& plan,std::string& error) {
  plan={};error.clear();
  if((!base_data&&!mip_data)||!info.bytes_per_host_block||key.mip_max_level>=gfx::xenos::kTextureMaxMips) {
    error="invalid native texture load plan";return false;
  }
  const auto* guest=gfx::FormatInfo::Get(key.format);
  const uint32_t bw=guest->block_width,bh=guest->block_height,bpb=guest->bytes_per_block();
  const bool is_3d=key.dimension==gfx::xenos::DataDimension::k3D;
  const uint32_t width=key.GetWidth(),height=key.GetHeight(),depth=is_3d?key.GetDepthOrArraySize():1,array=is_3d?1:key.GetDepthOrArraySize();
  const uint32_t first=base_data?0:1,last=mip_data?key.mip_max_level:0,packed=layout.packed_level;
  const uint32_t loop_first=packed==0?uint32_t(first!=0):std::min(first,packed);
  const uint32_t loop_last=packed==0?uint32_t(last!=0):std::min(last,packed);
  const uint32_t host_bw=format.block_compressed?bw:1,host_bh=format.block_compressed?bh:1;
  uint32_t blocks_per_thread=1u<<info.guest_x_blocks_per_thread_log2;
  if(!format.block_compressed)blocks_per_thread*=bw;
  struct Host {uint64_t offset=0,slice=0;uint32_t pitch=0,rows=0;};
  Host host_base,host_mips[gfx::xenos::kTextureMaxMips]{};
  for(uint32_t loop=loop_first;loop<=loop_last;++loop) {
    const bool base=loop==0;const uint32_t level=packed==0?0:loop;auto& h=base?host_base:host_mips[level];
    const auto& g=base?layout.base:layout.mips[level];h.offset=plan.size;
    const uint32_t w=level==packed?g.x_extent_blocks*bw:std::max(1u,width>>level);
    const uint32_t y=level==packed?g.y_extent_blocks*bh:std::max(1u,height>>level);
    const uint32_t z=level==packed?g.z_extent:std::max(1u,depth>>level);
    h.pitch=rex::round_up((w+host_bw-1)/host_bw,blocks_per_thread);h.rows=(y+host_bh-1)/host_bh;
    h.slice=uint64_t(info.bytes_per_host_block)*h.pitch*h.rows*z;plan.size+=h.slice*array;
  }
  if(!plan.size||plan.size>256ull<<20) {error="native texture staging exceeds 256 MiB";return false;}
  for(uint32_t loop=loop_first;loop<=loop_last;++loop) {
    const bool base=loop==0;const uint32_t level=packed==0?0:loop;const auto& g=base?layout.base:layout.mips[level];const auto& h=base?host_base:host_mips[level];
    LoadParams c{};c.is_tiled_3d_endian_scale=uint32_t(key.tiled)|(uint32_t(is_3d)<<1)|(uint32_t(key.endianness)<<2)|(1u<<4)|(1u<<7);
    c.guest_offset=base?0:layout.mip_offsets_bytes[level];c.guest_pitch_aligned=key.tiled?g.row_pitch_bytes/bpb:g.row_pitch_bytes;
    c.guest_z_stride_block_rows_aligned=g.z_slice_stride_block_rows;
    const uint32_t w=level==packed?g.x_extent_blocks*bw:std::max(1u,width>>level);
    const uint32_t y=level==packed?g.y_extent_blocks*bh:std::max(1u,height>>level);
    c.size_blocks[0]=(w+bw-1)/bw;c.size_blocks[1]=(y+bh-1)/bh;
    c.size_blocks[2]=level==packed?g.z_extent:std::max(1u,depth>>level);c.height_texels=y;
    c.host_offset=uint32_t(h.offset);c.host_pitch=info.bytes_per_host_block*h.pitch;
    for(uint32_t slice=0;slice<array;++slice) {
      plan.loads.push_back({c,{(c.size_blocks[0]+(1u<<info.GetGuestXBlocksPerGroupLog2())-1)>>info.GetGuestXBlocksPerGroupLog2(),
        (c.size_blocks[1]+(1u<<kLoadGuestYBlocksPerGroupLog2)-1)>>kLoadGuestYBlocksPerGroupLog2,c.size_blocks[2]},base});
      c.guest_offset+=g.array_slice_stride_bytes;c.host_offset+=uint32_t(h.slice);
    }
  }
  for(uint32_t level=first;level<=last;++level) {
    const auto& h=level==0?host_base:host_mips[std::min(level,packed)];uint64_t offset=h.offset;
    if(level>=packed) {
      uint32_t x,y,z;gfx::texture_util::GetPackedMipOffset(width,height,depth,key.format,level,x,y,z);
      if(!format.block_compressed) {x*=bw;y*=bh;}
      offset+=uint64_t(info.bytes_per_host_block)*(x+uint64_t(h.pitch)*(y+uint64_t(h.rows)*z));
    }
    for(uint32_t slice=0;slice<array;++slice) {
      BufferToTexture copy;copy.source={{},offset+h.slice*slice,plan.size-offset-h.slice*slice};
      copy.destination.level=level;copy.destination.slice=slice;
      copy.width=std::max(1u,width>>level);copy.height=std::max(1u,height>>level);copy.depth=std::max(1u,depth>>level);
      copy.bytes_per_row=uint64_t(info.bytes_per_host_block)*h.pitch;copy.bytes_per_image=copy.bytes_per_row*h.rows;
      plan.copies.push_back(copy);
    }
  }
  return true;
}
}
