// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_constants.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <set>

namespace rex::native {
namespace {
using Translator=graphics::SpirvShaderTranslator;
std::vector<std::byte> Padded(size_t size) {
  // Metal can reflect the trailing alignment of a constant struct beyond its
  // SPIR-V member extent. Retain zero padding and a view spanning the allocation.
  return std::vector<std::byte>((std::max(size,size_t(1))+255)&~size_t(255));
}
}
void RexConstantState::Reset() {banks_={};}
bool RexConstantState::Prepare(const System& system,const graphics::RegisterFile& regs,
    const Map& vertex,const Map& pixel,std::span<const ConvertedShader* const> shaders,
    const std::function<Identity()>& next_id,Frame& frame,std::vector<DescriptorResources>& descriptors,std::string& error) {
  error.clear();
  std::set<uint32_t> needed;
  for(const auto* shader:shaders) {
    if(!shader) {error="missing compiled shader for constant preparation";return false;}
    for(const auto& r:shader->resources)if(r.source==ResourceSource::Descriptor && r.descriptor_set==Translator::kDescriptorSetConstants) {
      if(r.argument.kind!=ArgumentKind::Buffer || r.argument.count!=1 || r.binding>=banks_.size()) {
        error="translated shader constant descriptor differs from the SDK layout";return false;
      }
      needed.insert(r.binding);
    }
  }
  auto pending=banks_;std::vector<Buffer> additions;std::vector<DescriptorResources> resources;
  for(uint32_t bank:needed) {
    std::vector<std::byte> bytes;
    switch(bank) {
      case Translator::kConstantBufferSystem:
        bytes=Padded(sizeof(system));memcpy(bytes.data(),&system,sizeof(system));break;
      case Translator::kConstantBufferFloatVertex:
      case Translator::kConstantBufferFloatPixel: {
        const auto& map=bank==Translator::kConstantBufferFloatVertex?vertex:pixel;
        uint32_t count=0;for(auto bits:map.float_bitmap)count+=std::popcount(bits);
        if(count!=map.float_count) {error="float constant bitmap/count mismatch";return false;}
        bytes=Padded(size_t(count)*16);size_t offset=0;
        uint32_t base=bank==Translator::kConstantBufferFloatVertex?
          graphics::XE_GPU_REG_SHADER_CONSTANT_000_X:graphics::XE_GPU_REG_SHADER_CONSTANT_256_X;
        for(uint32_t i=0;i<4;++i)for(uint64_t bits=map.float_bitmap[i];bits;bits&=bits-1) {
          uint32_t index=std::countr_zero(bits);
          memcpy(bytes.data()+offset,&regs[base+(i<<8)+(index<<2)],16);offset+=16;
        }
        break;
      }
      case Translator::kConstantBufferBoolLoop:
        bytes=Padded(160);memcpy(bytes.data(),&regs[graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],160);break;
      case Translator::kConstantBufferFetch:
        bytes=Padded(768);memcpy(bytes.data(),&regs[graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],768);break;
      default:error="unknown SDK constant bank";return false;
    }
    if(!pending[bank] || *pending[bank]->bytes!=bytes) {
      Identity id=next_id();if(!id) {error="constant identity allocator returned an invalid identity";return false;}
      pending[bank]=Buffer{id,std::make_shared<const std::vector<std::byte>>(std::move(bytes)),true};
    }
    const auto& buffer=*pending[bank];
    auto existing=std::find_if(frame.buffers.begin(),frame.buffers.end(),[&](const Buffer& b){return b.id==buffer.id;});
    if(existing!=frame.buffers.end() && (existing->bytes!=buffer.bytes || !existing->constants)) {
      error="constant identity collides with an existing frame buffer";return false;
    }
    if(existing==frame.buffers.end())additions.push_back(buffer);
    resources.push_back({ResourceSource::Descriptor,Translator::kDescriptorSetConstants,bank,
                         {{buffer.id,0,buffer.bytes->size()}},{},{}});
  }
  for(const auto& r:resources)for(const auto& old:descriptors)
    if(old.source==r.source && old.descriptor_set==r.descriptor_set && old.binding==r.binding) {
      error="constant preparation would duplicate an existing descriptor";return false;
    }
  banks_=std::move(pending);
  frame.buffers.insert(frame.buffers.end(),additions.begin(),additions.end());
  descriptors.insert(descriptors.end(),resources.begin(),resources.end());return true;
}
} // namespace rex::native
