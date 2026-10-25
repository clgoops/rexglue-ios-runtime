// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_live.h"
#include <rex/graphics/command_processor.h>
#include <rex/ui/surface_mac.h>
#include <rex/logging.h>
#include <cstring>
#include <chrono>

namespace rex::native {
namespace {
BackendOptions Options() {BackendOptions o;o.mutable_buffer_budget=768ull<<20;o.letterbox=REXCVAR_GET(present_letterbox);return o;}
Bytes CopyBytes(const void* data,size_t size) {
  auto bytes=std::make_shared<std::vector<std::byte>>(size);if(size)std::memcpy(bytes->data(),data,size);return bytes;
}
Surface Target(NativeService& service,uint32_t w,uint32_t h) {
  Surface s;s.id=service.Next();s.width=w;s.height=h;s.format=Format::RGBA8;s.renderable=true;
  s.owner=std::make_shared<const Identity>(s.id);return s;
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
  explicit NativeImmediateDrawer(std::shared_ptr<NativeService> s):service_(std::move(s)) {
    vs_.id=service_->Next();vs_.stage=Stage::Vertex;vs_.entry="ui_vertex";vs_.msl=kUIVertex;
    vs_.arguments={{ArgumentKind::Buffer,0,1,20,4},{ArgumentKind::Buffer,1,1,16,16}};
    fs_.id=service_->Next();fs_.stage=Stage::Fragment;fs_.entry="ui_fragment";fs_.msl=kUIFragment;
    fs_.arguments={{ArgumentKind::Texture,0},{ArgumentKind::Sampler,0}};
    const uint32_t white=0xffffffff;white_=CreateTexture(1,1,ui::ImmediateTextureFilter::kNearest,false,reinterpret_cast<const uint8_t*>(&white));
  }
  std::unique_ptr<ui::ImmediateTexture> CreateTexture(uint32_t w,uint32_t h,ui::ImmediateTextureFilter filter,bool repeat,const uint8_t* data) override {
    if(!w||!h||!data||uint64_t(w)*h>16384ull*16384)return {};
    auto texture=std::make_unique<NativeTexture>(w,h);texture->image=Target(*service_,w,h);texture->image.renderable=false;
    texture->data=CopyBytes(data,uint64_t(w)*h*4);texture->sampler.id=service_->Next();
    texture->sampler.min=texture->sampler.mag=filter==ui::ImmediateTextureFilter::kLinear?Filter::Linear:Filter::Nearest;
    texture->sampler.u=texture->sampler.v=repeat?Address::Repeat:Address::Clamp;return texture;
  }
  void Begin(ui::UIDrawContext& c,float w,float h) override {
    ImmediateDrawer::Begin(c,w,h);context_=&static_cast<NativeUIContext&>(c);
    auto& frame=context_->frame;Declare(frame.shaders,vs_);Declare(frame.shaders,fs_);
    Pipeline p;p.vertex=vs_.id;p.fragment=fs_.id;p.colors[0]=Format::RGBA8;
    p.blend[0].enabled=true;p.blend[0].source_rgb=BlendFactor::SourceAlpha;p.blend[0].destination_rgb=BlendFactor::OneMinusSourceAlpha;
    p.blend[0].source_alpha=BlendFactor::One;p.blend[0].destination_alpha=BlendFactor::OneMinusSourceAlpha;
    if(!pipeline_) {p.id=service_->Next();pipeline_=p;}Declare(frame.pipelines,*pipeline_);
    std::array<float,4> projection={2/coordinate_space_width(),-2/coordinate_space_height(),-1,1};
    projection_={service_->Next(),CopyBytes(projection.data(),sizeof(projection)),true};frame.buffers.push_back(projection_);
  }
  void BeginDrawBatch(const ui::ImmediateDrawBatch& batch) override {
    vertex_={};index_={};vertices_=std::max(0,batch.vertex_count);indices_=std::max(0,batch.index_count);
    if(!context_||!batch.vertices||!vertices_)return;
    vertex_={service_->Next(),CopyBytes(batch.vertices,size_t(vertices_)*sizeof(ui::ImmediateVertex)),false};context_->frame.buffers.push_back(vertex_);
    if(indices_&&batch.indices) {index_={service_->Next(),CopyBytes(batch.indices,size_t(indices_)*2),false};context_->frame.buffers.push_back(index_);}
  }
  void Draw(const ui::ImmediateDraw& in) override {
    if(!context_||!vertex_.id||in.count<=0)return;
    auto& frame=context_->frame;auto* texture=static_cast<NativeTexture*>(in.texture?in.texture:white_.get());if(!texture)return;
    Declare(frame.surfaces,texture->image);Declare(frame.samplers,texture->sampler);
    if(!texture->uploaded) {frame.commands.push_back(TextureUpload{{texture->image.id},texture->data,uint64_t(texture->width)*4,0});texture->uploaded=true;}
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
  std::shared_ptr<NativeService> service_;NativeUIContext* context_=nullptr;
  Shader vs_,fs_;std::optional<Pipeline> pipeline_;Buffer vertex_,index_,projection_;int vertices_=0,indices_=0;
  std::unique_ptr<ui::ImmediateTexture> white_;
};
class NativePresenter final:public ui::Presenter {
 public:
  NativePresenter(std::shared_ptr<NativeService> s,HostGpuLossCallback cb):Presenter(std::move(cb)),service_(std::move(s)) {}
  bool Initialize() {return InitializeCommonSurfaceIndependent();}
  ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override {return ui::Surface::kTypeFlag_CAMetalLayer;}
  bool CaptureGuestOutput(ui::RawImage& out) override {
    uint32_t index;auto lock=ConsumeGuestOutput(index,nullptr,nullptr);if(index==UINT32_MAX)return false;
    Surface image;{std::lock_guard guard(mailbox_mutex_);image=mailbox_[index];}lock.unlock();if(!image.id)return false;
    std::lock_guard guard(service_->mutex);std::string error;if(!service_->backend.Drain(std::chrono::seconds(30),error))return false;
    auto bytes=service_->backend.ReadRGBA8({image.id},error);if(bytes.empty())return false;
    out.width=image.width;out.height=image.height;out.stride=size_t(image.width)*4;out.data.resize(bytes.size());std::memcpy(out.data.data(),bytes.data(),bytes.size());return true;
  }
 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(ui::Surface& surface,uint32_t,uint32_t,bool,bool& vsync) override {
    if(surface.GetType()!=ui::Surface::kTypeIndex_CAMetalLayer)return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    auto* layer=const_cast<void*>(static_cast<ui::CAMetalLayerSurface&>(surface).layer());std::string error;std::lock_guard lock(service_->mutex);
    if(!service_->backend.AttachLayer(layer,error)) {service_->Fail(error);return SurfacePaintConnectResult::kFailure;}
    vsync=true;return SurfacePaintConnectResult::kSuccess;
  }
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override {std::string error;std::lock_guard lock(service_->mutex);service_->backend.AttachLayer(nullptr,error);}
  bool RefreshGuestOutputImpl(uint32_t index,uint32_t,uint32_t,std::function<bool(GuestOutputRefreshContext&)> refresher,bool& eight) override {
    Surface output;NativeRefreshContext context(eight,output);if(!refresher(context)||!output.id)return false;
    std::lock_guard lock(mailbox_mutex_);mailbox_[index]=std::move(output);return true;
  }
  PaintResult PaintAndPresentImpl(bool ui) override {
    if(service_->failed)return PaintResult::kNotPresented;
    uint32_t index;auto consumed=ConsumeGuestOutput(index,nullptr,nullptr);Surface image;
    if(index!=UINT32_MAX) {std::lock_guard lock(mailbox_mutex_);image=mailbox_[index];}consumed.unlock();
    if(!image.id)return PaintResult::kNotPresented;
    auto frame=std::make_shared<Frame>();Declare(frame->surfaces,image);
    if(ui) {
      if(!canvas_||canvas_->width!=image.width||canvas_->height!=image.height)canvas_=Target(*service_,image.width,image.height);
      Declare(frame->surfaces,*canvas_);frame->commands.push_back(ImageCopy{{image.id},{canvas_->id},0,0,0,0,image.width,image.height});
      Pass pass;pass.width=image.width;pass.height=image.height;pass.colors[0]=Attachment{{canvas_->id}};
      NativeUIContext context(*this,*frame,pass);ExecuteUIDrawersFromUIThread(context);if(!pass.commands.empty())AppendPass(*frame,std::move(pass));frame->output=SurfaceView{canvas_->id};
    } else frame->output=SurfaceView{image.id};
    std::string error;if(!service_->Submit(frame,error)) {service_->Fail(error);return PaintResult::kNotPresented;}return PaintResult::kPresented;
  }
 private:
  std::shared_ptr<NativeService> service_;std::mutex mailbox_mutex_;std::array<Surface,kGuestOutputMailboxSize> mailbox_;std::optional<Surface> canvas_;
};
class NativeProvider final:public ui::GraphicsProvider {
 public:
  explicit NativeProvider(std::shared_ptr<NativeService> s):service_(std::move(s)) {}
  std::unique_ptr<ui::Presenter> CreatePresenter(ui::Presenter::HostGpuLossCallback cb) override {
    auto p=std::make_unique<NativePresenter>(service_,std::move(cb));if(!p->Initialize())return {};return p;
  }
  std::unique_ptr<ui::ImmediateDrawer> CreateImmediateDrawer() override {return std::make_unique<NativeImmediateDrawer>(service_);}
 private:std::shared_ptr<NativeService> service_;
};
}
NativeService::NativeService():backend(Options()) {}
void NativeService::Fail(const std::string& error) {if(!failed.exchange(true))REXLOG_ERROR("Native Metal session stopped: {}",error);}
bool NativeService::Submit(std::shared_ptr<Frame> frame,std::string& error,std::function<void()> completed_callback) {
  std::lock_guard lock(mutex);if(failed) {error="native Metal session has failed";return false;}frame->sequence=++sequence_;
  auto weak=weak_from_this();return backend.Submit(std::move(frame),[weak,done=std::move(completed_callback)](Completion result) {
    if(auto self=weak.lock()) {if(!result.success)self->Fail(result.error);self->completed.store(result.sequence);
      if(done&&result.timing_available) {self->performance.SetGpuTimingSupported(true);self->performance.RecordGpuSubmission(uint64_t(result.native_window_ms*1e6));}}
    if(result.success&&done)done();
  },error);
}
bool NativeService::Drain(std::string& error) {std::lock_guard lock(mutex);return backend.Drain(std::chrono::seconds(30),error);}
std::unique_ptr<ui::GraphicsProvider> CreateNativeProvider(std::shared_ptr<NativeService> service) {return std::make_unique<NativeProvider>(std::move(service));}
void NativeGraphicsSystem::CreateProvider(bool presentation) {
  if(!service_)service_=std::make_shared<NativeService>();std::string error;
  if(!service_->backend.Ready(error)) {service_->Fail(error);return;}
  provider_=CreateNativeProvider(service_);provider_supports_presentation_=presentation;
}
std::unique_ptr<graphics::CommandProcessor> NativeGraphicsSystem::CreateCommandProcessor() {return CreateNativeCommandProcessor(this,kernel_state_,service_);}
void NativeGraphicsSystem::SetPerformanceMonitoringEnabled(bool enabled) {
  if(!service_)return;std::lock_guard lock(service_->mutex);service_->performance.SetEnabled(enabled);service_->backend.SetTimingEnabled(enabled);
}
graphics::PerformanceMetrics NativeGraphicsSystem::GetPerformanceMetrics() const {return service_?service_->performance.GetMetrics():graphics::PerformanceMetrics{};}
} // namespace rex::native
