// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "spirv.h"
#include <rex/graphics/shared_memory.h>

namespace rex::native {
// Keeps ReXGlue's physical-page watches and dirty-range rules, but realizes
// uploads as ordered native frame commands. All methods except the inherited
// invalidation callback run on the command-processor thread.
class RexSharedMemory final:public graphics::SharedMemory {
 public:
  explicit RexSharedMemory(memory::Memory&);
  // One, two or four equal chunks, matching the translator's selected shared
  // memory descriptor count and the native device's maximum buffer length.
  bool Initialize(std::span<const Identity> chunks,std::string& error);
  bool BeginFrame(Frame&,std::string& error);
  // A rejected frame cannot leave pages marked valid without uploading them.
  // Fail the renderer and restart its guest session; never continue with stale
  // or discarded GPU writes. Accepted frames remain ordered on the same queue.
  void EndFrame(bool submission_accepted);
  bool faulted() const {return faulted_;}
  DescriptorResources Descriptor() const;
  std::optional<BufferView> View(uint32_t physical_address,uint32_t length) const;
  // Joins a range spanning native chunks with ordered GPU copies. No CPU
  // readback can replace newer GPU resolve or memexport contents.
  std::optional<BufferView> ContiguousView(uint32_t address,uint32_t length,
      const std::function<Identity()>&,std::string&);
  bool CopyBack(uint32_t address,uint32_t length,BufferView source,std::string&);
 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t,uint32_t>>&) override;
 private:
  std::vector<BufferStorage> chunks_;
  Frame* frame_=nullptr;
  uint32_t chunk_size_=0;
  bool initialized_=false,faulted_=false;
};
} // namespace rex::native
