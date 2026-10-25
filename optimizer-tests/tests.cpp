#include <rex/graphics/vulkan/spirv_register_optimizer.h>
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <cstring>
#include <string>
#include <vulkan/vulkan.h>

using rex::graphics::vulkan::OptimizeSpirvRegisters;
using rex::graphics::vulkan::SpirvRegisterStats;
static unsigned checks=0;
static void Check(bool value) {++checks;if(!value){std::fprintf(stderr,"Register optimizer check %u failed\n",checks);std::abort();}}
static bool optimization_enabled=true;
static unsigned module_calls=0, module_failures=0;
static std::vector<uint32_t> submitted_module;
#define REXCVAR_GET(x) optimization_enabled
#define REXGPU_INFO(...) ((void)0)
#define REXGPU_WARN(...) ((void)0)
#define REXGPU_ERROR(...) ((void)0)
namespace rex::ui::vulkan {
struct VulkanDevice {
 struct Properties {bool uniformBufferStandardLayout=false,scalarBlockLayout=false;} properties_;
 const Properties& properties()const{return properties_;}
 struct Functions {
  VkResult vkCreateShaderModule(VkDevice,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks*,VkShaderModule* out)const {
   ++module_calls;submitted_module.assign(info->pCode,info->pCode+info->codeSize/4);
   if(module_failures){--module_failures;return VK_ERROR_UNKNOWN;}
   *out=(VkShaderModule)(uintptr_t)7;return VK_SUCCESS;
  }
 } functions_;
 const Functions& functions()const{return functions_;}
 VkDevice device()const{return VK_NULL_HANDLE;}
};
}
namespace rex::graphics::vulkan {
struct VulkanShader {
 const ui::vulkan::VulkanDevice* vulkan_device_;
 uint64_t ucode_data_hash()const{return 1;}
 struct VulkanTranslation;
};
struct VulkanShader::VulkanTranslation {
  VulkanShader owner;std::vector<uint8_t> bytes;VkShaderModule shader_module_=VK_NULL_HANDLE;bool valid=true;
  bool is_valid()const{return valid;} void MakeInvalid(){valid=false;}
  const auto& translated_binary()const{return bytes;}
  const auto& shader()const{return owner;} uint64_t modification()const{return 0;}
  VkShaderModule GetOrCreateShaderModule();
};
#include "shader-module.inc"
}
// This module models a static register bank and an externally visible store.
// No arithmetic is needed: the result must keep every original float bit.
static std::string Kernel(uint32_t bits,bool dynamic=false) {
  std::string text=R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %buffer
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 3
%void = OpTypeVoid
%function = OpTypeFunction %void
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%four = OpConstant %uint 4
%bits = OpConstant %uint )"+std::to_string(bits)+R"(
%arr = OpTypeArray %float %four
%ptr_reg = OpTypePointer Function %arr
%ptr_value = OpTypePointer Function %float
%block = OpTypeStruct %uint
%ptr_buffer = OpTypePointer StorageBuffer %block
%ptr_uint = OpTypePointer StorageBuffer %uint
%buffer = OpVariable %ptr_buffer StorageBuffer
%main = OpFunction %void None %function
%entry = OpLabel
%registers = OpVariable %ptr_reg Function
%out = OpAccessChain %ptr_uint %buffer %zero
%value = OpBitcast %float %bits
)";
  if(dynamic)text+=R"(
%input = OpLoad %uint %out
%index = OpBitwiseAnd %uint %input %one
%target = OpAccessChain %ptr_value %registers %index
)";
  else text+="%target = OpAccessChain %ptr_value %registers %one\n";
  text+=R"(
OpStore %target %value
%loaded = OpLoad %float %target
%result = OpBitcast %uint %loaded
OpStore %out %result
OpReturn
OpFunctionEnd
)";
  return text;
}

