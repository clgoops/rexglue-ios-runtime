// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_primitives.h"
namespace rex::native {
namespace gfx=graphics;
RexPrimitives::RexPrimitives(const gfx::RegisterFile& regs,memory::Memory& memory,RexSharedMemory& shared,std::function<Identity()> next)
  :gfx::PrimitiveProcessor(regs,memory,shared),shared_(shared),next_(std::move(next)) {}
RexPrimitives::~RexPrimitives() {ShutdownCommon();}
bool RexPrimitives::Initialize() {return InitializeCommon(true,false,false,false,false,false);}
void RexPrimitives::BeginNativeFrame(Frame& frame) {frame_=&frame;}
void RexPrimitives::EndNativeFrame() {ClearPerFrameCache();converted_.clear();frame_=nullptr;}
bool RexPrimitives::InitializeBuiltinIndexBuffer(size_t size,std::function<void(void*)> fill) {
  auto bytes=std::make_shared<std::vector<std::byte>>(size);fill(bytes->data());builtin_=Buffer{next_(),bytes,false};return true;
}
void* RexPrimitives::RequestHostConvertedIndexBufferForCurrentFrame(gfx::xenos::IndexFormat format,uint32_t count,
    bool coalign,uint32_t original,size_t& handle) {
  if(!frame_ || !count)return nullptr;
  Converted converted;converted.length=uint64_t(count)*(format==gfx::xenos::IndexFormat::kInt16?2:4);
  converted.bytes=std::make_shared<std::vector<std::byte>>(converted.length+2*XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE+16);
  if(coalign)converted.offset=GetSimdCoalignmentOffset(converted.bytes->data(),original);
  converted.buffer={next_(),converted.bytes,false};auto* result=converted.bytes->data()+converted.offset;
  handle=converted_.size();converted_.push_back(std::move(converted));return result;
}
bool RexPrimitives::Bind(const ProcessingResult& p,Draw& draw,std::string& error) {
  if(!frame_) {error="native primitive binding outside a frame";return false;}
  switch(p.host_primitive_type) {
    case gfx::xenos::PrimitiveType::kPointList:draw.primitive=Primitive::Points;break;
    case gfx::xenos::PrimitiveType::kLineList:draw.primitive=Primitive::Lines;break;
    case gfx::xenos::PrimitiveType::kLineStrip:draw.primitive=Primitive::LineStrip;break;
    case gfx::xenos::PrimitiveType::kTriangleList:draw.primitive=Primitive::Triangles;break;
    case gfx::xenos::PrimitiveType::kTriangleStrip:draw.primitive=Primitive::TriangleStrip;break;
    default:error="unsupported native primitive "+std::to_string(uint32_t(p.host_primitive_type));return false;
  }
  draw.index_bytes=p.host_index_format==gfx::xenos::IndexFormat::kInt16?2:4;
  if(p.index_buffer_type==kNone) {draw.vertices=p.host_draw_vertex_count;return true;}
  const uint32_t length=p.host_draw_vertex_count*draw.index_bytes;
  BufferView view;
  auto declare=[&](const Buffer& b) {
    if(std::none_of(frame_->buffers.begin(),frame_->buffers.end(),[&](const Buffer& i){return i.id==b.id;}))frame_->buffers.push_back(b);
  };
  switch(p.index_buffer_type) {
    case kGuestDMA: {
      auto data=shared_.ContiguousView(p.guest_index_base,length,next_,error);if(!data)return false;view=*data;
    } break;
    case kHostConverted: {
      if(p.host_index_buffer_handle>=converted_.size()) {error="invalid native converted index handle";return false;}
      const auto& data=converted_[p.host_index_buffer_handle];declare(data.buffer);view={data.buffer.id,data.offset,data.length};
    } break;
    case kHostBuiltinForAuto:case kHostBuiltinForDMA: {
      if(!builtin_) {error="native builtin index buffer is missing";return false;}
      declare(*builtin_);view={builtin_->id,GetBuiltinIndexBufferOffsetBytes(p.host_index_buffer_handle),length};
    } break;
    default:error="unsupported native index source";return false;
  }
  draw.indices=view;draw.index_count=p.host_draw_vertex_count;return true;
}
} // namespace rex::native
