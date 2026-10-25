// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "spirv.h"
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <functional>

namespace rex::native {
// CPU constants retain the SDK's own register map and system struct. This
// adapter neither imports Vulkan handles nor uses another title's bank layout.
class RexConstantState {
 public:
  using Map=graphics::Shader::ConstantRegisterMap;
  using System=graphics::SpirvShaderTranslator::SystemConstants;
  // Call on the command-processor thread with a stable register snapshot.
  // Only banks used by these compiled shaders are materialized. Identical bytes
  // reuse their immutable identity; changed data stays owned by older frames.
  bool Prepare(const System&,const graphics::RegisterFile&,const Map& vertex,const Map& pixel,
               std::span<const ConvertedShader* const> shaders,const std::function<Identity()>& next_id,
               Frame&,std::vector<DescriptorResources>& descriptors,std::string& error);
  void Reset();
 private:
  std::array<std::optional<Buffer>,graphics::SpirvShaderTranslator::kConstantBufferCount> banks_;
};
} // namespace rex::native
