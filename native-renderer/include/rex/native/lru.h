// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <list>
#include <map>
#include <utility>
namespace rex::native {
// The map owns complete keys. A hit is never inferred from a hash alone.
// Saturation retires one cold entry; it never clears the whole working set.
template<class Key,class Value> class Lru {
  struct Entry {Value value;typename std::list<Key>::iterator position;};
  std::list<Key> order_;
  std::map<Key,Entry> entries_;
 public:
  Lru()=default;
  Lru(const Lru&)=delete;Lru& operator=(const Lru&)=delete;
  Lru(Lru&&)=default;Lru& operator=(Lru&&)=default;
  Value* Find(const Key& key) {
    auto i=entries_.find(key);if(i==entries_.end())return nullptr;
    order_.splice(order_.end(),order_,i->second.position);return &i->second.value;
  }
  bool Contains(const Key& key) const{return entries_.contains(key);}
  Value& Insert(Key key,Value value) {
    if(auto* existing=Find(key)){*existing=std::move(value);return *existing;}
    order_.push_back(key);
    auto [i,added]=entries_.emplace(std::move(key),Entry{std::move(value),std::prev(order_.end())});
    return i->second.value;
  }
  bool Pop() {
    if(order_.empty())return false;
    entries_.erase(order_.front());order_.pop_front();return true;
  }
  void Trim(size_t maximum){while(entries_.size()>maximum)Pop();}
  size_t Size() const{return entries_.size();}
};
} // namespace rex::native