// Follow only copy/bitcast/constant definitions to the external store. This
// independently verifies that register promotion retained the original bits.
static uint32_t StoredBits(const std::vector<uint32_t>& code) {
  std::map<uint32_t,std::vector<uint32_t>> defs;uint32_t stored=0;
  for(size_t i=5;i<code.size();i+=code[i]>>16){
    uint32_t size=code[i]>>16,op=code[i]&0xffff;
    if(op==spv::OpConstant||op==spv::OpBitcast||op==spv::OpCopyObject)
      defs[code[i+2]]=std::vector<uint32_t>(code.begin()+i,code.begin()+i+size);
    if(op==spv::OpStore)stored=code[i+2];
  }
  for(unsigned tries=0;tries<32;++tries){
    Check(defs.contains(stored));const auto& op=defs.at(stored);
    if((op[0]&0xffff)==spv::OpConstant)return op[3];
    stored=op[3];
  }
  Check(false);return 0;
}

int main(){
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  tools.SetMessageConsumer([](spv_message_level_t,const char*,const spv_position_t&,const char* msg){std::fprintf(stderr,"SPIR-V test: %s\n",msg);});
  const std::array<uint32_t,8> patterns={0,0x80000000,0x3f800000,0x7f800000,0xff800000,0x7fc12345,0x00000001,0xffffffff};
  for(auto bits:patterns){
    std::vector<uint32_t> original,optimized;SpirvRegisterStats stats;std::string error;
    Check(tools.Assemble(Kernel(bits),&original));Check(tools.Validate(original));
    Check(OptimizeSpirvRegisters(original,optimized,stats,error));Check(error.empty());Check(tools.Validate(optimized));
    if(stats.function_variables_after) {
      std::string diagnostic;tools.Disassemble(optimized,&diagnostic);
      std::fprintf(stderr,"Register stats before/after: %u/%u loads %u/%u stores %u/%u\n%s\n",
        stats.function_variables_before,stats.function_variables_after,stats.loads_before,stats.loads_after,
        stats.stores_before,stats.stores_after,diagnostic.c_str());
    }
    Check(stats.function_variables_before==1&&stats.function_variables_after==0);
    Check(stats.loads_after<stats.loads_before);Check(stats.stores_after<stats.stores_before);
    Check(StoredBits(optimized)==bits);
    std::vector<uint32_t> second;Check(OptimizeSpirvRegisters(optimized,second,stats,error));Check(StoredBits(second)==bits);
    Check(tools.Assemble(Kernel(bits,true),&original));Check(OptimizeSpirvRegisters(original,optimized,stats,error));
    Check(tools.Validate(optimized)); // Dynamic address version must stay valid.
  }
  std::vector<uint32_t> output={1};SpirvRegisterStats stats;std::string error;
  Check(!OptimizeSpirvRegisters({},output,stats,error)&&output.empty()&&!error.empty());
  std::vector<uint32_t> invalid={spv::MagicNumber,0x00010500,0,2,0,0};
  Check(!OptimizeSpirvRegisters(invalid,output,stats,error)&&output.empty());
  std::vector<uint32_t> code;Check(tools.Assemble(Kernel(42),&code));code[3]=0;
  Check(!OptimizeSpirvRegisters(code,output,stats,error)&&output.empty());
  // A used arithmetic result must retain its precise decoration. The optimizer
  // must also leave resource bindings and the entry-point contract intact.
  std::string precise=Kernel(0x3f800000);
  precise.insert(precise.find("%void ="),"OpDecorate %sum NoContraction\n");
  auto where=precise.find("%result = OpBitcast");
  precise.insert(where,"%sum = OpFAdd %float %loaded %value\n");
  where=precise.find("%result = OpBitcast %uint %loaded");
  precise.replace(where,std::string("%result = OpBitcast %uint %loaded").size(),"%result = OpBitcast %uint %sum");
  Check(tools.Assemble(precise,&code));Check(OptimizeSpirvRegisters(code,output,stats,error));
  std::string disassembly;Check(tools.Disassemble(output,&disassembly));
  Check(disassembly.find("NoContraction")!=std::string::npos);
  Check(disassembly.find("Binding 3")!=std::string::npos);
  Check(disassembly.find("OpFAdd")!=std::string::npos);
  // Exercise the actual shader-module creation body and both fallback paths.
  using Translation=rex::graphics::vulkan::VulkanShader::VulkanTranslation;
  rex::ui::vulkan::VulkanDevice device;
  auto make=[&](const std::vector<uint32_t>& words){Translation t;t.owner.vulkan_device_=&device;
    t.bytes.resize(words.size()*4);std::memcpy(t.bytes.data(),words.data(),t.bytes.size());return t;};
  Check(tools.Assemble(Kernel(42),&code));
  auto t=make(code);module_calls=0;module_failures=0;optimization_enabled=true;
  Check(t.GetOrCreateShaderModule()!=VK_NULL_HANDLE&&module_calls==1&&submitted_module!=code);
  Check(t.GetOrCreateShaderModule()!=VK_NULL_HANDLE&&module_calls==1);
  t=make(code);module_calls=0;module_failures=1;
  Check(t.GetOrCreateShaderModule()!=VK_NULL_HANDLE&&module_calls==2&&submitted_module==code&&t.valid);
  t=make(code);module_calls=0;module_failures=2;
  Check(t.GetOrCreateShaderModule()==VK_NULL_HANDLE&&module_calls==2&&!t.valid);
  t=make(code);module_calls=0;module_failures=0;optimization_enabled=false;
  Check(t.GetOrCreateShaderModule()!=VK_NULL_HANDLE&&module_calls==1&&submitted_module==code);
  optimization_enabled=true;t=make(invalid);module_calls=0;
  Check(t.GetOrCreateShaderModule()!=VK_NULL_HANDLE&&module_calls==1&&submitted_module==invalid);

  // Packed uniform data uses the same scalar-layout requirement as the real
  // translator's system constants. Feature-disabled devices must still reject.
  std::string packed=Kernel(42);
  packed.replace(packed.find("%main \"main\" %buffer"),std::string("%main \"main\" %buffer").size(),"%main \"main\" %buffer %packedUniform");
  packed.insert(packed.find("%void ="),R"(
OpDecorate %packedBlock Block
OpMemberDecorate %packedBlock 0 Offset 0
OpMemberDecorate %packedBlock 1 Offset 4
OpDecorate %packedArray ArrayStride 16
OpDecorate %packedUniform DescriptorSet 2
OpDecorate %packedUniform Binding 1
)");
  packed.insert(packed.find("%main ="),R"(
%vec4 = OpTypeVector %float 4
%packedArray = OpTypeArray %vec4 %four
%packedBlock = OpTypeStruct %uint %packedArray
%packedPtr = OpTypePointer Uniform %packedBlock
%packedUniform = OpVariable %packedPtr Uniform
)");
  Check(tools.Assemble(packed,&code));
  Check(!OptimizeSpirvRegisters(code,output,stats,error,{}));Check(output.empty());
  Check(!OptimizeSpirvRegisters(code,output,stats,error,{true,false}));Check(output.empty());
  Check(OptimizeSpirvRegisters(code,output,stats,error,{false,true}));
  Check(OptimizeSpirvRegisters(code,output,stats,error,{true,true}));
  device.properties_={true,false};t=make(code);module_calls=0;
  Check(t.GetOrCreateShaderModule()!=VK_NULL_HANDLE&&module_calls==1&&submitted_module==code);
  device.properties_={false,true};t=make(code);module_calls=0;
  Check(t.GetOrCreateShaderModule()!=VK_NULL_HANDLE&&module_calls==1&&submitted_module!=code);
  device.properties_={true,true};t=make(code);module_calls=0;
  Check(t.GetOrCreateShaderModule()!=VK_NULL_HANDLE&&module_calls==1&&submitted_module!=code);
  // Scalar layout does not excuse a two-byte-misaligned vec4 member.
  packed.replace(packed.find("1 Offset 4"),std::string("1 Offset 4").size(),"1 Offset 2");
  Check(tools.Assemble(packed,&code));
  Check(!OptimizeSpirvRegisters(code,output,stats,error,{true,true}));Check(output.empty());
  std::printf("%u register promotion, bit preservation, dynamic-address and fallback checks passed\n",checks);
}
