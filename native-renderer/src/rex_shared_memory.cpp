// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_shared_memory.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace rex::native {
RexSharedMemory::RexSharedMemory(memory::Memory& memory):graphics::SharedMemory(memory) {}
bool RexSharedMemory::Initialize(std::span<const Identity> ids,std::string& error) {
  error.clear();
  if(initialized_ || faulted_ || (ids.size()!=1 && ids.size()!=2 && ids.size()!=4)) {
    error="invalid native shared-memory initialization";return false;
  }
  std::set<Identity> seen;
  for(auto id:ids)if(!id || !seen.insert(id).second) {error="invalid shared-memory chunk identity";return false;}
  chunk_size_=kBufferSize/uint32_t(ids.size());
  for(auto id:ids)chunks_.push_back({id,chunk_size_,true,std::make_shared<const Identity>(id)});
  InitializeCommon();initialized_=true;return true;
}
bool RexSharedMemory::BeginFrame(Frame& frame,std::string& error) {
  error.clear();
  if(!initialized_ || faulted_ || frame_) {error="shared memory has no available native frame scope";return false;}
  for(const auto& chunk:chunks_) {
    auto existing=std::find_if(frame.buffer_storage.begin(),frame.buffer_storage.end(),[&](const BufferStorage& b){return b.id==chunk.id;});
    if(existing!=frame.buffer_storage.end() && (existing->size!=chunk.size || existing->owner!=chunk.owner || !existing->writable)) {
      error="shared-memory storage identity collides in native frame";return false;
    }
    if(std::any_of(frame.buffers.begin(),frame.buffers.end(),[&](const Buffer& b){return b.id==chunk.id;})) {
      error="shared-memory identity collides with immutable data";return false;
    }
  }
  for(const auto& chunk:chunks_)if(std::none_of(frame.buffer_storage.begin(),frame.buffer_storage.end(),[&](const BufferStorage& b){return b.id==chunk.id;}))
    frame.buffer_storage.push_back(chunk);
  frame_=&frame;return true;
}
void RexSharedMemory::EndFrame(bool accepted) {
  if(!frame_ || !accepted)faulted_=true;
  frame_=nullptr;
}
DescriptorResources RexSharedMemory::Descriptor() const {
  DescriptorResources out;out.descriptor_set=0;out.binding=0;
  for(const auto& chunk:chunks_)out.buffers.push_back({chunk.id,0,chunk.size});return out;
}
std::optional<BufferView> RexSharedMemory::View(uint32_t start,uint32_t length) const {
  if(!initialized_ || faulted_ || start>=kBufferSize || !length || length>kBufferSize-start)return {};
  uint32_t chunk=start/chunk_size_,offset=start%chunk_size_;
  if(length>chunk_size_-offset)return {}; // The caller must split and rebase its kernel explicitly.
  return BufferView{chunks_[chunk].id,offset,length};
}
std::optional<BufferView> RexSharedMemory::ContiguousView(uint32_t start,uint32_t length,
    const std::function<Identity()>& next,std::string& error) {
  if(auto existing=View(start,length))return existing;
  if(!frame_ || faulted_ || !length || start>=kBufferSize || length>kBufferSize-start) {
    error="invalid native contiguous shared-memory range";return {};
  }
  BufferStorage scratch{next(),length,true,{}};scratch.owner=std::make_shared<const Identity>(scratch.id);
  frame_->buffer_storage.push_back(scratch);uint32_t copied=0;
  while(copied<length) {
    uint32_t address=start+copied,amount=std::min(length-copied,chunk_size_-address%chunk_size_);
    auto source=View(address,amount);if(!source) {error="native shared-memory chunk mapping failed";return {};}
    frame_->commands.push_back(BufferCopy{*source,{scratch.id,copied,amount},amount});copied+=amount;
  }
  return BufferView{scratch.id,0,length};
}
bool RexSharedMemory::CopyBack(uint32_t start,uint32_t length,BufferView source,std::string& error) {
  if(!frame_ || faulted_ || !length || start>=kBufferSize || length>kBufferSize-start || source.length<length) {
    error="invalid GPU shared-memory writeback";return false;
  }
  uint32_t copied=0;
  while(copied<length) {
    uint32_t address=start+copied,amount=std::min(length-copied,chunk_size_-address%chunk_size_);
    auto dest=View(address,amount);if(!dest) {error="GPU shared-memory writeback chunk is unavailable";return false;}
    BufferView from{source.buffer,source.offset+copied,amount};
    if(from!=*dest)frame_->commands.push_back(BufferCopy{from,*dest,amount});copied+=amount;
  }
  return true;
}
bool RexSharedMemory::UploadRanges(const std::vector<std::pair<uint32_t,uint32_t>>& pages) {
  if(!frame_ || faulted_)return false;
  struct Segment {uint32_t start,length;BufferView destination;std::shared_ptr<std::vector<std::byte>> bytes;};
  std::vector<Segment> segments;
  try {
    uint64_t previous_end=0;
    for(auto [page,count]:pages) {
      uint64_t start=uint64_t(page)<<page_size_log2(),length=uint64_t(count)<<page_size_log2();
      if(!length || start<previous_end || start>=kBufferSize || length>kBufferSize-start)return false;
      previous_end=start+length;
      while(length) {
        uint32_t size=uint32_t(std::min({length,uint64_t(4*1024*1024),uint64_t(chunk_size_-start%chunk_size_)}));
        auto view=View(uint32_t(start),size);if(!view)return false;
        segments.push_back({uint32_t(start),size,*view,std::make_shared<std::vector<std::byte>>(size)});
        start+=size;length-=size;
      }
    }
    // Reserve every command before changing coherence metadata. The shared-memory
    // base requires protection/validity BEFORE memcpy, so concurrent CPU writes
    // invalidate the page rather than being lost after the snapshot is copied.
    frame_->commands.reserve(frame_->commands.size()+segments.size());
    for(auto& segment:segments) {
      MakeRangeValid(segment.start,segment.length,false);
      memcpy(segment.bytes->data(),memory().TranslatePhysical(segment.start),segment.length);
      frame_->commands.emplace_back(BufferUpload{segment.destination,std::move(segment.bytes)});
    }
    return true;
  } catch(const std::exception&) {
    faulted_=true;return false;
  }
}
} // namespace rex::native
