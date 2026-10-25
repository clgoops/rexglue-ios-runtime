// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex_shared_memory.h"
#include <rex/graphics/primitive_processor.h>
namespace rex::native {
class RexPrimitives final:public graphics::PrimitiveProcessor {
 public:
  RexPrimitives(const graphics::RegisterFile&,memory::Memory&,RexSharedMemory&,std::function<Identity()>);
  ~RexPrimitives() override;
  bool Initialize();
  void BeginNativeFrame(Frame&);
  void EndNativeFrame();
  bool Bind(const ProcessingResult&,Draw&,std::string&);
 protected:
  bool InitializeBuiltinIndexBuffer(size_t,std::function<void(void*)>) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(graphics::xenos::IndexFormat,uint32_t,bool,uint32_t,size_t&) override;
 private:
  struct Converted {Buffer buffer;std::shared_ptr<std::vector<std::byte>> bytes;uint64_t offset=0,length=0;};
  RexSharedMemory& shared_;
  std::function<Identity()> next_;
  Frame* frame_=nullptr;
  std::optional<Buffer> builtin_;
  std::vector<Converted> converted_;
};
} // namespace rex::native
