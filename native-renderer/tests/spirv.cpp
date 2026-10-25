// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/spirv.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spirv.hpp>
using namespace rex::native;
static std::vector<uint32_t> ArrayLengthModule(int workgroup=0) {
  using namespace spv;
  std::vector<uint32_t> w={0x07230203,0x00010500,0,128,0};
  auto op=[&](Op code,std::initializer_list<uint32_t> args) {w.push_back((uint32_t(args.size()+1)<<16)|uint32_t(code));w.insert(w.end(),args);};
  op(OpCapability,{CapabilityShader});op(OpMemoryModel,{AddressingModelLogical,MemoryModelGLSL450});
  op(OpEntryPoint,{ExecutionModelGLCompute,100,0x6e69616d,0,40,41});op(OpExecutionMode,{100,ExecutionModeLocalSize,1,1,1});
  op(OpDecorate,{4,DecorationArrayStride,4});op(OpDecorate,{5,DecorationBlock});op(OpMemberDecorate,{5,0,DecorationOffset,0});
  op(OpDecorate,{6,DecorationBlock});op(OpMemberDecorate,{6,0,DecorationOffset,0});
  op(OpDecorate,{40,DecorationDescriptorSet,0});op(OpDecorate,{40,DecorationBinding,7});op(OpDecorate,{40,DecorationNonWritable});
  op(OpDecorate,{41,DecorationDescriptorSet,0});op(OpDecorate,{41,DecorationBinding,8});op(OpDecorate,{11,DecorationSpecId,9});
  if(workgroup)op(OpDecorate,{32,DecorationBuiltIn,BuiltInWorkgroupSize});
  op(OpTypeVoid,{1});op(OpTypeInt,{2,32,0});op(OpTypeFunction,{3,1});op(OpTypeRuntimeArray,{4,2});op(OpTypeStruct,{5,4});op(OpTypeStruct,{6,2});
  op(OpTypePointer,{7,StorageClassStorageBuffer,5});op(OpTypePointer,{8,StorageClassStorageBuffer,6});op(OpTypePointer,{9,StorageClassStorageBuffer,2});
  op(OpConstant,{2,10,0});op(OpSpecConstant,{2,11,5});op(OpVariable,{7,40,StorageClassStorageBuffer});op(OpVariable,{8,41,StorageClassStorageBuffer});
  if(workgroup) {
    op(OpTypeVector,{30,2,3});op(OpConstant,{2,31,1});
    if(workgroup==1)op(OpSpecConstantComposite,{30,32,11,31,31});
    else op(OpConstantComposite,{30,32,31,31,31});
  }
  op(OpFunction,{1,100,FunctionControlMaskNone,3});op(OpLabel,{101});op(OpArrayLength,{2,102,40,0});op(OpIAdd,{2,103,102,11});
  op(OpAccessChain,{9,104,41,10});op(OpStore,{104,103});op(OpReturn,{});op(OpFunctionEnd,{});return w;
}
int main() {
  unsigned checks=0;
  auto check=[&](bool condition) {++checks;if(!condition) {fprintf(stderr,"SPIR-V contract %u failed\n",checks);std::abort();}};
  ConvertedShader shader;std::string error;
  check(!ConvertSpirv({},{1,1},Stage::Vertex,"main",shader,error));check(!error.empty());check(shader.resources.empty());
  uint32_t bad[]={0x07230203,0x00010000,0,3,0,0};
  check(!ConvertSpirv(bad,{1,1},Stage::Vertex,"main",shader,error));check(error.find("instruction")!=std::string::npos);
  bad[5]=2u<<16;
  check(!ConvertSpirv(bad,{1,1},Stage::Vertex,"main",shader,error));check(shader.shader.msl.empty());
  check(!ConvertSpirv(bad,{},Stage::Vertex,"main",shader,error));
  Argument buffer;buffer.kind=ArgumentKind::Buffer;buffer.index=3;buffer.count=2;buffer.minimum_bytes=16;buffer.alignment=16;
  Argument texture;texture.kind=ArgumentKind::Texture;texture.index=5;
  Argument sampler;sampler.kind=ArgumentKind::Sampler;sampler.index=1;
  Argument sizes;sizes.index=30;sizes.minimum_bytes=124;sizes.alignment=4;
  shader.resources={{ResourceSource::Descriptor,1,7,buffer,100},
                    {ResourceSource::Descriptor,2,9,texture,101},
                    {ResourceSource::Descriptor,2,9,sampler,101},
                    {ResourceSource::BufferSizes,0,0,sizes,0}};
  std::vector<DescriptorResources> descriptors={
    {ResourceSource::Descriptor,1,7,{{{10,1},16,64},{{11,1},0,128}},{},{}},
    {ResourceSource::Descriptor,2,9,{},{{{20,1}}},{{30,1}}}};
  Bindings bindings;std::optional<Buffer> generated;
  check(BindConverted(shader,descriptors,{40,1},bindings,generated,error));
  check(bindings.buffers.size()==3 && bindings.textures.size()==1 && bindings.samplers.size()==1);
  check(bindings.buffers[0].index==3 && bindings.buffers[1].index==4 && bindings.buffers[2].index==30);
  check(bindings.textures[0].index==5 && bindings.samplers[0].index==1);
  uint32_t values[31];memcpy(values,generated->bytes->data(),sizeof(values));
  check(values[3]==64 && values[4]==128 && values[0]==0);
  descriptors[0].buffers[1].length=8;
  check(!BindConverted(shader,descriptors,{40,2},bindings,generated,error));
  check(!generated && bindings.buffers.empty());
  descriptors[0].buffers[1].length=128;descriptors[0].buffers[0].offset=4;
  check(!BindConverted(shader,descriptors,{40,2},bindings,generated,error));
  descriptors[0].buffers[0].offset=16;
  check(!BindConverted(shader,descriptors,{},bindings,generated,error));
  descriptors.push_back(descriptors[0]);
  check(!BindConverted(shader,descriptors,{40,2},bindings,generated,error));descriptors.pop_back();
  descriptors[1].samplers.clear();
  check(!BindConverted(shader,descriptors,{40,2},bindings,generated,error));
  auto module=ArrayLengthModule();
  bool converted=ConvertSpirv(module,{50,1},Stage::Compute,"main",shader,error);
  if(!converted)fprintf(stderr,"Array-length conversion: %s\n",error.c_str());check(converted);
  check(shader.resources.size()==3 && shader.shader.constants.size()==1);
  check(shader.shader.constants[0].index==9 && shader.shader.constants[0].bits==5);
  check(!shader.resources[0].argument.writable && shader.resources[1].argument.writable);
  check(shader.workgroup_size==std::array<uint32_t,3>{1,1,1});
  descriptors={{ResourceSource::Descriptor,0,7,{{{51,1},16,64}},{},{}},
               {ResourceSource::Descriptor,0,8,{{{52,1},0,4}},{},{}}};
  check(BindConverted(shader,descriptors,{53,1},bindings,generated,error));
  memcpy(values,generated->bytes->data(),sizeof(values));check(values[0]==64 && values[1]==4);
  check(!ConvertSpirv(module,{50,2},Stage::Vertex,"main",shader,error));check(shader.resources.empty());
  check(!ConvertSpirv(ArrayLengthModule(1),{50,3},Stage::Compute,"main",shader,error));
  check(error.find("workgroup")!=std::string::npos);
  check(ConvertSpirv(ArrayLengthModule(2),{50,4},Stage::Compute,"main",shader,error));
  check(shader.workgroup_size==std::array<uint32_t,3>{1,1,1});
  printf("%u native shader binding contracts passed\n",checks);
}
