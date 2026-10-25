// SPDX-License-Identifier: BSD-3-Clause
// Layout and loading rules adapted from ReXGlue/Xenia's BSD texture cache.
#include "rex/native/rex_textures.h"
#include <rex/graphics/pipeline/texture/info.h>
#include <cstring>
namespace rex::native {
namespace gfx=graphics;
namespace {
bool Dimensions(gfx::xenos::FetchOpDimension binding,gfx::xenos::DataDimension resource) {
  using B=gfx::xenos::FetchOpDimension;using R=gfx::xenos::DataDimension;
  switch(binding) {
    case B::k1D:case B::k2D:return resource==R::k1D || resource==R::k2DOrStacked || resource==R::k3D;
    case B::k3DOrStacked:return resource==R::k3D;
    case B::kCube:return resource==R::kCube;
    default:return false;
  }
}
Bytes Constants(const auto& value) {auto bytes=std::make_shared<std::vector<std::byte>>(256);std::memcpy(bytes->data(),&value,sizeof(value));return bytes;}
template<class T> void Add(std::vector<T>& list,const T& value) {
  if(std::none_of(list.begin(),list.end(),[&](const T& s){return s.id==value.id;}))list.push_back(value);
}
}
RexTextures::RexTextures(const gfx::RegisterFile& regs,RexSharedMemory& shared,std::function<Identity()> next)
    :gfx::TextureCache(regs,shared,1,1),shared_(shared),next_(std::move(next)) {}
RexTextures::~RexTextures() {DestroyAllTextures(true);}
void RexTextures::BeginNativeFrame(Frame& f,uint64_t submission,uint64_t completed) {
  frame_=&f;CompletedSubmissionUpdated(completed);BeginSubmission(submission);BeginFrame();
}
void RexTextures::EndNativeFrame() {frame_=nullptr;}
bool RexTextures::Declare(const Surface& image) {
  if(!frame_) {failure_="native texture operation outside a frame";return false;}
  Add(frame_->surfaces,image);return true;
}
bool RexTextures::IsSignedVersionSeparateForFormat(Key key) const {
  auto p=Formats(key.format);return p.format_unsigned.format!=Format::Invalid &&
    p.format_signed.format!=Format::Invalid && p.format_unsigned.format!=p.format_signed.format;
}
uint32_t RexTextures::GetHostFormatSwizzle(Key key) const {return Formats(key.format).swizzle;}
std::unique_ptr<gfx::TextureCache::Texture> RexTextures::CreateTexture(Key key) {
  auto pair=Formats(key.format);
  auto format=key.signed_separate || pair.format_unsigned.load_shader==kLoadShaderIndexUnknown?
    pair.format_signed:pair.format_unsigned;
  if(format.format==Format::Invalid || format.load_shader==kLoadShaderIndexUnknown || key.scaled_resolve) {
    failure_="unsupported native guest texture format "+std::to_string(uint32_t(key.format));return {};
  }
  Surface s;s.id=next_();s.width=key.GetWidth();s.height=key.GetHeight();s.levels=key.mip_max_level+1;
  s.format=format.format;s.owner=std::make_shared<const Identity>(s.id);
  if(key.dimension==gfx::xenos::DataDimension::k3D) {s.kind=TextureKind::ThreeD;s.depth=key.GetDepthOrArraySize();}
  else if(key.dimension==gfx::xenos::DataDimension::kCube) {s.kind=TextureKind::Cube;s.layers=6;}
  else {s.kind=TextureKind::TwoDArray;s.layers=key.GetDepthOrArraySize();}
  return std::make_unique<NativeTexture>(*this,key,std::move(s));
}
const ConvertedShader* RexTextures::Kernel(LoadIndex index) {
  if(auto* result=kernels_.Find(uint32_t(index)))return result;
  auto words=LoadKernel(index);ConvertedShader shader;
  if(words.empty()) {failure_="native texture load kernel is unavailable: "+std::to_string(index);return nullptr;}
  if(!ConvertSpirv(words,next_(),Stage::Compute,"main",shader,failure_))return nullptr;
  auto* result=&kernels_.Insert(uint32_t(index),std::move(shader));kernels_.Trim(kLoadShaderCount);return result;
}
bool RexTextures::Prepare(uint32_t mask,RexTextureState& state,std::string& error) {
  if(!frame_ || faulted()) {error=failure_.empty()?"native texture cache has no frame":failure_;return false;}
  RequestTextures(mask);
  if(faulted()) {error=failure_;return false;}
  for(unsigned i=0;i<32;++i)if(mask&(1u<<i)) {
    state.swizzled_signs[i]=GetActiveTextureSwizzledSigns(i);state.host_swizzles[i]=GetActiveTextureHostSwizzle(i);
    if(IsActiveTextureResolutionScaled(i))state.resolution_scaled|=1u<<i;else state.resolution_scaled&=~(1u<<i);
  }
  return true;
}
bool RexTextures::Null(gfx::xenos::FetchOpDimension dimension,SurfaceView& out,std::string& error) {
  unsigned index=dimension==gfx::xenos::FetchOpDimension::k3DOrStacked?1:dimension==gfx::xenos::FetchOpDimension::kCube?2:0;
  if(!nulls_[index]) {
    Surface image;image.id=next_();image.format=Format::RGBA8;image.width=image.height=1;
    image.kind=index==1?TextureKind::ThreeD:index==2?TextureKind::Cube:TextureKind::TwoDArray;
    image.layers=index==2?6:1;image.owner=std::make_shared<const Identity>(image.id);nulls_[index]=image;
    if(!Declare(image)) {error=failure_;return false;}
    auto zero=std::make_shared<const std::vector<std::byte>>(4);
    for(uint32_t slice=0;slice<image.layers;++slice) {SurfaceView v{image.id};v.slice=slice;frame_->commands.push_back(TextureUpload{v,zero,4,4});}
  }
  if(!Declare(*nulls_[index])) {error=failure_;return false;}
  out={nulls_[index]->id};return true;
}
bool RexTextures::Active(uint32_t fetch,gfx::xenos::FetchOpDimension dimension,bool is_signed,SurfaceView& out,std::string& error) {
  if(fetch>=32 || !frame_ || faulted()) {error="invalid native texture binding request";return false;}
  auto* binding=GetValidTextureBinding(fetch);
  if(!binding || !Dimensions(dimension,binding->key.dimension))return Null(dimension,out,error);
  const auto formats=Formats(binding->key.format);
  if((is_signed?formats.format_signed:formats.format_unsigned).format==Format::Invalid)return Null(dimension,out,error);
  auto* texture=static_cast<NativeTexture*>(is_signed && IsSignedVersionSeparateForFormat(binding->key)?binding->texture_signed:binding->texture);
  if(!texture)return Null(dimension,out,error);
  if(!Declare(texture->image)) {error=failure_;return false;}
  texture->MarkAsUsed();out={texture->image.id};
  if(binding->key.dimension==gfx::xenos::DataDimension::k3D &&
      (dimension==gfx::xenos::FetchOpDimension::k1D || dimension==gfx::xenos::FetchOpDimension::k2D)) {
    if(!texture->flat) {
      auto flat=texture->image;flat.id=next_();flat.kind=TextureKind::TwoDArray;flat.depth=flat.layers=1;
      flat.owner=std::make_shared<const Identity>(flat.id);texture->flat=std::move(flat);
    }
    Declare(*texture->flat);
    if(texture->flat_loaded!=texture->loaded) {
      for(uint32_t level=0;level<texture->image.levels;++level) {
        ImageCopy copy;copy.source={texture->image.id};copy.destination={texture->flat->id};
        copy.source.level=copy.destination.level=level;copy.width=std::max(1u,texture->image.width>>level);
        copy.height=std::max(1u,texture->image.height>>level);frame_->commands.push_back(copy);
      }
      texture->flat_loaded=texture->loaded;
    }
    out={texture->flat->id};
  }
  return true;
}
bool RexTextures::Swap(SurfaceView& out,uint32_t& swizzle,std::string& error) {
  auto fetch=register_file().GetTextureFetch(0);Key key;BindingInfoFromFetchConstant(fetch,key,nullptr);
  if(!key.is_valid || !key.base_page || key.dimension!=gfx::xenos::DataDimension::k2DOrStacked) {
    error="swap texture fetch is invalid";return false;
  }
  auto* texture=static_cast<NativeTexture*>(FindOrCreateTexture(key));
  if(!texture || !LoadTextureData(*texture) || !Declare(texture->image)) {error=failure_;return false;}
  texture->MarkAsUsed();out={texture->image.id};swizzle=GuestToHostSwizzle(fetch.swizzle,GetHostFormatSwizzle(key));return true;
}
bool RexTextures::LoadTextureDataFromResidentMemoryImpl(Texture& texture,bool base,bool mips) {
  if(!frame_ || faulted())return false;
  const bool loaded=Load(static_cast<NativeTexture&>(texture),base,mips);
  if(!loaded && failure_.empty())failure_="native texture upload failed";return loaded;
}
bool RexTextures::Load(NativeTexture& texture,bool load_base,bool load_mips) {
  auto key=texture.key();const auto pair=Formats(key.format);
  auto format=key.signed_separate || pair.format_unsigned.load_shader==kLoadShaderIndexUnknown?pair.format_signed:pair.format_unsigned;
  auto* converted=Kernel(format.load_shader);if(!converted || !Declare(texture.image))return false;
  const auto& info=GetLoadShaderInfo(format.load_shader);const auto& layout=texture.guest_layout();
  const auto* guest=gfx::FormatInfo::Get(key.format);
  const uint32_t block_width=guest->block_width,block_height=guest->block_height,bpb=guest->bytes_per_block();
  const bool is_3d=key.dimension==gfx::xenos::DataDimension::k3D;
  const uint32_t width=key.GetWidth(),height=key.GetHeight(),depth=is_3d?key.GetDepthOrArraySize():1,array=is_3d?1:key.GetDepthOrArraySize();
  const uint32_t first=load_base?0:1,last=load_mips?key.mip_max_level:0,packed=layout.packed_level;
  const uint32_t loop_first=packed==0?uint32_t(first!=0):std::min(first,packed);
  const uint32_t loop_last=packed==0?uint32_t(last!=0):std::min(last,packed);
  const uint32_t host_bw=format.block_compressed?block_width:1,host_bh=format.block_compressed?block_height:1;
  uint32_t blocks_per_thread=1u<<info.guest_x_blocks_per_thread_log2;
  if(!format.block_compressed)blocks_per_thread*=block_width;
  struct Host {uint64_t offset=0,slice=0;uint32_t pitch=0,rows=0;};
  Host host_base,host_mips[gfx::xenos::kTextureMaxMips]{};uint64_t size=0;
  for(uint32_t loop=loop_first;loop<=loop_last;++loop) {
    bool base=loop==0;uint32_t level=packed==0?0:loop;auto& h=base?host_base:host_mips[level];
    const auto& g=base?layout.base:layout.mips[level];h.offset=size;
    uint32_t w=level==packed?g.x_extent_blocks*block_width:std::max(1u,width>>level);
    uint32_t y=level==packed?g.y_extent_blocks*block_height:std::max(1u,height>>level);
    uint32_t z=level==packed?g.z_extent:std::max(1u,depth>>level);
    h.pitch=rex::round_up((w+host_bw-1)/host_bw,blocks_per_thread);h.rows=(y+host_bh-1)/host_bh;
    h.slice=uint64_t(info.bytes_per_host_block)*h.pitch*h.rows*z;size+=h.slice*array;
  }
  if(!size || size>256ull*1024*1024) {failure_="native texture staging exceeds 256 MiB";return false;}
  BufferStorage scratch{next_(),size,true,{}};scratch.owner=std::make_shared<const Identity>(scratch.id);frame_->buffer_storage.push_back(scratch);
  Add(frame_->shaders,converted->shader);
  const uint32_t source_alignment=1u<<info.source_bpe_log2;
  std::optional<BufferView> source_base,source_mips;
  if(first==0)source_base=shared_.ContiguousView(key.base_page<<12,rex::align(texture.GetGuestBaseSize(),source_alignment),next_,failure_);
  if(last!=0)source_mips=shared_.ContiguousView(key.mip_page<<12,rex::align(texture.GetGuestMipsSize(),source_alignment),next_,failure_);
  if((first==0&&!source_base)||(last!=0&&!source_mips))return false;
  for(uint32_t loop=loop_first;loop<=loop_last;++loop) {
    bool base=loop==0;uint32_t level=packed==0?0:loop;const auto& g=base?layout.base:layout.mips[level];const auto& h=base?host_base:host_mips[level];
    LoadConstants c{};c.is_tiled_3d_endian_scale=uint32_t(key.tiled)|(uint32_t(is_3d||texture.force_load_3d_tiling())<<1)|(uint32_t(key.endianness)<<2)|(1u<<4)|(1u<<7);
    c.guest_offset=base?0:layout.mip_offsets_bytes[level];c.guest_pitch_aligned=key.tiled?g.row_pitch_bytes/bpb:g.row_pitch_bytes;
    c.guest_z_stride_block_rows_aligned=g.z_slice_stride_block_rows;
    const uint32_t w=level==packed?g.x_extent_blocks*block_width:std::max(1u,width>>level);
    const uint32_t y=level==packed?g.y_extent_blocks*block_height:std::max(1u,height>>level);
    c.size_blocks[0]=(w+block_width-1)/block_width;c.size_blocks[1]=(y+block_height-1)/block_height;
    c.size_blocks[2]=level==packed?g.z_extent:std::max(1u,depth>>level);c.height_texels=y;
    c.host_offset=uint32_t(h.offset);c.host_pitch=info.bytes_per_host_block*h.pitch;
    for(uint32_t slice=0;slice<array;++slice) {
      Buffer constants{next_(),Constants(c),true};frame_->buffers.push_back(constants);
      std::vector<DescriptorResources> resources={
        {ResourceSource::Descriptor,0,0,{{scratch.id,0,size}},{},{}},
        {ResourceSource::Descriptor,1,0,{base?*source_base:*source_mips},{},{}},
        {ResourceSource::PushConstants,0,0,{{constants.id,0,256}},{},{}}};
      Dispatch d;d.shader=converted->shader.id;d.threads=converted->workgroup_size;
      d.groups={(c.size_blocks[0]+(1u<<info.GetGuestXBlocksPerGroupLog2())-1)>>info.GetGuestXBlocksPerGroupLog2(),
        (c.size_blocks[1]+(1u<<kLoadGuestYBlocksPerGroupLog2)-1)>>kLoadGuestYBlocksPerGroupLog2,c.size_blocks[2]};
      std::optional<Buffer> aux;if(!BindConverted(*converted,resources,next_(),d.bindings,aux,failure_))return false;
      if(aux)frame_->buffers.push_back(*aux);frame_->commands.push_back(std::move(d));
      c.guest_offset+=g.array_slice_stride_bytes;c.host_offset+=uint32_t(h.slice);
    }
  }
  for(uint32_t level=first;level<=last;++level) {
    const auto& h=level==0?host_base:host_mips[std::min(level,packed)];uint64_t offset=h.offset;
    if(level>=packed) {
      uint32_t x,y,z;gfx::texture_util::GetPackedMipOffset(width,height,depth,key.format,level,x,y,z);
      if(!format.block_compressed) {x*=block_width;y*=block_height;}
      offset+=uint64_t(info.bytes_per_host_block)*(x+uint64_t(h.pitch)*(y+uint64_t(h.rows)*z));
    }
    for(uint32_t slice=0;slice<array;++slice) {
      BufferToTexture copy;copy.source={scratch.id,offset+h.slice*slice,size-offset-h.slice*slice};
      copy.destination={texture.image.id};copy.destination.level=level;copy.destination.slice=slice;
      copy.width=std::max(1u,width>>level);copy.height=std::max(1u,height>>level);copy.depth=std::max(1u,depth>>level);
      copy.bytes_per_row=uint64_t(info.bytes_per_host_block)*h.pitch;copy.bytes_per_image=copy.bytes_per_row*h.rows;
      frame_->commands.push_back(copy);
    }
  }
  ++texture.loaded;texture.MarkAsUsed();return true;
}
} // namespace rex::native
