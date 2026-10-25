#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/vulkan/spirv_register_optimizer.h>
#include <cstdio>
#include <fstream>
#include <rex/graphics/util/draw.h>
#include <cstring>
// Only the file-dump host service is supplied by this isolated probe.
namespace rex::filesystem {
FILE* OpenFile(const std::filesystem::path& path,std::string_view mode) {return fopen(path.string().c_str(),std::string(mode).c_str());}
}
namespace rex {
bool bit_scan_forward(uint32_t value,uint32_t* result) {*result=std::countr_zero(value);return value!=0;}
namespace memory {
void copy_and_swap_32_unaligned(void* dest,const void* src,size_t count) {
  for(size_t i=0;i<count;++i) {uint32_t value;memcpy(&value,static_cast<const char*>(src)+i*4,4);value=std::byteswap(value);memcpy(static_cast<char*>(dest)+i*4,&value,4);}
}
void copy_and_swap_32_aligned(void* dest,const void* src,size_t count) {copy_and_swap_32_unaligned(dest,src,count);}
}
}
namespace rex::graphics::draw_util {
const int8_t kD3D10StandardSamplePositions2x[2][2]={{4,4},{-4,-4}};
const int8_t kD3D10StandardSamplePositions4x[4][2]={{-2,-6},{6,-2},{-6,2},{2,6}};
}
int main() {
  using namespace rex::graphics;
  unsigned checks=0;
  auto check=[&](bool value) {++checks;if(!value) {fprintf(stderr,"Translator check %u failed\n",checks);std::abort();}};
  for (uint32_t version : {spv::Spv_1_0,spv::Spv_1_3,spv::Spv_1_5}) {
    for (int variant=0;variant<3;++variant) for (int stage=0;stage<3;++stage) {
      SpirvShaderTranslator::Features features(false);features.spirv_version=version;
      features.signed_zero_inf_nan_preserve_float32=true;
      features.denorm_flush_to_zero_float32=true;
      features.rounding_mode_rte_float32=true;
      SpirvShaderTranslator translator(features,true,true,false);
      std::vector<uint8_t> bytes;
      if(stage==2) bytes=translator.CreateDepthOnlyFragmentShader();
      else {
        const uint32_t export_dest=stage?0:62;
        // Three ALU instructions: c0+c1 -> r0, r0*c1 -> r1, export r1.
        // Variant 2 indexes the source register via aL, exercising a dynamic bank.
        const uint32_t ucode[]={1u|(3u<<12),2u<<12,0,
          15u<<16,0,1u<<8,
          1u|(15u<<16),0,(1u<<24)|(1u<<8)|(variant==2?0x40u<<16:0u)|(1u<<31),
          export_dest|(1u<<15)|(15u<<16),0,(2u<<24)|(1u<<16)|(1u<<8)|(3u<<30)};
        Shader shader(stage?xenos::ShaderType::kPixel:xenos::ShaderType::kVertex,0,
          variant?ucode:nullptr,variant?std::size(ucode):0,std::endian::native);
        rex::string::StringBuffer disasm;shader.AnalyzeUcode(disasm);
        const uint32_t registers=variant==2?8:0;
        const auto mod=stage?translator.GetDefaultPixelShaderModification(registers):translator.GetDefaultVertexShaderModification(registers);
        auto* translation=shader.GetOrCreateTranslation(mod);
        check(translator.TranslateAnalyzedShader(*translation));
        bytes=translation->translated_binary();
      }
      std::vector<uint32_t> out;vulkan::SpirvRegisterStats stats;std::string error;
      const auto words=std::span(reinterpret_cast<const uint32_t*>(bytes.data()),bytes.size()/4);
      // Regression: defaults and std430 alone reject the real translator's packed
      // system constants. Only the enabled scalar-layout feature admits them.
      check(!vulkan::OptimizeSpirvRegisters(words,out,stats,error,{}));check(out.empty());
      check(!vulkan::OptimizeSpirvRegisters(words,out,stats,error,{true,false}));check(out.empty());
      for (bool standard : {false,true}) {
        const bool ok=vulkan::OptimizeSpirvRegisters(words,out,stats,error,{standard,true});
        printf("version=%x stage=%d variant=%d standard=%d accepted=%d words=%zu/%zu loads=%u/%u locals=%u/%u reason=%s\n",version,stage,variant,standard,ok,words.size(),out.size(),stats.loads_before,stats.loads_after,stats.function_variables_before,stats.function_variables_after,error.c_str());
        check(ok);check(error.empty());
        if (version==spv::Spv_1_5 && variant==1 && stage!=2) check(stats.function_variables_after<stats.function_variables_before);
      }
    }
  }
  printf("%u actual-translator layout, optimization and dynamic-register checks passed\n",checks);
}
