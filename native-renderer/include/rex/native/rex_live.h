// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "metal_backend.h"
#include <rex/graphics/graphics_system.h>
#include <atomic>
#include <map>
#include <mutex>

namespace rex::native {
template<class T> void Declare(std::vector<T>& list,const T& value) {
  if(std::none_of(list.begin(),list.end(),[&](const T& item){return item.id==value.id;}))list.push_back(value);
}
// One device and ordered queue are shared by guest work and UI composition.
// Sequence numbers are allocated under the submission lock, not at recording.
class NativeService:public std::enable_shared_from_this<NativeService> {
 public:
  NativeService();
  Identity Next() {return {next_.fetch_add(1),1};}
  bool Submit(std::shared_ptr<Frame>,std::string&,std::function<void()> completed_callback={});
  bool Drain(std::string&);
  void Fail(const std::string&);
  std::mutex mutex;
  MetalBackend backend;
  std::atomic<uint64_t> completed{0};
  std::atomic<bool> failed{false};
 private:
  std::atomic<uint64_t> next_{1};
  uint64_t sequence_=0;
};
class NativeRefreshContext:public ui::Presenter::GuestOutputRefreshContext {
 public:
  NativeRefreshContext(bool& eight,Surface& output):GuestOutputRefreshContext(eight),output(output) {}
  Surface& output;
};
class NativeUIContext:public ui::UIDrawContext {
 public:
  NativeUIContext(ui::Presenter& p,Frame& f,Pass& pass):UIDrawContext(p,pass.width,pass.height),frame(f),pass(pass) {}
  Frame& frame;
  Pass& pass;
};
class NativeGraphicsSystem final:public graphics::GraphicsSystem {
 public:
  std::string name() const override {return "Xenos native Metal";}
 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<graphics::CommandProcessor> CreateCommandProcessor() override;
 private:
  std::shared_ptr<NativeService> service_;
};
std::unique_ptr<ui::GraphicsProvider> CreateNativeProvider(std::shared_ptr<NativeService>);
std::unique_ptr<graphics::CommandProcessor> CreateNativeCommandProcessor(graphics::GraphicsSystem*,system::KernelState*,std::shared_ptr<NativeService>);
} // namespace rex::native
