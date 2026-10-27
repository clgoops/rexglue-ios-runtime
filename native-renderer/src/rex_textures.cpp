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
RexTextures::RexTextures(const gfx::RegisterFile& regs,RexSharedMemory& shared,std::function<Identity()> next,bool native_bc)
    :gfx::TextureCache(regs,shared,1,1),shared_(shared),next_(std::move(next)),native_bc_(native_bc) {}
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
  auto p=Formats(key,native_bc_);return p.format_unsigned.format!=Format::Invalid &&
    p.format_signed.format!=Format::Invalid && p.format_unsigned.format!=p.format_signed.format;
}
uint32_t RexTextures::GetHostFormatSwizzle(Key key) const {return Formats(key,native_bc_).swizzle;}
std::unique_ptr<gfx::TextureCache::Texture> RexTextures::CreateTexture(Key key) {
  auto pair=Formats(key,native_bc_);
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
  const auto formats=Formats(binding->key,native_bc_);
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
  auto key=texture.key();const auto pair=Formats(key,native_bc_);
  auto format=key.signed_separate || pair.format_unsigned.load_shader==kLoadShaderIndexUnknown?pair.format_signed:pair.format_unsigned;
  auto* converted=Kernel(format.load_shader);if(!converted || !Declare(texture.image))return false;
  const auto& info=GetLoadShaderInfo(format.load_shader);const auto& layout=texture.guest_layout();
  LoadPlan plan;
  if(!PlanLoad(key,layout,info,format,load_base,load_mips,plan,failure_))return false;
  // Some old 2D declarations use 3D tiled addressing; preserve that SDK rule.
  if(texture.force_load_3d_tiling())for(auto& step:plan.loads)step.constants.is_tiled_3d_endian_scale|=2;
  BufferStorage scratch{next_(),plan.size,true,{}};scratch.owner=std::make_shared<const Identity>(scratch.id);frame_->buffer_storage.push_back(scratch);
  Add(frame_->shaders,converted->shader);
  const uint32_t source_alignment=1u<<info.source_bpe_log2;
  std::optional<BufferView> source_base,source_mips;
  if(load_base)source_base=shared_.ContiguousView(key.base_page<<12,rex::align(texture.GetGuestBaseSize(),source_alignment),next_,failure_);
  if(load_mips&&key.mip_max_level)source_mips=shared_.ContiguousView(key.mip_page<<12,rex::align(texture.GetGuestMipsSize(),source_alignment),next_,failure_);
  if((load_base&&!source_base)||(load_mips&&key.mip_max_level&&!source_mips))return false;
  for(const auto& step:plan.loads) {
    Buffer constants{next_(),Constants(step.constants),true};frame_->buffers.push_back(constants);
    std::vector<DescriptorResources> resources={
      {ResourceSource::Descriptor,0,0,{{scratch.id,0,plan.size}},{},{}},
      {ResourceSource::Descriptor,1,0,{step.base?*source_base:*source_mips},{},{}},
      {ResourceSource::PushConstants,0,0,{{constants.id,0,256}},{},{}}};
    Dispatch d;d.shader=converted->shader.id;d.threads=converted->workgroup_size;d.groups=step.groups;
    std::optional<Buffer> aux;if(!BindConverted(*converted,resources,next_(),d.bindings,aux,failure_))return false;
    if(aux)frame_->buffers.push_back(*aux);frame_->commands.push_back(std::move(d));
  }
  for(auto copy:plan.copies) {
    copy.source.buffer=scratch.id;copy.destination.surface=texture.image.id;frame_->commands.push_back(copy);
  }
  if(format.block_compressed)++stats_.native_bc;else ++stats_.uncompressed;
  stats_.staging_bytes+=plan.size;
  ++texture.loaded;texture.MarkAsUsed();return true;
}
} // namespace rex::native
