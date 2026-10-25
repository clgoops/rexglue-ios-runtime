// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "spirv.h"
#include <rex/graphics/pipeline/shader/spirv.h>
#include <functional>
#include <mutex>
namespace rex::native {
// Native replacement for the SDK's VulkanShader subclass. Analysis, shader
// modifications and fetch/sampler records remain the SDK's own definitions.
// Preparation never creates a VkShaderModule or a Vulkan pipeline.
// Recreate the shader registry if backend feature/translation options change.
class RexShader final:public graphics::SpirvShader {
 public:
  using SpirvShader::SpirvShader;
  class NativeTranslation final:public SpirvTranslation {
   public:
    NativeTranslation(RexShader& shader,uint64_t modification):SpirvTranslation(shader,modification) {}
    bool Prepare(graphics::SpirvShaderTranslator&,const std::function<Identity()>& next_id,std::string& error);
    std::shared_ptr<const ConvertedShader> converted() const {std::lock_guard lock(preparation_mutex_);return converted_;}
   private:
    mutable std::mutex preparation_mutex_;
    std::shared_ptr<const ConvertedShader> converted_;
  };
 protected:
  graphics::Shader::Translation* CreateTranslationInstance(uint64_t modification) override;
};
} // namespace rex::native
