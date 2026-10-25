// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_constants.h"
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/util/draw.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
namespace rex::filesystem {
FILE* OpenFile(const std::filesystem::path& path,std::string_view mode) {return fopen(path.string().c_str(),std::string(mode).c_str());}
}
namespace rex {
bool bit_scan_forward(uint32_t value,uint32_t* result) {*result=std::countr_zero(value);return value!=0;}
namespace memory {
void copy_and_swap_32_unaligned(void* dst,const void* src,size_t count) {
  for(size_t i=0;i<count;++i) {uint32_t value;memcpy(&value,static_cast<const char*>(src)+i*4,4);value=std::byteswap(value);memcpy(static_cast<char*>(dst)+i*4,&value,4);}
}
void copy_and_swap_32_aligned(void* dst,const void* src,size_t count) {copy_and_swap_32_unaligned(dst,src,count);}
}
}
namespace rex::graphics::draw_util {
const int8_t kD3D10StandardSamplePositions2x[2][2]={{4,4},{-4,-4}};
const int8_t kD3D10StandardSamplePositions4x[4][2]={{-2,-6},{6,-2},{-6,2},{2,6}};
}
int main(int argc,char** argv) {
  using namespace rex;using namespace rex::graphics;using namespace rex::native;
  unsigned checks=0,modules=0;std::string error;uint64_t object=100;
  auto check=[&](bool value) {++checks;if(!value) {std::cerr<<"ReX shader check "<<checks<<" failed: "<<error<<'\n';std::abort();}};
  auto next=[&] {return Identity{object++,1};};
  RexConstantState state;RegisterFile registers;
  for(size_t i=0;i<RegisterFile::kRegisterCount;++i)registers.values[i]=uint32_t(i);
  RexConstantState::System system{};
  if(argc>1)std::filesystem::create_directories(argv[1]);
  for(uint32_t version:{spv::Spv_1_0,spv::Spv_1_3,spv::Spv_1_5}) {
    for(int variant=0;variant<3;++variant)for(int stage=0;stage<3;++stage) {
      SpirvShaderTranslator::Features features(false);features.spirv_version=version;
      features.signed_zero_inf_nan_preserve_float32=true;
      features.denorm_flush_to_zero_float32=true;features.rounding_mode_rte_float32=true;
      SpirvShaderTranslator translator(features,true,true,false);
      std::vector<uint8_t> bytes;RexConstantState::Map map{};
      if(stage==2)bytes=translator.CreateDepthOnlyFragmentShader();
      else {
        const uint32_t dest=stage?0:62;
        const uint32_t ucode[]={1u|(3u<<12),2u<<12,0,15u<<16,0,1u<<8,
          1u|(15u<<16),0,(1u<<24)|(1u<<8)|(variant==2?0x40u<<16:0u)|(1u<<31),
          dest|(1u<<15)|(15u<<16),0,(2u<<24)|(1u<<16)|(1u<<8)|(3u<<30)};
        graphics::Shader source(stage?xenos::ShaderType::kPixel:xenos::ShaderType::kVertex,0,
          variant?ucode:nullptr,variant?std::size(ucode):0,std::endian::native);
        string::StringBuffer disasm;source.AnalyzeUcode(disasm);
        auto modification=stage?translator.GetDefaultPixelShaderModification(variant==2?8:0):
                                translator.GetDefaultVertexShaderModification(variant==2?8:0);
        auto* translated=source.GetOrCreateTranslation(modification);
        check(translator.TranslateAnalyzedShader(*translated));bytes=translated->translated_binary();map=source.constant_register_map();
      }
      std::vector<uint32_t> words(bytes.size()/4);memcpy(words.data(),bytes.data(),bytes.size());
      ConvertedShader converted;
      check(ConvertSpirv(words,next(),stage==0?Stage::Vertex:Stage::Fragment,"main",converted,error));
      Frame frame;std::vector<DescriptorResources> descriptors;const ConvertedShader* shaders[]={&converted};
      check(state.Prepare(system,registers,map,map,shaders,next,frame,descriptors,error));
      for(const auto& r:converted.resources)if(r.source==ResourceSource::Descriptor && r.descriptor_set!=SpirvShaderTranslator::kDescriptorSetConstants) {
        check(r.argument.kind==ArgumentKind::Buffer);
        DescriptorResources resource;resource.source=r.source;resource.descriptor_set=r.descriptor_set;resource.binding=r.binding;
        for(uint32_t i=0;i<r.argument.count;++i)resource.buffers.push_back({next(),0,std::max(uint64_t(4*1024*1024),r.argument.minimum_bytes)});
        descriptors.push_back(std::move(resource));
      }
      Bindings bindings;std::optional<Buffer> auxiliary;
      check(BindConverted(converted,descriptors,next(),bindings,auxiliary,error));
      for(const auto& b:frame.buffers)check(b.bytes->size()%256==0 && b.constants);
      auto previous=frame.buffers;Frame second;descriptors.clear();
      check(state.Prepare(system,registers,map,map,shaders,next,second,descriptors,error));
      check(second.buffers.size()==previous.size());
      for(size_t i=0;i<previous.size();++i)check(previous[i].id==second.buffers[i].id && previous[i].bytes==second.buffers[i].bytes);
      if(argc>1) {
        auto path=std::filesystem::path(argv[1])/(std::to_string(version)+"-"+std::to_string(variant)+"-"+std::to_string(stage)+".metal");
        std::ofstream(path)<<converted.shader.msl;
      }
      ++modules;
    }
  }
  // Verify sparse bank order and that changing a constant never changes an old
  // frame's snapshot. Counts and bitmaps come from the SDK's actual map type.
  ConvertedShader shader;Argument a;a.minimum_bytes=32;
  shader.resources={{ResourceSource::Descriptor,1,1,a,1},{ResourceSource::Descriptor,1,0,a,2}};
  RexConstantState::Map map{};map.float_bitmap[0]=(1ull<<1)|(1ull<<5);map.float_count=2;
  const ConvertedShader* list[]={&shader};Frame first;std::vector<DescriptorResources> descriptors;
  check(state.Prepare(system,registers,map,map,list,next,first,descriptors,error));
  auto floats=std::find_if(descriptors.begin(),descriptors.end(),[](const auto& d){return d.binding==1;})->buffers[0];
  auto old=*std::find_if(first.buffers.begin(),first.buffers.end(),[&](const Buffer& b){return b.id==floats.buffer;});
  uint32_t first_value,second_value;memcpy(&first_value,old.bytes->data(),4);memcpy(&second_value,old.bytes->data()+16,4);
  check(first_value==registers[XE_GPU_REG_SHADER_CONSTANT_000_X+4] && second_value==registers[XE_GPU_REG_SHADER_CONSTANT_000_X+20]);
  registers[XE_GPU_REG_SHADER_CONSTANT_000_X+4]=0x12345678;
  Frame changed;descriptors.clear();check(state.Prepare(system,registers,map,map,list,next,changed,descriptors,error));
  auto newer=std::find_if(changed.buffers.begin(),changed.buffers.end(),[&](const Buffer& b){return b.id!=old.id && b.bytes->size()==old.bytes->size();});
  check(newer!=changed.buffers.end());memcpy(&first_value,old.bytes->data(),4);check(first_value!=0x12345678);
  map.float_count=3;Frame rejected;descriptors.clear();
  check(!state.Prepare(system,registers,map,map,list,next,rejected,descriptors,error));check(rejected.buffers.empty() && descriptors.empty());
  std::cout<<modules<<" actual ReX translator modules converted; "<<checks<<" conversion, binding and constant-ownership checks passed\n";
}
