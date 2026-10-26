// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_live.h"
#include <rex/graphics/command_processor.h>
#include <rex/ui/surface_mac.h>
#include <rex/logging.h>
#include <cstring>
#include <chrono>

namespace rex::native {
namespace {
BackendOptions Options() {BackendOptions o;o.surface_budget=768ull<<20;o.mutable_buffer_budget=768ull<<20;o.letterbox=REXCVAR_GET(present_letterbox);return o;}
Surface Target(NativeService& service,uint32_t w,uint32_t h) {
  Surface s;s.id=service.Next();s.width=w;s.height=h;s.format=Format::RGBA8;s.renderable=true;
  s.owner=std::make_shared<const Identity>(s.id);return s;
}
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
    if(ui&&!ui_failed_) {
      if(!canvas_||canvas_->width!=image.width||canvas_->height!=image.height)canvas_=Target(*service_,image.width,image.height);
      Declare(frame->surfaces,*canvas_);frame->commands.push_back(ImageCopy{{image.id},{canvas_->id},0,0,0,0,image.width,image.height});
      Pass pass;pass.width=image.width;pass.height=image.height;pass.colors[0]=Attachment{{canvas_->id}};
      NativeUIContext context(*this,*frame,pass);ExecuteUIDrawersFromUIThread(context);if(!pass.commands.empty())AppendPass(*frame,std::move(pass));frame->output=SurfaceView{canvas_->id};
    } else frame->output=SurfaceView{image.id};
    std::string error;if(service_->Submit(frame,error))return PaintResult::kPresented;
    // Overlay admission/compilation is independent of guest GPU coherence.
    // Preserve the completed guest image if an optional UI frame is rejected.
    if(ui&&!ui_failed_&&!service_->failed) {
      ui_failed_=true;REXLOG_ERROR("Native UI composition rejected; retaining game presentation: {}",error);rex::FlushLogging();
      auto fallback=std::make_shared<Frame>();fallback->surfaces.push_back(image);fallback->output=SurfaceView{image.id};
      if(service_->Submit(std::move(fallback),error))return PaintResult::kPresented;
    }
    service_->Fail(error);return PaintResult::kNotPresented;
  }
 private:
  std::shared_ptr<NativeService> service_;std::mutex mailbox_mutex_;std::array<Surface,kGuestOutputMailboxSize> mailbox_;std::optional<Surface> canvas_;bool ui_failed_=false;
};
class NativeProvider final:public ui::GraphicsProvider {
 public:
  explicit NativeProvider(std::shared_ptr<NativeService> s):service_(std::move(s)) {}
  std::unique_ptr<ui::Presenter> CreatePresenter(ui::Presenter::HostGpuLossCallback cb) override {
    auto p=std::make_unique<NativePresenter>(service_,std::move(cb));if(!p->Initialize())return {};return p;
  }
  std::unique_ptr<ui::ImmediateDrawer> CreateImmediateDrawer() override {return CreateNativeImmediateDrawer([s=service_]{return s->Next();});}
 private:std::shared_ptr<NativeService> service_;
};
}
NativeService::NativeService():backend(Options()) {}
void NativeService::Fail(const std::string& error) {if(!failed.exchange(true))REXLOG_ERROR("Native Metal session stopped: {}",error);}
bool NativeService::Submit(std::shared_ptr<Frame> frame,std::string& error,std::function<void()> completed_callback) {
  std::lock_guard lock(mutex);if(failed) {error="native Metal session has failed";return false;}frame->sequence=++sequence_;
  const bool guest=bool(completed_callback);const auto before=backend.Stats();
  auto weak=weak_from_this();bool accepted=backend.Submit(std::move(frame),[weak,done=std::move(completed_callback)](Completion result) {
    if(auto self=weak.lock()) {if(!result.success)self->Fail(result.error);self->completed.store(result.sequence);
      if(done&&result.timing_available) {self->performance.SetGpuTimingSupported(true);self->performance.RecordGpuSubmission(uint64_t(result.native_window_ms*1e6));}}
    if(result.success&&done)done();
  },error);
  if(accepted&&guest) {
    const auto after=backend.Stats();performance.RecordRenderAreas(after.passes-before.passes,after.reduced_passes-before.reduced_passes,
      after.render_area_pixels-before.render_area_pixels,after.render_area_used_pixels-before.render_area_used_pixels);
  }
  return accepted;
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
