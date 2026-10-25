// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "frame.h"

namespace rex::native {
enum class ResourceSource:uint8_t {Descriptor,PushConstants,BufferSizes};
enum class TextureLowering:uint8_t {None,OneDAsRow,LinearTexels};
struct ResourceMapping {
  ResourceSource source=ResourceSource::Descriptor;
  uint32_t descriptor_set=0,binding=0;
  Argument argument;
  // Original SPIR-V variable ID, for diagnostics and generated manifests only.
  uint32_t spirv_id=0;
  // SPIRV-Cross rewrites the corresponding image coordinates. The frontend
  // creates a 2D row for 1D textures, or a row-major 2D image for linear texels.
  // LinearTexels uses the bound image's actual width (no fixed row width).
  TextureLowering texture_lowering=TextureLowering::None;
  auto operator<=>(const ResourceMapping&) const=default;
};
struct ConvertedShader {
  Shader shader;
  std::vector<ResourceMapping> resources;
  std::array<uint32_t,3> workgroup_size{1,1,1};
};
struct SpirvOptions {
  // Reserve one discrete Metal buffer for OpArrayLength. No argument buffers
  // or implicit MoltenVK descriptor layout are used by this route.
  uint32_t buffer_size_slot=30;
  uint32_t max_texture_slots=128,max_sampler_slots=16;
};
// Converts one selected entry point. All bindings are assigned explicitly from
// reflected (set,binding) pairs. The output is cleared on failure; unsupported
// resource classes are errors rather than silently bound placeholders.
bool ConvertSpirv(std::span<const uint32_t>,Identity,Stage,std::string_view entry,
                  ConvertedShader&,std::string& error,const SpirvOptions& ={});

struct DescriptorResources {
  ResourceSource source=ResourceSource::Descriptor;
  uint32_t descriptor_set=0,binding=0;
  std::vector<BufferView> buffers;
  std::vector<SurfaceView> textures;
  std::vector<Identity> samplers;
};
// Materializes exactly the compiler's resource ABI. BufferSizes, when required,
// is generated from the bound buffer *view* lengths, not the whole allocation.
// No resource lifetime or guest-bank interpretation is hidden here.
bool BindConverted(const ConvertedShader&,std::span<const DescriptorResources>,
                   Identity size_buffer_id,Bindings&,std::optional<Buffer>& size_buffer,
                   std::string& error);
} // namespace rex::native
