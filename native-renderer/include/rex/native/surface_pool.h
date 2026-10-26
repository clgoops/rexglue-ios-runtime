// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "frame.h"
#include <list>
#include <map>
namespace rex::native {
// Reuses exact render-target keys after the SDK drops their EDRAM ownership.
// Pool eviction releases only its reference; recorded/GPU packets keep theirs.
class SurfacePool {
  struct Entry {Surface image;uint64_t bytes;std::list<uint32_t>::iterator position;};
  std::map<uint32_t,Entry> entries_;
  std::list<uint32_t> order_;
  uint64_t bytes_=0,limit_;
 public:
  explicit SurfacePool(uint64_t limit):limit_(limit) {}
  SurfacePool(const SurfacePool&)=delete;
  std::optional<Surface> Find(uint32_t key) {
    auto i=entries_.find(key);if(i==entries_.end())return {};
    order_.splice(order_.end(),order_,i->second.position);return i->second.image;
  }
  void Remember(uint32_t key,const Surface& image) {
    if(entries_.contains(key))return;
    const uint64_t size=uint64_t(image.width)*image.height*image.samples*Describe(image.format).bytes;
    if(!size||size>limit_)return;
    while(bytes_>limit_-size) {
      auto i=entries_.find(order_.front());bytes_-=i->second.bytes;entries_.erase(i);order_.pop_front();
    }
    order_.push_back(key);entries_.emplace(key,Entry{image,size,std::prev(order_.end())});bytes_+=size;
  }
  void Clear() {entries_.clear();order_.clear();bytes_=0;}
  uint64_t bytes() const {return bytes_;}
};
} // namespace rex::native
