// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_resolve.h"
#include "rex/native/rex_transfer_shader.h"
#include <rex/graphics/pipeline/texture/cache.h>
#include <cstring>
namespace rex::native {
namespace gfx=graphics;
namespace {
template<class T> void Add(std::vector<T>& list,const T& value) {
  if(std::none_of(list.begin(),list.end(),[&](const T& s){return s.id==value.id;}))list.push_back(value);
}
Buffer Constant(const std::function<Identity()>& next,const auto& value) {
  auto data=std::make_shared<std::vector<std::byte>>(256);std::memcpy(data->data(),&value,sizeof(value));return {next(),data,true};
}
Attachment Attach(SurfaceView v) {Attachment a;a.view=v;return a;}
}
bool RexResolve::Encode(Frame& frame,const ConvertedShader& shader,std::vector<DescriptorResources> resources,uint32_t gx,uint32_t gy,std::string& error) {
  Add(frame.shaders,shader.shader);Dispatch d;d.shader=shader.shader.id;d.groups={gx,gy,1};d.threads=shader.workgroup_size;
  std::optional<Buffer> aux;if(!BindConverted(shader,resources,next_(),d.bindings,aux,error))return false;
  if(aux)frame.buffers.push_back(*aux);frame.commands.push_back(std::move(d));return true;
}
bool RexResolve::Dump(Frame& frame,RexRenderTargets::Key key,const Surface& source,const BufferStorage& edram,
    uint32_t pitch,uint32_t first,uint32_t width,uint32_t height,std::string& error) {
  const uint32_t cache_key=key.resource_format|(uint32_t(key.msaa_samples)<<4)|(uint32_t(key.is_depth)<<6);
  auto* shader=dumps_.Find(cache_key);
  if(!shader) {ConvertedShader converted;if(!DumpShader(key,next_(),converted,error))return false;shader=&dumps_.Insert(cache_key,std::move(converted));dumps_.Trim(48);}
  Add(frame.surfaces,source);Add(frame.buffer_storage,edram);
  const std::array<uint32_t,2> values={pitch|(key.GetPitchTiles()<<gfx::xenos::kEdramPitchTilesBits),first|(key.base_tiles<<(gfx::xenos::kEdramBaseTilesBits+1))};
  auto constants=Constant(next_,values);frame.buffers.push_back(constants);
  SurfaceView view{source.id};if(!key.is_depth)view.format=RexTransferShader::OwnershipFormat(key.GetColorFormat());
  std::vector<DescriptorResources> resources={
    {ResourceSource::Descriptor,0,0,{{edram.id,0,edram.size}},{},{}},
    {ResourceSource::Descriptor,1,0,{}, {view},{}},
    {ResourceSource::PushConstants,0,0,{{constants.id,0,256}},{},{}}};
  if(key.is_depth) {SurfaceView stencil{source.id};stencil.format=Format::Stencil8View;resources.push_back({ResourceSource::Descriptor,1,1,{}, {stencil},{}});}
  return Encode(frame,*shader,std::move(resources),((gfx::xenos::kEdramTileWidthSamples>>uint32_t(key.Is64bpp()))*width+7)/8,
    gfx::xenos::kEdramTileHeightSamples*height/16,error);
}
bool RexResolve::Copy(Frame& frame,gfx::draw_util::ResolveCopyShaderIndex index,const gfx::draw_util::ResolveCopyShaderConstants& c,
    const BufferStorage& edram,BufferView dest,uint32_t gx,uint32_t gy,std::string& error) {
  auto* shader=copies_.Find(uint32_t(index));
  if(!shader) {
    auto words=CopyKernel(index);ConvertedShader converted;
    if(words.empty()) {error="native resolve kernel is unavailable";return false;}
    if(!ConvertSpirv(words,next_(),Stage::Compute,"main",converted,error))return false;
    shader=&copies_.Insert(uint32_t(index),std::move(converted));copies_.Trim(16);
  }
  Add(frame.buffer_storage,edram);auto constants=Constant(next_,c);frame.buffers.push_back(constants);
  return Encode(frame,*shader,{{ResourceSource::Descriptor,0,0,{{edram.id,0,edram.size}},{},{}},
    {ResourceSource::Descriptor,1,0,{dest},{},{}},{ResourceSource::PushConstants,0,0,{{constants.id,0,256}},{},{}}},gx,gy,error);
}
} // namespace rex::native
