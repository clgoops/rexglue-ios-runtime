// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_ui.h"
#include <algorithm>
#include <cstring>
namespace rex::native {
namespace {
template<class T> void Declare(std::vector<T>& list,const T& value) {
  if(std::none_of(list.begin(),list.end(),[&](const T& item){return item.id==value.id;}))list.push_back(value);
}
Surface UITarget(const std::function<Identity()>& next,uint32_t w,uint32_t h) {
  Surface s;s.id=next();s.width=w;s.height=h;s.format=Format::RGBA8;s.renderable=true;
  s.owner=std::make_shared<const Identity>(s.id);return s;
}
Bytes CopyBytes(const void* data,size_t size) {
  auto bytes=std::make_shared<std::vector<std::byte>>(size);if(size)std::memcpy(bytes->data(),data,size);return bytes;
}
class NativeTexture final:public ui::ImmediateTexture {
 public:
  NativeTexture(uint32_t w,uint32_t h):ImmediateTexture(w,h) {}
  Surface image;Bytes data;Sampler sampler;bool uploaded=false;
};
constexpr const char* kUIVertex=R"(#include <metal_stdlib>
using namespace metal;
struct V {packed_float2 xy;packed_float2 uv;uint color;};
struct O {float4 position [[position]];float2 uv;float4 color;};
vertex O ui_vertex(uint i [[vertex_id]],const device V* v [[buffer(0)]],constant float4& p [[buffer(1)]]) {
 O o;o.position=float4(float2(v[i].xy)*p.xy+p.zw,0,1);o.uv=v[i].uv;
 o.color=float4(v[i].color&255u,(v[i].color>>8)&255u,(v[i].color>>16)&255u,v[i].color>>24)/255.0;return o;
})";
constexpr const char* kUIFragment=R"(#include <metal_stdlib>
using namespace metal;
struct O {float4 position [[position]];float2 uv;float4 color;};
fragment float4 ui_fragment(O in [[stage_in]],texture2d<float> image [[texture(0)]],sampler s [[sampler(0)]]) {
 return in.color*image.sample(s,in.uv);
})";
class NativeImmediateDrawer final:public ui::ImmediateDrawer {
 public:
  explicit NativeImmediateDrawer(std::function<Identity()> next):next_(std::move(next)) {
    vs_.id=next_();vs_.stage=Stage::Vertex;vs_.entry="ui_vertex";vs_.msl=kUIVertex;
    vs_.arguments={{ArgumentKind::Buffer,0,1,20,4},{ArgumentKind::Buffer,1,1,16,16}};
    fs_.id=next_();fs_.stage=Stage::Fragment;fs_.entry="ui_fragment";fs_.msl=kUIFragment;
    fs_.arguments={{ArgumentKind::Texture,0},{ArgumentKind::Sampler,0}};
    const uint32_t white=0xffffffff;white_=CreateTexture(1,1,ui::ImmediateTextureFilter::kNearest,false,reinterpret_cast<const uint8_t*>(&white));
  }
  std::unique_ptr<ui::ImmediateTexture> CreateTexture(uint32_t w,uint32_t h,ui::ImmediateTextureFilter filter,bool repeat,const uint8_t* data) override {
    if(!w||!h||!data||uint64_t(w)*h>16384ull*16384)return {};
    auto texture=std::make_unique<NativeTexture>(w,h);texture->image=UITarget(next_,w,h);texture->image.renderable=false;
    texture->data=CopyBytes(data,uint64_t(w)*h*4);texture->sampler.id=next_();
    texture->sampler.min=texture->sampler.mag=filter==ui::ImmediateTextureFilter::kLinear?Filter::Linear:Filter::Nearest;
    texture->sampler.u=texture->sampler.v=repeat?Address::Repeat:Address::Clamp;return texture;
  }
  void Begin(ui::UIDrawContext& c,float w,float h) override {
    ImmediateDrawer::Begin(c,w,h);context_=&static_cast<NativeUIContext&>(c);
    auto& frame=context_->frame;Declare(frame.shaders,vs_);Declare(frame.shaders,fs_);
    Pipeline p;p.vertex=vs_.id;p.fragment=fs_.id;p.colors[0]=Format::RGBA8;
    p.blend[0].enabled=true;p.blend[0].source_rgb=BlendFactor::SourceAlpha;p.blend[0].destination_rgb=BlendFactor::OneMinusSourceAlpha;
    p.blend[0].source_alpha=BlendFactor::One;p.blend[0].destination_alpha=BlendFactor::OneMinusSourceAlpha;
    if(!pipeline_) {p.id=next_();pipeline_=p;}Declare(frame.pipelines,*pipeline_);
    std::array<float,4> projection={2/coordinate_space_width(),-2/coordinate_space_height(),-1,1};
    projection_={next_(),CopyBytes(projection.data(),sizeof(projection)),true};frame.buffers.push_back(projection_);
  }
  void BeginDrawBatch(const ui::ImmediateDrawBatch& batch) override {
    vertex_={};index_={};vertices_=std::max(0,batch.vertex_count);indices_=std::max(0,batch.index_count);
    if(!context_||!batch.vertices||!vertices_)return;
    vertex_={next_(),CopyBytes(batch.vertices,size_t(vertices_)*sizeof(ui::ImmediateVertex)),false};context_->frame.buffers.push_back(vertex_);
    if(indices_&&batch.indices) {index_={next_(),CopyBytes(batch.indices,size_t(indices_)*2),false};context_->frame.buffers.push_back(index_);}
  }
  void Draw(const ui::ImmediateDraw& in) override {
    if(!context_||!vertex_.id||in.count<=0)return;
    auto& frame=context_->frame;auto* texture=static_cast<NativeTexture*>(in.texture?in.texture:white_.get());if(!texture)return;
    Declare(frame.surfaces,texture->image);Declare(frame.samplers,texture->sampler);
    if(!texture->uploaded) {frame.commands.push_back(TextureUpload{{texture->image.id},texture->data,uint64_t(texture->width)*4,uint64_t(texture->width)*texture->height*4});texture->uploaded=true;}
    rex::native::Draw draw;draw.pipeline=pipeline_->id;draw.viewport={0,0,double(context_->pass.width),double(context_->pass.height),0,1};
    if(!ScissorToRenderTarget(in,draw.scissor.x,draw.scissor.y,draw.scissor.width,draw.scissor.height))return;
    draw.primitive=in.primitive_type==ui::ImmediatePrimitiveType::kLines?Primitive::Lines:Primitive::Triangles;
    draw.bindings[0].buffers={{0,{vertex_.id,0,vertex_.bytes->size()}},{1,{projection_.id,0,projection_.bytes->size()}}};
    draw.bindings[1].textures={{0,{texture->image.id}}};draw.bindings[1].samplers={{0,texture->sampler.id}};
    if(index_.id) {
      if(in.index_offset<0||uint64_t(in.index_offset)+in.count>uint64_t(indices_))return;
      draw.indices=BufferView{index_.id,uint64_t(in.index_offset)*2,uint64_t(in.count)*2};draw.index_count=in.count;draw.base_vertex=in.base_vertex;
    } else {if(in.base_vertex<0||uint64_t(in.base_vertex)+in.count>uint64_t(vertices_))return;draw.first_vertex=in.base_vertex;draw.vertices=in.count;}
    context_->pass.commands.push_back(std::move(draw));
  }
  void EndDrawBatch() override {vertex_={};index_={};}
  void End() override {context_=nullptr;ImmediateDrawer::End();}
 private:
  std::function<Identity()> next_;NativeUIContext* context_=nullptr;
  Shader vs_,fs_;std::optional<Pipeline> pipeline_;Buffer vertex_,index_,projection_;int vertices_=0,indices_=0;
  std::unique_ptr<ui::ImmediateTexture> white_;
};
}
std::unique_ptr<ui::ImmediateDrawer> CreateNativeImmediateDrawer(std::function<Identity()> next) {
  return std::make_unique<NativeImmediateDrawer>(std::move(next));
}
} // namespace rex::native
