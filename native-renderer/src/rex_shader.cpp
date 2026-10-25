// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_shader.h"
#include <cstring>
namespace rex::native {
graphics::Shader::Translation* RexShader::CreateTranslationInstance(uint64_t modification) {
  return new NativeTranslation(*this,modification);
}
bool RexShader::NativeTranslation::Prepare(graphics::SpirvShaderTranslator& translator,
    const std::function<Identity()>& next_id,std::string& error) {
  error.clear();std::lock_guard lock(preparation_mutex_);
  if(converted_.load())return true;
  if(!shader().is_ucode_analyzed()) {error="native shader must be analyzed before preparation";return false;}
  if(!is_translated() && !translator.TranslateAnalyzedShader(*this)) {
    error="ReXGlue microcode translation failed";return false;
  }
  if(!is_valid()) {error="ReXGlue shader translation is invalid";return false;}
  Stage stage=Stage::Fragment;
  if(shader().type()==graphics::xenos::ShaderType::kVertex) {
    auto mod=graphics::SpirvShaderTranslator::Modification(modification());
    if(mod.vertex.host_vertex_shader_type==graphics::Shader::HostVertexShaderType::kMemExportCompute)
      stage=Stage::Compute;
    else if(graphics::Shader::IsHostVertexShaderTypeDomain(mod.vertex.host_vertex_shader_type)) {
      error="native tessellation requires explicit frontend lowering";return false;
    } else stage=Stage::Vertex;
  }
  const auto& binary=translated_binary();
  if(binary.size()%4) {error="translated SPIR-V byte length is invalid";return false;}
  std::vector<uint32_t> words(binary.size()/4);memcpy(words.data(),binary.data(),binary.size());
  ConvertedShader result;
  if(!ConvertSpirv(words,next_id(),stage,"main",result,error))return false;
  converted_.store(std::make_shared<const ConvertedShader>(std::move(result)));return true;
}
} // namespace rex::native
