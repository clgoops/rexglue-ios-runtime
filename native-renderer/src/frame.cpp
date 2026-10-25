// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/frame.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace rex::native {
FormatInfo Describe(Format format) {
  switch(format) {
    case Format::R8:case Format::R8Snorm:return {1,1,1,true};
    case Format::RG8:case Format::RG8Snorm:case Format::R16Float:case Format::R16Unorm:case Format::R16Snorm:
    case Format::BGR565:case Format::BGR5A1:case Format::ABGR4:return {1,1,2,true};
    case Format::RGBA8:case Format::RGBA8Srgb:case Format::BGRA8:case Format::BGRA8Srgb:
    case Format::RGB10A2:case Format::RG16Float:case Format::RG16Snorm:case Format::R32Float:
    case Format::RG16Uint:case Format::R32Uint:case Format::RGBA8Snorm:case Format::RG16Unorm:return {1,1,4,true};
    case Format::RGBA16Float:case Format::RGBA16Snorm:case Format::RG32Float:
    case Format::RGBA16Uint:case Format::RG32Uint:case Format::RGBA16Unorm:return {1,1,8,true};
    case Format::RGBA32Float:return {1,1,16,true};
    case Format::Depth32Float:return {1,1,4,false,true};
    case Format::Depth32Stencil8:return {1,1,8,false,true,true};
    case Format::Stencil8View:return {1,1,8,false,false,true};
    case Format::BC1:case Format::BC1Srgb:case Format::BC4:case Format::BC4Signed:return {4,4,8,true};
    case Format::BC2:case Format::BC2Srgb:case Format::BC3:case Format::BC3Srgb:
    case Format::BC5:case Format::BC5Signed:case Format::BC6Float:case Format::BC6SignedFloat:
    case Format::BC7:case Format::BC7Srgb:case Format::ASTC4x4:case Format::ASTC4x4Srgb:return {4,4,16,true};
    default:return {};
  }
}
bool CompatibleView(Format a,Format b) {
  if(a==b)return Describe(a).bytes!=0;
  auto pair=[&](Format x,Format y){return (a==x&&b==y)||(a==y&&b==x);};
  auto group=[&](std::initializer_list<Format> values) {
    return std::find(values.begin(),values.end(),a)!=values.end() &&
           std::find(values.begin(),values.end(),b)!=values.end();
  };
  if(group({Format::RG16Snorm,Format::RG16Float,Format::RG16Uint}) ||
     group({Format::RGBA16Snorm,Format::RGBA16Float,Format::RGBA16Uint}) ||
     pair(Format::R32Float,Format::R32Uint) || pair(Format::RG32Float,Format::RG32Uint))return true;
  // Metal's X32_Stencil8 is a sampled view of Depth32Float_Stencil8. It is
  // never a storage allocation, color target, or reciprocal depth view.
  if(a==Format::Depth32Stencil8 && b==Format::Stencil8View)return true;
  return pair(Format::RGBA8,Format::RGBA8Srgb)||pair(Format::BGRA8,Format::BGRA8Srgb)||
      pair(Format::BC1,Format::BC1Srgb)||pair(Format::BC2,Format::BC2Srgb)||
      pair(Format::BC3,Format::BC3Srgb)||pair(Format::BC7,Format::BC7Srgb)||
      pair(Format::ASTC4x4,Format::ASTC4x4Srgb);
}
namespace {
template<class T> bool Enum(T value){return value>=T{}&&value<T::Count;}
bool PowerOfTwo(uint64_t n){return n&&!(n&(n-1));}
bool Finite(auto&& values){for(auto v:values)if(!std::isfinite(v))return false;return true;}
struct Admission {
  const Frame& frame;const Limits& limits;std::string& error;
  std::set<Identity> identities;
  std::map<Identity,const Surface*> surfaces;
  std::map<Identity,const Buffer*> buffers;
  std::map<Identity,const BufferStorage*> buffer_storage;
  std::map<Identity,const Shader*> shaders;
  std::map<Identity,const Pipeline*> pipelines;
  std::map<Identity,const Sampler*> samplers;
  uint32_t passes=0,draws=0;
  bool Fail(const char* message){error=message;return false;}
  template<class T> bool Add(const std::vector<T>& values,std::map<Identity,const T*>& map) {
    for(const auto& value:values) {
      if(!value.id || !identities.insert(value.id).second)return Fail("Invalid or duplicate resource identity");
      map.emplace(value.id,&value);
    }
    return true;
  }
  const Surface* View(const SurfaceView& view) {
    auto i=surfaces.find(view.surface);if(i==surfaces.end())return nullptr;
    const auto& s=*i->second;
    if(view.level>=s.levels || view.slice>=s.layers ||
       view.plane>=(s.kind==TextureKind::ThreeD?std::max(1u,s.depth>>view.level):1u) ||
       (view.format!=Format::Invalid&&!CompatibleView(s.format,view.format)))return nullptr;
    return &s;
  }
  bool BufferRange(const BufferView& view,uint64_t minimum=0,uint64_t alignment=1) {
    auto i=buffers.find(view.buffer);auto storage=buffer_storage.find(view.buffer);
    if(i==buffers.end()&&storage==buffer_storage.end())return false;
    const uint64_t size=i!=buffers.end()?i->second->bytes->size():storage->second->size;
    return alignment&&view.offset%alignment==0&&view.offset<=size&&
        view.length<=size-view.offset&&minimum<=view.length;
  }
  Format ViewFormat(const SurfaceView& view) {
    return view.format!=Format::Invalid?view.format:surfaces.at(view.surface)->format;
  }
  bool RectInside(const Rect& r,uint32_t w,uint32_t h) {
    return r.x<=w&&r.y<=h&&r.width<=w-r.x&&r.height<=h-r.y;
  }
  bool AttachmentValid(const Attachment& a,const Pass& pass,int aspect) {
    const Surface* s=View(a.view);if(!s || !s->renderable || !Enum(a.load)||!Enum(a.store))return false;
    if(ViewFormat(a.view)==Format::Stencil8View)return false;
    const auto format=Describe(ViewFormat(a.view));
    if((aspect==0&&!format.color)||(aspect==1&&!format.depth)||(aspect==2&&!format.stencil))return false;
    if(pass.width>std::max(1u,s->width>>a.view.level)||pass.height>std::max(1u,s->height>>a.view.level)||pass.samples!=s->samples)return false;
    if(!Finite(a.color)||!std::isfinite(a.depth)||a.depth<0||a.depth>1||a.stencil>255)return false;
    return true;
  }
  bool BindingValid(const Shader& shader,const Bindings& binding,const Pass* pass) {
    std::set<uint32_t> bs,ts,ss;
    for(const auto& b:binding.buffers)
      if(b.index>=limits.max_buffer_slots||!bs.insert(b.index).second||!BufferRange(b.view))return Fail("Invalid or duplicate buffer binding");
    for(const auto& t:binding.textures) {
      const Surface* surface=View(t.view);
      if(t.index>=limits.max_texture_slots||!ts.insert(t.index).second||!surface||(!surface->sampled&&!surface->storage)||t.view.plane||
          ((surface->kind==TextureKind::Cube||surface->kind==TextureKind::CubeArray)&&t.view.slice%6))return Fail("Invalid or duplicate texture binding");
      // A frontend must lower attachment feedback/aliases explicitly. Reading
      // the currently written storage as an ordinary texture is not admitted.
      if(pass) {
        for(const auto& color:pass->colors)if(color&&color->view.surface==t.view.surface)return Fail("Unlowered attachment feedback");
        for(const auto* a:{&pass->depth,&pass->stencil})if(*a&&(*a)->view.surface==t.view.surface)return Fail("Unlowered depth/stencil feedback");
      }
    }
    for(const auto& s:binding.samplers)
      if(s.index>=limits.max_sampler_slots||!ss.insert(s.index).second||!samplers.contains(s.sampler))return Fail("Invalid or duplicate sampler binding");
    for(const auto& a:shader.arguments)for(uint32_t element=0;element<a.count;++element) {
      const uint32_t index=a.index+element;
      if(a.kind==ArgumentKind::Buffer) {
        auto b=std::find_if(binding.buffers.begin(),binding.buffers.end(),[&](const auto& b){return b.index==index;});
        if(b==binding.buffers.end()||!BufferRange(b->view,a.minimum_bytes,a.alignment))return Fail("Missing, undersized or misaligned shader buffer");
        if(a.writable&&(!buffer_storage.contains(b->view.buffer)||!buffer_storage.at(b->view.buffer)->writable))return Fail("Shader writes require mutable buffer storage");
      } else if(a.kind==ArgumentKind::Texture) {
        auto t=std::find_if(binding.textures.begin(),binding.textures.end(),[&](const auto& t){return t.index==index;});
        if(t==binding.textures.end())return Fail("Missing shader texture");
        const Surface& surface=*View(t->view);
        if(surface.kind!=a.texture_kind||(surface.samples>1)!=a.multisampled)return Fail("Shader texture shape mismatch");
        if((a.writable&&!surface.storage)||(!a.writable&&!surface.sampled))return Fail("Shader texture access differs from storage usage");
      } else if(!ss.contains(index))return Fail("Missing shader sampler");
    }
    return true;
  }
  bool DrawValid(const Draw& d,const Pass& pass) {
    if(++draws>limits.max_draws)return Fail("Frame draw limit exceeded");
    auto pi=pipelines.find(d.pipeline);if(pi==pipelines.end())return Fail("Unknown draw pipeline");
    const Pipeline& p=*pi->second;
    if(p.samples!=pass.samples)return Fail("Pipeline sample count differs from pass");
    for(size_t i=0;i<4;++i)if(p.colors[i]!=(pass.colors[i]?ViewFormat(pass.colors[i]->view):Format::Invalid))return Fail("Pipeline color format differs from pass");
    if(p.depth!=(pass.depth?ViewFormat(pass.depth->view):Format::Invalid)||p.stencil!=(pass.stencil?ViewFormat(pass.stencil->view):Format::Invalid))return Fail("Pipeline depth/stencil format differs from pass");
    const auto& v=d.viewport;
    if(!Enum(d.primitive)||!Enum(d.cull)||!d.instances||!RectInside(d.scissor,pass.width,pass.height)||
       !Finite(std::array{v.x,v.y,v.width,v.height,v.near_z,v.far_z})||v.width<=0||v.height<=0||
       v.near_z<0||v.near_z>1||v.far_z<0||v.far_z>1||!Finite(d.blend_color)||
       !std::isfinite(d.depth_bias)||!std::isfinite(d.slope_bias)||d.stencil_front>255||d.stencil_back>255)return Fail("Invalid dynamic draw state");
    if(d.indices) {
      if((d.index_bytes!=2&&d.index_bytes!=4)||!BufferRange(*d.indices,uint64_t(d.index_count)*d.index_bytes,d.index_bytes))return Fail("Index view is out of bounds or misaligned");
    } else if(d.index_count)return Fail("Index count has no index buffer");
    if(!BindingValid(*shaders.at(p.vertex),d.bindings[0],&pass))return false;
    return !p.fragment||BindingValid(*shaders.at(p.fragment),d.bindings[1],&pass);
  }
  bool PassValid(const Pass& p) {
    if(++passes>limits.max_passes||!p.width||!p.height||p.width>limits.max_dimension||p.height>limits.max_dimension||!PowerOfTwo(p.samples)||p.samples>8)return Fail("Invalid pass extent or sample count");
    bool attached=false;std::set<SurfaceView> colors;
    for(const auto& a:p.colors)if(a) {
      attached=true;if(!AttachmentValid(*a,p,0)||!colors.insert(a->view).second)return Fail("Invalid or repeated color attachment");
    }
    if(p.depth){attached=true;if(!AttachmentValid(*p.depth,p,1))return Fail("Invalid depth attachment");}
    if(p.stencil){attached=true;if(!AttachmentValid(*p.stencil,p,2))return Fail("Invalid stencil attachment");}
    if(p.depth&&p.stencil&&p.depth->view!=p.stencil->view)return Fail("Separate depth/stencil storage is not supported by this contract");
    if(!attached)return Fail("Attachmentless pass requires an explicit lowering");
    for(const auto& command:p.commands) {
      if(const auto* draw=std::get_if<Draw>(&command)){if(!DrawValid(*draw,p))return false;continue;}
      const auto& c=std::get<RectClear>(command);
      if(!RectInside(c.rectangle,p.width,p.height)||c.colors>15||
         !std::isfinite(c.depth_value)||c.depth_value<0||c.depth_value>1||c.stencil_value>255)return Fail("Invalid rectangular clear");
      for(size_t i=0;i<4;++i)if(!Finite(c.color[i])||((c.colors&(1u<<i))&&!p.colors[i]))return Fail("Clear refers to an absent color attachment");
      if((c.depth&&!p.depth)||(c.stencil&&!p.stencil))return Fail("Clear refers to an absent depth/stencil attachment");
    }
    return true;
  }
  bool Run() {
    error.clear();if(!frame.sequence)return Fail("Frame sequence must be nonzero");
    if(!Add(frame.surfaces,surfaces)||!Add(frame.buffers,buffers)||!Add(frame.buffer_storage,buffer_storage)||!Add(frame.shaders,shaders)||!Add(frame.pipelines,pipelines)||!Add(frame.samplers,samplers))return false;
    uint64_t upload_bytes=0;
    for(const auto& b:frame.buffers) {
      if(!b.bytes||b.bytes->empty()||b.bytes->size()>limits.max_buffer_bytes||b.bytes->size()>limits.max_upload_bytes-upload_bytes)return Fail("Invalid immutable buffer owner or upload budget");
      upload_bytes+=b.bytes->size();
    }
    for(const auto& b:frame.buffer_storage)if(!b.owner||!b.size||b.size>limits.max_buffer_bytes)return Fail("Invalid mutable buffer storage");
    for(const auto& s:frame.surfaces) {
      const auto f=Describe(s.format);
      const uint32_t biggest=std::max({s.width,s.height,s.depth});
      uint32_t max_levels=0;for(uint32_t n=biggest;n;n>>=1)++max_levels;
      if(!s.owner||!f.bytes||s.format==Format::Stencil8View||!Enum(s.kind)||!s.width||!s.height||!s.depth||!s.layers||!s.levels||s.levels>max_levels||
         biggest>limits.max_dimension||s.layers>2048||!PowerOfTwo(s.samples)||s.samples>8||
         (s.samples>1&&(s.levels!=1||s.kind!=TextureKind::TwoD||!s.renderable))||
         (s.kind!=TextureKind::ThreeD&&s.depth!=1)||
         ((s.kind==TextureKind::TwoD||s.kind==TextureKind::ThreeD)&&s.layers!=1)||
         ((s.kind==TextureKind::Cube||s.kind==TextureKind::CubeArray)&&(s.width!=s.height||s.layers%6!=0))||
         (s.kind==TextureKind::Cube&&s.layers!=6)||
         (s.renderable&&f.block_width!=1)||(s.storage&&(f.block_width!=1||!f.color||s.samples!=1)))return Fail("Invalid surface declaration");
    }
    for(const auto& s:frame.shaders) {
      if(!Enum(s.stage)||s.entry.empty()||s.msl.empty()==(!s.library||s.library->empty()))return Fail("Shader needs one code representation and an entry point");
      std::array<std::set<uint32_t>,3> slots;
      for(const auto& a:s.arguments) {
        uint32_t capacity=a.kind==ArgumentKind::Buffer?limits.max_buffer_slots:(a.kind==ArgumentKind::Texture?limits.max_texture_slots:limits.max_sampler_slots);
        if(!Enum(a.kind)||!a.count||a.index>=capacity||a.count>capacity-a.index||!PowerOfTwo(a.alignment)||!Enum(a.texture_kind))return Fail("Invalid reflected shader argument");
        for(uint32_t i=0;i<a.count;++i)if(!slots[size_t(a.kind)].insert(a.index+i).second)return Fail("Overlapping reflected shader slots");
      }
      std::set<uint32_t> constants;
      for(const auto& c:s.constants)if(!Enum(c.type)||!constants.insert(c.index).second||
          (c.type==Shader::ConstantType::Bool&&c.bits>1))return Fail("Invalid shader function constant");
    }
    for(const auto& p:frame.pipelines) {
      auto vertex=shaders.find(p.vertex),fragment=shaders.find(p.fragment);
      if(vertex==shaders.end()||vertex->second->stage!=Stage::Vertex||
         (p.fragment&&(fragment==shaders.end()||fragment->second->stage!=Stage::Fragment))||
         !PowerOfTwo(p.samples)||p.samples>8||!Enum(p.depth_compare)||
         (p.depth_write&&!Describe(p.depth).depth)||(p.stencil_enabled&&!Describe(p.stencil).stencil))return Fail("Invalid pipeline stages or depth state");
      for(size_t i=0;i<4;++i) {
        auto f=Describe(p.colors[i]);const auto& b=p.blend[i];
        if((p.colors[i]!=Format::Invalid&&(!f.color||f.block_width!=1))||b.write_mask>15||!Enum(b.source_rgb)||!Enum(b.source_alpha)||!Enum(b.destination_rgb)||!Enum(b.destination_alpha)||!Enum(b.rgb)||!Enum(b.alpha))return Fail("Invalid pipeline color/blend state");
      }
      if((p.depth!=Format::Invalid&&!Describe(p.depth).depth)||(p.stencil!=Format::Invalid&&!Describe(p.stencil).stencil))return Fail("Invalid pipeline attachment format");
      for(const auto& s:{p.front,p.back})if(!Enum(s.compare)||!Enum(s.fail)||!Enum(s.depth_fail)||!Enum(s.pass)||s.read_mask>255||s.write_mask>255)return Fail("Invalid stencil state");
    }
    for(const auto& s:frame.samplers)if(!Enum(s.min)||!Enum(s.mag)||!Enum(s.mip)||!Enum(s.u)||!Enum(s.v)||!Enum(s.w)||!Enum(s.border)||!std::isfinite(s.min_lod)||!std::isfinite(s.max_lod)||s.min_lod<0||s.max_lod<s.min_lod||!s.anisotropy||s.anisotropy>16)return Fail("Invalid sampler state");
    for(const auto& command:frame.commands) {
      if(const auto* pass=std::get_if<Pass>(&command)){if(!PassValid(*pass))return false;continue;}
      if(const auto* d=std::get_if<Dispatch>(&command)) {
        auto shader=shaders.find(d->shader);
        if(shader==shaders.end()||shader->second->stage!=Stage::Compute||!BindingValid(*shader->second,d->bindings,nullptr))return Fail("Invalid compute dispatch bindings");
        uint64_t threads=1;
        for(size_t i=0;i<3;++i){if(!d->groups[i]||d->groups[i]>(1u<<20)||!d->threads[i]||d->threads[i]>1024)return Fail("Invalid compute dispatch extent");threads*=d->threads[i];}
        if(threads>1024)return Fail("Compute threadgroup exceeds portable limit");
        continue;
      }
      if(const auto* u=std::get_if<BufferUpload>(&command)) {
        if(!buffer_storage.contains(u->destination.buffer)||!u->bytes||u->bytes->empty()||
           !BufferRange(u->destination,u->bytes->size())||u->bytes->size()>limits.max_upload_bytes-upload_bytes)return Fail("Invalid mutable buffer upload");
        upload_bytes+=u->bytes->size();continue;
      }
      if(const auto* u=std::get_if<TextureUpload>(&command)) {
        const Surface* s=View(u->destination);if(!s||s->samples!=1||!u->bytes||u->bytes->empty())return Fail("Invalid texture upload");
        const auto f=Describe(ViewFormat(u->destination));if(!f.color)return Fail("Depth/stencil CPU uploads need explicit format lowering");
        const uint64_t w=std::max(1u,s->width>>u->destination.level),h=std::max(1u,s->height>>u->destination.level);
        const uint64_t row=((w+f.block_width-1)/f.block_width)*f.bytes,rows=(h+f.block_height-1)/f.block_height;
        if(u->bytes_per_row<row||u->bytes_per_row%f.bytes||u->bytes_per_row>limits.max_upload_bytes/rows||
           u->bytes_per_image<u->bytes_per_row*rows||u->bytes_per_image>u->bytes->size()||u->bytes->size()>limits.max_upload_bytes-upload_bytes)return Fail("Texture upload pitch or payload is invalid");
        upload_bytes+=u->bytes->size();continue;
      }
      if(const auto* c=std::get_if<BufferToTexture>(&command)) {
        const Surface* s=View(c->destination);
        if(!s || s->samples!=1 || !c->width || !c->height || !c->depth)return Fail("Invalid GPU texture copy destination");
        const auto format=Describe(ViewFormat(c->destination));
        if(!format.color)return Fail("GPU texture copy needs an explicit depth/stencil aspect conversion");
        const uint32_t w=std::max(1u,s->width>>c->destination.level),h=std::max(1u,s->height>>c->destination.level);
        const uint32_t d=s->kind==TextureKind::ThreeD?std::max(1u,s->depth>>c->destination.level):1;
        if(!RectInside({c->destination_x,c->destination_y,c->width,c->height},w,h) ||
           c->depth>d-c->destination.plane || c->destination_x%format.block_width || c->destination_y%format.block_height ||
           (c->width%format.block_width && c->destination_x+c->width!=w) ||
           (c->height%format.block_height && c->destination_y+c->height!=h))return Fail("GPU texture copy exceeds destination bounds or blocks");
        const uint64_t row=((uint64_t(c->width)+format.block_width-1)/format.block_width)*format.bytes;
        const uint64_t rows=(uint64_t(c->height)+format.block_height-1)/format.block_height;
        if(c->source.offset%format.bytes || c->bytes_per_row<row || c->bytes_per_row%format.bytes ||
           c->bytes_per_row>uint64_t(limits.max_dimension)*format.bytes ||
           c->bytes_per_row>limits.max_buffer_bytes/rows)return Fail("GPU texture copy pitch or alignment is invalid");
        const uint64_t one=c->bytes_per_row*(rows-1)+row;
        uint64_t required=one;
        if(c->depth>1) {
          if(c->bytes_per_image<c->bytes_per_row*rows || c->bytes_per_image%format.bytes ||
             c->bytes_per_image>(limits.max_buffer_bytes-one)/(c->depth-1))return Fail("GPU volume texture stride exceeds its buffer");
          required+=c->bytes_per_image*(c->depth-1);
        }
        if(!BufferRange(c->source,required))return Fail("GPU texture copy exceeds its source buffer view");
        continue;
      }
      if(const auto* c=std::get_if<BufferCopy>(&command)) {
        if(!c->size || !buffer_storage.contains(c->destination.buffer) ||
           !BufferRange(c->source,c->size) || !BufferRange(c->destination,c->size) ||
           (c->source.buffer==c->destination.buffer && c->source.offset<c->destination.offset+c->size &&
             c->destination.offset<c->source.offset+c->size))return Fail("Invalid or overlapping GPU buffer copy");
        continue;
      }
      SurfaceView source,destination;
      if(const auto* c=std::get_if<ImageCopy>(&command)){source=c->source;destination=c->destination;}
      else {const auto& r=std::get<Resolve>(command);source=r.source;destination=r.destination;}
      const Surface *s=View(source),*d=View(destination);
      if(!s||!d||source.surface==destination.surface||ViewFormat(source)!=ViewFormat(destination))return Fail("Invalid or reinterpreting image transfer");
      const uint32_t sw=std::max(1u,s->width>>source.level),sh=std::max(1u,s->height>>source.level),dw=std::max(1u,d->width>>destination.level),dh=std::max(1u,d->height>>destination.level);
      if(const auto* c=std::get_if<ImageCopy>(&command)) {
        const auto f=Describe(ViewFormat(source));
        if(s->samples!=1||d->samples!=1||!c->width||!c->height||!RectInside({c->source_x,c->source_y,c->width,c->height},sw,sh)||!RectInside({c->destination_x,c->destination_y,c->width,c->height},dw,dh)||
           c->source_x%f.block_width||c->destination_x%f.block_width||c->source_y%f.block_height||c->destination_y%f.block_height||
           (c->width%f.block_width&&(c->source_x+c->width!=sw||c->destination_x+c->width!=dw))||
           (c->height%f.block_height&&(c->source_y+c->height!=sh||c->destination_y+c->height!=dh)))return Fail("Image copy bounds, blocks or sample count mismatch");
      } else {
        const auto& r=std::get<Resolve>(command);const auto f=Describe(ViewFormat(source));
        if(!Enum(r.filter)||s->samples<=1||d->samples!=1||!s->renderable||!d->renderable||sw!=dw||sh!=dh||
           (f.color&&r.filter!=ResolveFilter::Average)||(f.depth&&r.filter==ResolveFilter::Average))return Fail("Resolve requires an exact supported native filter");
      }
    }
    if(frame.output) {
      const Surface* s=View(*frame.output);if(!s||s->samples!=1||!Describe(ViewFormat(*frame.output)).color||s->kind!=TextureKind::TwoD)return Fail("Invalid presentable frame output");
    }
    return true;
  }
};
bool Joinable(const std::optional<Attachment>& a,const std::optional<Attachment>& b) {
  if(bool(a)!=bool(b))return false;
  return !a||(a->view==b->view&&a->store==Store::Store&&b->load==Load::Load);
}
}
bool Validate(const Frame& frame,std::string& error,const Limits& limits) {return Admission{frame,limits,error}.Run();}
void AppendPass(Frame& frame,Pass next) {
  Pass* previous=frame.commands.empty()?nullptr:std::get_if<Pass>(&frame.commands.back());
  bool join=previous&&previous->mergeable&&next.mergeable&&previous->width==next.width&&previous->height==next.height&&previous->samples==next.samples&&Joinable(previous->depth,next.depth)&&Joinable(previous->stencil,next.stencil);
  if(join)for(size_t i=0;i<4;++i)join&=Joinable(previous->colors[i],next.colors[i]);
  if(!join){frame.commands.emplace_back(std::move(next));return;}
  for(size_t i=0;i<4;++i)if(previous->colors[i])previous->colors[i]->store=next.colors[i]->store;
  if(previous->depth)previous->depth->store=next.depth->store;
  if(previous->stencil)previous->stencil->store=next.stencil->store;
  previous->commands.insert(previous->commands.end(),std::make_move_iterator(next.commands.begin()),std::make_move_iterator(next.commands.end()));
}
} // namespace rex::native
