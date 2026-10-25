/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Native adaptation: explicit host capabilities, no API shader modules.
#include "rex/native/rex_transfer_shader.h"
#include <rex/assert.h>
#include <rex/math.h>
#include <rex/graphics/pipeline/shader/spirv_builder.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <SPIRV/GLSL.std.450.h>
namespace rex::native {
namespace {
using namespace rex::graphics;
bool IsIntegerOwnership(xenos::ColorRenderTargetFormat f) {
  using F=xenos::ColorRenderTargetFormat;
  switch(f) {
    case F::k_16_16:case F::k_16_16_FLOAT:case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:case F::k_32_FLOAT:case F::k_32_32_FLOAT:return true;
    default:return false;
  }
}
struct Generator {
  enum TransferUsedDescriptorSet : uint32_t {
    // Ordered from the least to the most frequently changed.
    kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTextures,
    // Mutually exclusive with kTransferUsedDescriptorSetDepthStencilTextures.
    kTransferUsedDescriptorSetColorTexture,

    kTransferUsedDescriptorSetCount,

    kTransferUsedDescriptorSetHostDepthBufferBit = uint32_t(1)
                                                   << kTransferUsedDescriptorSetHostDepthBuffer,
    kTransferUsedDescriptorSetHostDepthStencilTexturesBit =
        uint32_t(1) << kTransferUsedDescriptorSetHostDepthStencilTextures,
    kTransferUsedDescriptorSetDepthStencilTexturesBit =
        uint32_t(1) << kTransferUsedDescriptorSetDepthStencilTextures,
    kTransferUsedDescriptorSetColorTextureBit = uint32_t(1)
                                                << kTransferUsedDescriptorSetColorTexture,
  };

  // 32-bit push constants (for simplicity of size calculation and to avoid
  // std140 packing issues).
  enum TransferUsedPushConstantDword : uint32_t {
    kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddress,
    // Changed 8 times per transfer.
    kTransferUsedPushConstantDwordStencilMask,

    kTransferUsedPushConstantDwordCount,

    kTransferUsedPushConstantDwordHostDepthAddressBit =
        uint32_t(1) << kTransferUsedPushConstantDwordHostDepthAddress,
    kTransferUsedPushConstantDwordAddressBit = uint32_t(1) << kTransferUsedPushConstantDwordAddress,
    kTransferUsedPushConstantDwordStencilMaskBit = uint32_t(1)
                                                   << kTransferUsedPushConstantDwordStencilMask,
  };

  enum class TransferPipelineLayoutIndex {
    kColor,
    kDepth,
    kColorToStencilBit,
    kDepthToStencilBit,
    kColorAndHostDepthTexture,
    kColorAndHostDepthBuffer,
    kDepthAndHostDepthTexture,
    kDepthAndHostDepthBuffer,

    kCount,
  };

  struct TransferPipelineLayoutInfo {
    uint32_t used_descriptor_sets;
    uint32_t used_push_constant_dwords;
  };

  static const TransferPipelineLayoutInfo
      kTransferPipelineLayoutInfos[size_t(TransferPipelineLayoutIndex::kCount)];

  enum class TransferMode : uint32_t {
    kColorToDepth,
    kColorToColor,

    kDepthToDepth,
    kDepthToColor,

    kColorToStencilBit,
    kDepthToStencilBit,

    // Two-source modes, using the host depth if it, when converted to the guest
    // format, matches what's in the owner source (not modified, keep host
    // precision), or the guest data otherwise (significantly modified, possibly
    // cleared). Stencil for FragStencilRef is always taken from the guest
    // source.

    kColorAndHostDepthToDepth,
    // When using different source and destination depth formats.
    kDepthAndHostDepthToDepth,

    // If host depth is fetched, but it's the same image as the destination,
    // it's copied to the EDRAM buffer (but since it's just a scratch buffer,
    // with tiles laid out linearly with the same pitch as in the original
    // render target; also no swapping of 40-sample columns as opposed to the
    // host render target - this is done only for the color source) and fetched
    // from there instead of the host depth texture.
    kColorAndHostDepthCopyToDepth,
    kDepthAndHostDepthCopyToDepth,

    kCount,
  };

  enum class TransferOutput {
    kColor,
    kDepth,
    kStencilBit,
  };

  struct TransferModeInfo {
    TransferOutput output;
    TransferPipelineLayoutIndex pipeline_layout;
  };

  static const TransferModeInfo kTransferModes[size_t(TransferMode::kCount)];

  union TransferShaderKey {
    uint32_t key;
    struct {
      xenos::MsaaSamples dest_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t dest_color_rt_index : xenos::kColorRenderTargetIndexBits;
      uint32_t dest_resource_format : xenos::kRenderTargetFormatBits;
      xenos::MsaaSamples source_msaa_samples : xenos::kMsaaSamplesBits;
      // Always 1x when the host depth is a copy from a buffer rather than an
      // image, not to create the same pipeline for different MSAA sample counts
      // as it doesn't matter in this case.
      xenos::MsaaSamples host_depth_source_msaa_samples : xenos::kMsaaSamplesBits;
      uint32_t source_resource_format : xenos::kRenderTargetFormatBits;

      // Last bits because this affects the pipeline layout - after sorting,
      // only change it as fewer times as possible. Depth buffers have an
      // additional stencil texture.
      static_assert(size_t(TransferMode::kCount) <= (size_t(1) << 4));
      TransferMode mode : 4;
    };

    TransferShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const TransferShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const TransferShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const TransferShaderKey& other_key) const { return !(*this == other_key); }
    bool operator<(const TransferShaderKey& other_key) const { return key < other_key.key; }
  };


  static bool IsColor16FormatFloatLike(xenos::ColorRenderTargetFormat f) {
    return f==xenos::ColorRenderTargetFormat::k_16_16_FLOAT ||
           f==xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT;
  }
  static std::vector<uint32_t> Generate(TransferShaderKey key,uint32_t scale_x,uint32_t scale_y);
};
const Generator::TransferPipelineLayoutInfo
    Generator::kTransferPipelineLayoutInfos[size_t(
        TransferPipelineLayoutIndex::kCount)] = {
        // kColor
        {kTransferUsedDescriptorSetColorTextureBit, kTransferUsedPushConstantDwordAddressBit},
        // kDepth
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit},
        // kColorToStencilBit
        {kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordAddressBit | kTransferUsedPushConstantDwordStencilMaskBit},
        // kDepthToStencilBit
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit | kTransferUsedPushConstantDwordStencilMaskBit},
        // kColorAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kColorAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit | kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
};

const Generator::TransferModeInfo
    Generator::kTransferModes[size_t(TransferMode::kCount)] = {
        // kColorToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColor},
        // kColorToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kColor},
        // kDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepth},
        // kDepthToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kDepth},
        // kColorToStencilBit
        {TransferOutput::kStencilBit, TransferPipelineLayoutIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {TransferOutput::kStencilBit, TransferPipelineLayoutIndex::kDepthToStencilBit},
        // kColorAndHostDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColorAndHostDepthTexture},
        // kDepthAndHostDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepthAndHostDepthTexture},
        // kColorAndHostDepthCopyToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColorAndHostDepthBuffer},
        // kDepthAndHostDepthCopyToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepthAndHostDepthBuffer},
};
std::vector<uint32_t> Generator::Generate(TransferShaderKey key,uint32_t scale_x,uint32_t scale_y) {
  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;

  SpirvBuilder builder(spv::Spv_1_0, (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint4 = builder.makeVectorType(type_uint, 4);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);

  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  const TransferPipelineLayoutInfo& pipeline_layout_info =
      kTransferPipelineLayoutInfos[size_t(mode.pipeline_layout)];

  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
  bool dest_is_color = (mode.output == TransferOutput::kColor);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp = dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color =
      (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_texture_component_mask;
  bool source_color_is_uint;
  spv::Id source_color_component_type;
  if (source_is_color) {
    assert_zero(pipeline_layout_info.used_descriptor_sets &
                kTransferUsedDescriptorSetDepthStencilTexturesBit);
    source_is_64bpp = xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (mode.output == TransferOutput::kStencilBit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_texture_component_mask =
            0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_texture_component_mask = 0b1;
      }
    } else {
      source_color_texture_component_mask =
          (uint32_t(1) << source_color_format_component_count) - 1;
    }
    source_color_is_uint = IsIntegerOwnership(source_color_format);
    source_color_component_type = source_color_is_uint ? type_uint : type_float;
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_texture_component_mask = 0;
    source_color_is_uint = false;
    source_color_component_type = spv::NoType;
  }

  std::vector<spv::Id> main_interface;

  // Outputs.
  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth &&
      true;
  bool dest_color_is_uint = false;
  uint32_t dest_color_component_count = 0;
  spv::Id type_fragment_data_component = spv::NoResult;
  spv::Id type_fragment_data = spv::NoResult;
  spv::Id output_fragment_data = spv::NoResult;
  spv::Id output_fragment_depth = spv::NoResult;
  spv::Id output_fragment_stencil_ref = spv::NoResult;
  switch (mode.output) {
    case TransferOutput::kColor:
      dest_color_is_uint = IsIntegerOwnership(dest_color_format);
      dest_color_component_count =
          xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
      type_fragment_data_component = dest_color_is_uint ? type_uint : type_float;
      type_fragment_data =
          dest_color_component_count > 1
              ? builder.makeVectorType(type_fragment_data_component, dest_color_component_count)
              : type_fragment_data_component;
      output_fragment_data =
          builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, type_fragment_data,
                                 "xe_transfer_fragment_data");
      builder.addDecoration(output_fragment_data, spv::DecorationLocation, key.dest_color_rt_index);
      main_interface.push_back(output_fragment_data);
      break;
    case TransferOutput::kDepth:
      output_fragment_depth = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                     type_float, "gl_FragDepth");
      builder.addDecoration(output_fragment_depth, spv::DecorationBuiltIn, spv::BuiltInFragDepth);
      main_interface.push_back(output_fragment_depth);
      if (shader_uses_stencil_reference_output) {
        builder.addExtension("SPV_EXT_shader_stencil_export");
        builder.addCapability(spv::CapabilityStencilExportEXT);
        output_fragment_stencil_ref = builder.createVariable(
            spv::NoPrecision, spv::StorageClassOutput, type_int, "gl_FragStencilRefARB");
        builder.addDecoration(output_fragment_stencil_ref, spv::DecorationBuiltIn,
                              spv::BuiltInFragStencilRefEXT);
        main_interface.push_back(output_fragment_stencil_ref);
      }
      break;
    default:
      break;
  }

  // Bindings.
  // Generating SPIR-V 1.0, no need to add bindings to the entry point's
  // interface until SPIR-V 1.4.
  // Color source.
  bool source_is_multisampled = key.source_msaa_samples != xenos::MsaaSamples::k1X;
  spv::Id source_color_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetColorTextureBit) {
    source_color_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(source_color_component_type, spv::Dim2D, false, false,
                              source_is_multisampled, 1, spv::ImageFormatUnknown),
        "xe_transfer_color");
    builder.addDecoration(source_color_texture, spv::DecorationDescriptorSet,
                          rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                                         (kTransferUsedDescriptorSetColorTextureBit - 1)));
    builder.addDecoration(source_color_texture, spv::DecorationBinding, 0);
  }
  // Depth / stencil source.
  spv::Id source_depth_texture = spv::NoResult;
  spv::Id source_stencil_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetDepthStencilTexturesBit) {
    uint32_t source_depth_stencil_descriptor_set =
        rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                       (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1));
    // Using `depth == false` in makeImageType because comparisons are not
    // required, and other values of `depth` are causing issues in drivers.
    // https://github.com/microsoft/DirectXShaderCompiler/issues/1107
    if (mode.output != TransferOutput::kStencilBit) {
      source_depth_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_float, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_depth");
      builder.addDecoration(source_depth_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_depth_texture, spv::DecorationBinding, 0);
    }
    if (mode.output != TransferOutput::kDepth || shader_uses_stencil_reference_output) {
      source_stencil_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_uint, spv::Dim2D, false, false, source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_stencil");
      builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
    }
  }
  // Host depth source buffer.
  spv::Id host_depth_source_buffer = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets & kTransferUsedDescriptorSetHostDepthBufferBit) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    // Storage buffers have std430 packing, no padding to 4-component vectors.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride, sizeof(uint32_t));
    spv::Id type_host_depth_source_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferHostDepthBuffer");
    builder.addMemberName(type_host_depth_source_buffer, 0, "host_depth");
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationNonWritable);
    builder.addMemberDecoration(type_host_depth_source_buffer, 0, spv::DecorationOffset, 0);
    // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // BufferBlock.
    builder.addDecoration(type_host_depth_source_buffer, spv::DecorationBufferBlock);
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    host_depth_source_buffer =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform,
                               type_host_depth_source_buffer, "xe_transfer_host_depth_buffer");
    builder.addDecoration(host_depth_source_buffer, spv::DecorationDescriptorSet,
                          rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                                         (kTransferUsedDescriptorSetHostDepthBufferBit - 1)));
    builder.addDecoration(host_depth_source_buffer, spv::DecorationBinding, 0);
  }
  // Host depth source texture (the depth / stencil descriptor set is reused,
  // but stencil is not needed).
  spv::Id host_depth_source_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    host_depth_source_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_float, spv::Dim2D, false, false,
                              key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X, 1,
                              spv::ImageFormatUnknown),
        "xe_transfer_host_depth");
    builder.addDecoration(
        host_depth_source_texture, spv::DecorationDescriptorSet,
        rex::bit_count(pipeline_layout_info.used_descriptor_sets &
                       (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)));
    builder.addDecoration(host_depth_source_texture, spv::DecorationBinding, 0);
  }
  // Push constants.
  id_vector_temp.clear();
  uint32_t push_constants_member_host_depth_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordHostDepthAddressBit) {
    push_constants_member_host_depth_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
    push_constants_member_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_stencil_mask = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordStencilMaskBit) {
    push_constants_member_stencil_mask = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  spv::Id push_constants = spv::NoResult;
  if (!id_vector_temp.empty()) {
    spv::Id type_push_constants = builder.makeStructType(id_vector_temp, "XeTransferPushConstants");
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordHostDepthAddressBit) {
      assert_true(push_constants_member_host_depth_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_host_depth_address,
                            "host_depth_address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_host_depth_address, spv::DecorationOffset,
          sizeof(uint32_t) *
              rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                             (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords & kTransferUsedPushConstantDwordAddressBit) {
      assert_true(push_constants_member_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_address, "address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_address, spv::DecorationOffset,
          sizeof(uint32_t) * rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                                            (kTransferUsedPushConstantDwordAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordStencilMaskBit) {
      assert_true(push_constants_member_stencil_mask != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_stencil_mask,
                            "stencil_mask");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_stencil_mask, spv::DecorationOffset,
          sizeof(uint32_t) * rex::bit_count(pipeline_layout_info.used_push_constant_dwords &
                                            (kTransferUsedPushConstantDwordStencilMaskBit - 1)));
    }
    builder.addDecoration(type_push_constants, spv::DecorationBlock);
    push_constants = builder.createVariable(spv::NoPrecision, spv::StorageClassPushConstant,
                                            type_push_constants, "xe_transfer_push_constants");
  }

  // Coordinate inputs.
  spv::Id input_fragment_coord =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput, type_float4, "gl_FragCoord");
  builder.addDecoration(input_fragment_coord, spv::DecorationBuiltIn, spv::BuiltInFragCoord);
  main_interface.push_back(input_fragment_coord);
  spv::Id input_sample_id = spv::NoResult;
  spv::Id spec_const_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (true) {
      // One draw for all samples.
      builder.addCapability(spv::CapabilitySampleRateShading);
      input_sample_id =
          builder.createVariable(spv::NoPrecision, spv::StorageClassInput, type_int, "gl_SampleID");
      builder.addDecoration(input_sample_id, spv::DecorationFlat);
      builder.addDecoration(input_sample_id, spv::DecorationBuiltIn, spv::BuiltInSampleId);
      main_interface.push_back(input_sample_id);
    } else {
      // One sample per draw, with different sample masks.
      spec_const_sample_id = builder.makeUintConstant(0, true);
      builder.addName(spec_const_sample_id, "xe_transfer_sample_id");
      builder.addDecoration(spec_const_sample_id, spv::DecorationSpecId, 0);
    }
  }

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function = builder.makeFunctionEntry(
      spv::NoPrecision, type_void, "main", main_param_types, main_precisions, &main_entry);

  // Working with unsigned numbers for simplicity now, bitcasting to signed will
  // be done at texture fetch.

  uint32_t tile_width_samples = xenos::kEdramTileWidthSamples * scale_x;
  uint32_t tile_height_samples = xenos::kEdramTileHeightSamples * scale_y;

  // Split the destination pixel index into 32bpp tile and 32bpp-tile-relative
  // pixel index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the pixel position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  uint_vector_temp.clear();
  uint_vector_temp.push_back(0);
  uint_vector_temp.push_back(1);
  spv::Id dest_pixel_coord = builder.createUnaryOp(
      spv::OpConvertFToU, type_uint2,
      builder.createRvalueSwizzle(spv::NoPrecision, type_float2,
                                  builder.createLoad(input_fragment_coord, spv::NoPrecision),
                                  uint_vector_temp));
  spv::Id dest_pixel_x = builder.createCompositeExtract(dest_pixel_coord, type_uint, 0);
  spv::Id const_dest_tile_width_pixels = builder.makeUintConstant(
      tile_width_samples >>
      (uint32_t(dest_is_64bpp) + uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X)));
  spv::Id dest_tile_index_x =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_tile_pixel_x =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_pixel_y = builder.createCompositeExtract(dest_pixel_coord, type_uint, 1);
  spv::Id const_dest_tile_height_pixels = builder.makeUintConstant(
      tile_height_samples >> uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X));
  spv::Id dest_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
  spv::Id dest_tile_pixel_y =
      builder.createBinOp(spv::OpUMod, type_uint, dest_pixel_y, const_dest_tile_height_pixels);

  assert_true(push_constants_member_address != UINT32_MAX);
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(int32_t(push_constants_member_address)));
  spv::Id address_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants, id_vector_temp),
      spv::NoPrecision);

  // Calculate the 32bpp tile index from its X and Y parts.
  spv::Id dest_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                              builder.makeUintConstant(0),
                              builder.makeUintConstant(xenos::kEdramPitchTilesBits)),
          dest_tile_index_y),
      dest_tile_index_x);

  // Load the destination sample index.
  spv::Id dest_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (true) {
      assert_true(input_sample_id != spv::NoResult);
      dest_sample_id = builder.createUnaryOp(spv::OpBitcast, type_uint,
                                             builder.createLoad(input_sample_id, spv::NoPrecision));
    } else {
      assert_true(spec_const_sample_id != spv::NoResult);
      // Already uint.
      dest_sample_id = spec_const_sample_id;
    }
  }

  // Transform the destination framebuffer pixel and sample coordinates into the
  // source texture pixel and sample coordinates.

  // First sample bit at 4x with Vulkan standard locations - horizontal sample.
  // Second sample bit at 4x with Vulkan standard locations - vertical sample.
  // At 2x:
  // - Native 2x: top is 1 in Vulkan, bottom is 0.
  // - 2x as 4x: top is 0, bottom is 3.

  spv::Id source_sample_id = dest_sample_id;
  spv::Id source_tile_pixel_x = dest_tile_pixel_x;
  spv::Id source_tile_pixel_y = dest_tile_pixel_y;
  spv::Id source_color_half = spv::NoResult;
  if (!source_is_64bpp && dest_is_64bpp) {
    // 32bpp -> 64bpp, need two samples of the source.
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 32bpp -> 64bpp, 4x ->.
      // Source has 32bpp halves in two adjacent samples.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 4x -> 4x.
        // 1 destination horizontal sample = 2 source horizontal samples.
        // D p0,0 s0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1,0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p0,0 s0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p0,0 s1,1 = S p1,0 s0,1 | S p1,0 s1,1
        // Thus destination horizontal sample -> source horizontal pixel,
        // vertical samples are 1:1.
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1 << 1));
        source_tile_pixel_x = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                   dest_tile_pixel_x, builder.makeUintConstant(1),
                                                   builder.makeUintConstant(31));
      } else if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
        // 32bpp -> 64bpp, 4x -> 2x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 s0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p1,0 s0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p1,0 s1 = S p1,0 s0,1 | S p1,0 s1,1
        // Pixel index can be reused. Sample 1 (for native 2x) or 0 (for 2x as
        // 4x) should become samples 01, sample 0 or 3 should become samples 23.
        if (true) {
          source_sample_id =
              builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                  builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                                      builder.makeUintConstant(1)),
                                  builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                                 builder.makeUintConstant(1 << 1));
        }
      } else {
        // 32bpp -> 64bpp, 4x -> 1x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // Horizontal pixel index can be reused. Vertical pixel 1 should
        // become sample 2.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, builder.makeUintConstant(0), dest_tile_pixel_y,
            builder.makeUintConstant(1), builder.makeUintConstant(1));
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      }
    } else {
      // 32bpp -> 64bpp, 1x/2x ->.
      // Source has 32bpp halves in two adjacent pixels.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 1x/2x -> 4x.
        // The X part.
        // 1 destination horizontal sample = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(2)),
            dest_sample_id, builder.makeUintConstant(1), builder.makeUintConstant(1));
        // Y is handled by common code.
      } else {
        // 32bpp -> 64bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 1 destination horizontal pixel = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    // 64bpp -> 32bpp, also the half to load.
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 64bpp -> 32bpp, -> 4x.
      // The needed half is in the destination horizontal sample index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 4x.
        // D p0,0 s0,0 = S s0,0 low
        // D p0,0 s1,0 = S s0,0 high
        // D p1,0 s0,0 = S s1,0 low
        // D p1,0 s1,0 = S s1,0 high
        // Vertical pixel and sample (second bit) addressing is the same.
        // However, 1 horizontal destination pixel = 1 horizontal source sample.
        source_sample_id = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, dest_sample_id,
                                                dest_tile_pixel_x, builder.makeUintConstant(0),
                                                builder.makeUintConstant(1));
        // 2 destination horizontal samples = 1 source horizontal sample, thus
        // 2 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 4x.
        // 2 destination horizontal samples = 1 source horizontal pixel, thus
        // 1 destination horizontal pixel = 1 source horizontal pixel. Can reuse
        // horizontal pixel index.
        // Y is handled by common code.
      }
      // Half from the destination horizontal sample index.
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1));
    } else {
      // 64bpp -> 32bpp, -> 1x/2x.
      // The needed half is in the destination horizontal pixel index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 1x/2x.
        // (Destination horizontal pixel >> 1) & 1 = source horizontal sample
        // (first bit).
        source_sample_id =
            builder.createTriOp(spv::OpBitFieldUExtract, type_uint, dest_tile_pixel_x,
                                builder.makeUintConstant(1), builder.makeUintConstant(1));
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // 64bpp -> 32bpp, 4x -> 2x.
          // Destination vertical samples (1/0 in the first bit for native 2x or
          // 0/1 in the second bit for 2x as 4x) = source vertical samples
          // (second bit).
          if (true) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, source_sample_id,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(1));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, source_sample_id,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
        } else {
          // 64bpp -> 32bpp, 4x -> 1x.
          // 1 destination vertical pixel = 1 source vertical sample.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_tile_pixel_y,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
        // 2 destination horizontal pixels = 1 source horizontal sample.
        // 4 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(2));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 2 destination horizontal pixels = 1 destination source pixel.
        source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
      // Half from the destination horizontal pixel index.
      source_color_half = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                              builder.makeUintConstant(1));
    }
    assert_true(source_color_half != spv::NoResult);
  } else {
    // Same bit count.
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // Same BPP, 4x -> 1x/2x.
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // Same BPP, 4x -> 2x.
          // Horizontal pixels to samples. Vertical sample (1/0 in the first bit
          // for native 2x or 0/1 in the second bit for 2x as 4x) to second
          // sample bit.
          if (true) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(31));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                builder.makeUintConstant(0), builder.makeUintConstant(1));
          }
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
        } else {
          // Same BPP, 4x -> 1x.
          // Pixels to samples.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
          source_tile_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_x, builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                    dest_tile_pixel_y, builder.makeUintConstant(1));
        }
      } else {
        // Same BPP, 1x/2x -> 1x/2x/4x (as long as they're different).
        // Only the X part - Y is handled by common code.
        if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
          // Horizontal samples to pixels.
          source_tile_pixel_x = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
              builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }
  // Common source Y and sample index for 1x/2x AA sources, independent of bits
  // per sample.
  if (key.source_msaa_samples < xenos::MsaaSamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 1x/2x -> 4x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 4x.
        // Vertical samples (second bit) of 4x destination to vertical sample
        // (1, 0 for native 2x, or 0, 3 for 2x as 4x) of 2x source.
        source_sample_id = builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                               builder.makeUintConstant(1));
        if (true) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
      } else {
        // 1x -> 4x.
        // Vertical samples (second bit) to Y pixels.
        source_tile_pixel_y = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                builder.makeUintConstant(1)),
            dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
      }
    } else {
      // 1x/2x -> different 1x/2x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 1x.
        // Vertical pixels of 2x destination to vertical samples (1, 0 for
        // native 2x, or 0, 3 for 2x as 4x) of 1x source.
        source_sample_id = builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                               builder.makeUintConstant(1));
        if (true) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint, source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id, source_sample_id,
              builder.makeUintConstant(1), builder.makeUintConstant(1));
        }
        source_tile_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                                  dest_tile_pixel_y, builder.makeUintConstant(1));
      } else {
        // 1x -> 2x.
        // Vertical samples (1/0 in the first bit for native 2x or 0/1 in the
        // second bit for 2x as 4x) of 2x destination to vertical pixels of 1x
        // source.
        if (true) {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        } else {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
        }
      }
    }
  }

  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) + uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile.
    uint32_t source_32bpp_tile_half_pixels =
        tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    source_tile_pixel_x = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int, source_tile_pixel_x),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(), source_tile_pixel_x,
                                    builder.makeUintConstant(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(int32_t(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(-int32_t(source_32bpp_tile_half_pixels)))));
  }

  // Transform the destination 32bpp tile index into the source. After the
  // addition, it may be negative - in which case, the transfer is done across
  // EDRAM addressing wrapping, and xenos::kEdramTileCount must be added to it,
  // but `& (xenos::kEdramTileCount - 1)` handles that regardless of the sign.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpBitwiseAnd, type_uint,
      builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
              builder.createTriOp(spv::OpBitFieldSExtract, type_int,
                                  builder.createUnaryOp(spv::OpBitcast, type_int, address_constant),
                                  builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                                  builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
      builder.makeUintConstant(xenos::kEdramTileCount - 1));
  // Split the source 32bpp tile index into X and Y tile index within the source
  // image.
  spv::Id source_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                          builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                          builder.makeUintConstant(xenos::kEdramPitchTilesBits));
  spv::Id source_tile_index_y =
      builder.createBinOp(spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x =
      builder.createBinOp(spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Finally calculate the source texture coordinates.
  spv::Id source_pixel_x_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_width_samples >> source_pixel_width_dwords_log2),
              source_tile_index_x),
          source_tile_pixel_x));
  spv::Id source_pixel_y_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_height_samples >> uint32_t(key.source_msaa_samples >=
                                                                       xenos::MsaaSamples::k2X)),
              source_tile_index_y),
          source_tile_pixel_y));

  // Load the source.

  spv::Builder::TextureParameters source_texture_parameters = {};
  id_vector_temp.clear();
  id_vector_temp.push_back(source_pixel_x_int);
  id_vector_temp.push_back(source_pixel_y_int);
  spv::Id source_coordinates[2] = {
      builder.createCompositeConstruct(type_int2, id_vector_temp),
  };
  spv::Id source_sample_ids_int[2] = {};
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    source_sample_ids_int[0] = builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  // Go to the next sample or pixel along X if need to load two dwords.
  bool source_load_is_two_32bpp_samples = !source_is_64bpp && dest_is_64bpp;
  if (source_load_is_two_32bpp_samples) {
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      source_coordinates[1] = source_coordinates[0];
      source_sample_ids_int[1] = builder.createBinOp(
          spv::OpBitwiseOr, type_int, source_sample_ids_int[0], builder.makeIntConstant(1));
    } else {
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.createBinOp(spv::OpBitwiseOr, type_int, source_pixel_x_int,
                                                   builder.makeIntConstant(1)));
      id_vector_temp.push_back(source_pixel_y_int);
      source_coordinates[1] = builder.createCompositeConstruct(type_int2, id_vector_temp);
      source_sample_ids_int[1] = source_sample_ids_int[0];
    }
  }
  spv::Id source_color[2][4] = {};
  if (source_color_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_color_texture, spv::NoPrecision);
    assert_true(source_color_component_type != spv::NoType);
    spv::Id source_color_vec4_type = builder.makeVectorType(source_color_component_type, 4);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      spv::Id source_color_vec4 = builder.createTextureCall(
          spv::NoPrecision, source_color_vec4_type, false, true, false, false, false,
          source_texture_parameters, spv::ImageOperandsMaskNone);
      uint32_t source_color_components_remaining = source_color_texture_component_mask;
      uint32_t source_color_component_index;
      while (
          rex::bit_scan_forward(source_color_components_remaining, &source_color_component_index)) {
        source_color_components_remaining &= ~(uint32_t(1) << source_color_component_index);
        source_color[i][source_color_component_index] = builder.createCompositeExtract(
            source_color_vec4, source_color_component_type, source_color_component_index);
      }
    }
  }
  spv::Id source_depth_float[2] = {};
  if (source_depth_texture != spv::NoResult) {
    source_texture_parameters.sampler = builder.createLoad(source_depth_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_depth_float[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_float4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_float, 0);
    }
  }
  spv::Id source_stencil[2] = {};
  if (source_stencil_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_stencil[i] = builder.createCompositeExtract(
          builder.createTextureCall(spv::NoPrecision, type_uint4, false, true, false, false, false,
                                    source_texture_parameters, spv::ImageOperandsMaskNone),
          type_uint, 0);
    }
  }

  // Pick the needed 32bpp half of the 64bpp color.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count = source_color_format_component_count >> 1;
    assert_true(source_color_half != spv::NoResult);
    spv::Id source_color_is_second_half = builder.createBinOp(
        spv::OpINotEqual, type_bool, source_color_half, builder.makeUintConstant(0));
    if (mode.output == TransferOutput::kStencilBit) {
      source_color[0][0] = builder.createTriOp(
          spv::OpSelect, source_color_component_type, source_color_is_second_half,
          source_color[0][source_color_half_component_count], source_color[0][0]);
    } else {
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        source_color[0][i] = builder.createTriOp(
            spv::OpSelect, source_color_component_type, source_color_is_second_half,
            source_color[0][source_color_half_component_count + i], source_color[0][i]);
      }
    }
  }

  if (output_fragment_stencil_ref != spv::NoResult && source_stencil[0] != spv::NoResult) {
    // For the depth -> depth case, write the stencil directly to the output.
    assert_true(mode.output == TransferOutput::kDepth);
    builder.createStore(builder.createUnaryOp(spv::OpBitcast, type_int, source_stencil[0]),
                        output_fragment_stencil_ref);
  }

  const bool source_color_16_is_float = IsColor16FormatFloatLike(source_color_format);
  const bool dest_color_16_is_float = IsColor16FormatFloatLike(dest_color_format);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_uint_16 = builder.makeUintConstant(16);
  spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
  spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
  spv::Id const_float_minus_1 = builder.makeFloatConstant(-1.0f);
  spv::Id const_float_32767 = builder.makeFloatConstant(32767.0f);
  spv::Id const_float_inv_32767 = builder.makeFloatConstant(1.0f / 32767.0f);
  auto PWLGammaToLinear = [&](spv::Id gamma, bool gamma_pre_saturated) -> spv::Id {
    if (!gamma_pre_saturated) {
      gamma = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                           gamma, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(192.0f / 255.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(8.0f / 1024.0f),
                                               builder.makeFloatConstant(4.0f / 1024.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(-1024.0f),
                                                builder.makeFloatConstant(-256.0f));
    spv::Id is_piece_at_least_1 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(2.0f / 1024.0f),
                                               builder.makeFloatConstant(1.0f / 1024.0f));
    spv::Id offset_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                                builder.makeFloatConstant(-64.0f), const_float_0);
    spv::Id is_piece_at_least_2 = builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, gamma,
                                                      builder.makeFloatConstant(96.0f / 255.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    spv::Id linear = builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createBinOp(spv::OpFMul, type_float, gamma,
                                                builder.makeFloatConstant(255.0f * 1024.0f)),
                            scale),
        offset);
    linear = builder.createBinOp(spv::OpFAdd, type_float, linear,
                                 builder.createUnaryBuiltinCall(
                                     type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                     builder.createBinOp(spv::OpFMul, type_float, linear, scale)));
    return builder.createBinOp(spv::OpFMul, type_float, linear,
                               builder.makeFloatConstant(1.0f / 1023.0f));
  };
  auto LinearToPWLGamma = [&](spv::Id linear, bool linear_pre_saturated) -> spv::Id {
    if (!linear_pre_saturated) {
      linear = builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                            linear, const_float_0, const_float_1);
    }
    spv::Id is_piece_at_least_3 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, linear,
                            builder.makeFloatConstant(512.0f / 1023.0f));
    spv::Id scale_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                               builder.makeFloatConstant(1023.0f / 8.0f),
                                               builder.makeFloatConstant(1023.0f / 4.0f));
    spv::Id offset_3_or_2 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_3,
                                                builder.makeFloatConstant(128.0f / 255.0f),
                                                builder.makeFloatConstant(64.0f / 255.0f));
    spv::Id is_piece_at_least_1 = builder.createBinOp(
        spv::OpFOrdGreaterThanEqual, type_bool, linear, builder.makeFloatConstant(64.0f / 1023.0f));
    spv::Id scale_1_or_0 = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                                               builder.makeFloatConstant(1023.0f / 2.0f),
                                               builder.makeFloatConstant(1023.0f));
    spv::Id offset_1_or_0 =
        builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_1,
                            builder.makeFloatConstant(32.0f / 255.0f), const_float_0);
    spv::Id is_piece_at_least_2 =
        builder.createBinOp(spv::OpFOrdGreaterThanEqual, type_bool, linear,
                            builder.makeFloatConstant(128.0f / 1023.0f));
    spv::Id scale = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                        scale_3_or_2, scale_1_or_0);
    spv::Id offset = builder.createTriOp(spv::OpSelect, type_float, is_piece_at_least_2,
                                         offset_3_or_2, offset_1_or_0);
    return builder.createBinOp(
        spv::OpFAdd, type_float,
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryBuiltinCall(
                                type_float, ext_inst_glsl_std_450, GLSLstd450Trunc,
                                builder.createBinOp(spv::OpFMul, type_float, linear, scale)),
                            builder.makeFloatConstant(1.0f / 255.0f)),
        offset);
  };
  auto PackSource16ComponentToUint = [&](spv::Id component) -> spv::Id {
    if (source_color_is_uint) {
      return component;
    }
    if (source_color_16_is_float) {
      id_vector_temp.clear();
      id_vector_temp.push_back(component);
      id_vector_temp.push_back(const_float_0);
      spv::Id packed_half = builder.createUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
          builder.createCompositeConstruct(type_float2, id_vector_temp));
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_half, const_uint_0,
                                 const_uint_16);
    }
    spv::Id component_clamped =
        builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, component,
                                     const_float_minus_1, const_float_1);
    spv::Id component_rounded = builder.createUnaryBuiltinCall(
        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
        builder.createBinOp(spv::OpFMul, type_float, component_clamped, const_float_32767));
    spv::Id component_snorm =
        builder.createUnaryOp(spv::OpConvertFToS, type_int, component_rounded);
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                               builder.createUnaryOp(spv::OpBitcast, type_uint, component_snorm),
                               const_uint_0, const_uint_16);
  };
  auto PackSource16PairToUint32 = [&](spv::Id component_0, spv::Id component_1) -> spv::Id {
    return builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, PackSource16ComponentToUint(component_0),
        PackSource16ComponentToUint(component_1), const_uint_16, const_uint_16);
  };
  auto UnpackDest16ComponentFromUint32 = [&](spv::Id packed_word,
                                             uint32_t component_index) -> spv::Id {
    spv::Id component_offset = component_index & 1 ? const_uint_16 : const_uint_0;
    if (dest_color_is_uint) {
      return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed_word, component_offset,
                                 const_uint_16);
    }
    if (dest_color_16_is_float) {
      spv::Id component_pair = builder.createUnaryBuiltinCall(
          type_float2, ext_inst_glsl_std_450, GLSLstd450UnpackHalf2x16, packed_word);
      return builder.createCompositeExtract(component_pair, type_float, component_index & 1);
    }
    spv::Id component_snorm = builder.createTriOp(spv::OpBitFieldSExtract, type_int, packed_word,
                                                  component_offset, const_uint_16);
    spv::Id component_float =
        builder.createBinOp(spv::OpFMul, type_float,
                            builder.createUnaryOp(spv::OpConvertSToF, type_float, component_snorm),
                            const_float_inv_32767);
    return builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                        component_float, const_float_minus_1, const_float_1);
  };

  if (dest_is_64bpp) {
    // Construct the 64bpp color from two 32-bit samples or one 64-bit sample.
    // If `packed` (two uints) are created, use the generic path involving
    // unpacking.
    // Otherwise, the fragment data output must be written to directly by the
    // reached control flow path.
    spv::Id packed[2] = {};
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (false) {
            // 8_8_8_8_GAMMA is represented by linear stored in
            // R16G16B16A16_UNORM.
            for (uint32_t i = 0; i < 2; ++i) {
              for (uint32_t j = 0; j < 3; ++j) {
                source_color[i][j] = LinearToPWLGamma(source_color[i][j], true);
              }
            }
          }
        }
          [[fallthrough]];
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id component_width = builder.makeUintConstant(8);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0], unorm_scale),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(spv::OpFAdd, type_float,
                                          builder.createBinOp(spv::OpFMul, type_float,
                                                              source_color[i][j], unorm_scale),
                                          unorm_round_offset)),
                  builder.makeUintConstant(8 * j), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[i][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[i][j],
                                              j == 3 ? unorm_scale_a : unorm_scale_rgb),
                          unorm_round_offset)),
                  builder.makeUintConstant(10 * j), j == 3 ? width_a : width_rgb);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id float_0 = builder.makeFloatConstant(0.0f);
          spv::Id float_1 = builder.makeFloatConstant(1.0f);
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id offset_a = builder.makeUintConstant(30);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed[i] = SpirvShaderTranslator::UnclampedFloat32To7e3(builder, source_color[i][0],
                                                                     ext_inst_glsl_std_450);
            for (uint32_t j = 1; j < 3; ++j) {
              packed[i] =
                  builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed[i],
                                       SpirvShaderTranslator::UnclampedFloat32To7e3(
                                           builder, source_color[i][j], ext_inst_glsl_std_450),
                                       builder.makeUintConstant(10 * j), width_rgb);
            }
            // Saturate and convert the alpha.
            spv::Id alpha_saturated =
                builder.createTriBuiltinCall(type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                                             source_color[i][3], float_0, float_1);
            packed[i] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[i],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(spv::OpFAdd, type_float,
                                        builder.createBinOp(spv::OpFMul, type_float,
                                                            alpha_saturated, unorm_scale_a),
                                        unorm_round_offset)),
                offset_a, width_a);
          }
        } break;
        // Route through packed uint32 words so mixed source/destination
        // transfer component types (integer vs float fallback) are handled
        // uniformly.
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = PackSource16PairToUint32(source_color[i][0], source_color[i][1]);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] =
                PackSource16PairToUint32(source_color[0][i << 1], source_color[0][(i << 1) + 1]);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[i][0];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[0][i];
            if (!source_color_is_uint) {
              packed[i] = builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
      }
    } else {
      assert_true(source_depth_texture != spv::NoResult);
      assert_true(source_stencil_texture != spv::NoResult);
      spv::Id depth_offset = builder.makeUintConstant(8);
      spv::Id depth_width = builder.makeUintConstant(24);
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Id depth24 = spv::NoResult;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the
            // correct conversion, adding +0.5 and rounding towards zero results
            // in red instead of black in the 4D5307E6 clear shader.
            depth24 = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[i],
                                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[i], false, true, ext_inst_glsl_std_450);
          } break;
        }
        // Merge depth and stencil.
        packed[i] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[i],
                                         depth24, depth_offset, depth_width);
      }
    }
    // Common path unless there was a specialized one - unpack two packed 32-bit
    // parts.
    if (packed[0] != spv::NoResult) {
      assert_true(packed[1] != spv::NoResult);
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        id_vector_temp.clear();
        id_vector_temp.push_back(packed[0]);
        id_vector_temp.push_back(packed[1]);
        // If integer transfer formats are unavailable for this sample count,
        // ownership transfer falls back to float formats and raw bits are
        // passed via bitcasts.
        if (!dest_color_is_uint) {
          for (spv::Id& float32 : id_vector_temp) {
            float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
          }
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      } else {
        id_vector_temp.clear();
        for (uint32_t i = 0; i < 4; ++i) {
          id_vector_temp.push_back(UnpackDest16ComponentFromUint32(packed[i >> 1], i));
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                            output_fragment_data);
      }
    }
  } else {
    // If `packed` is created, use the generic path involving unpacking.
    // - For a color destination, the packed 32bpp color.
    // - For a depth / stencil destination, stencil in 0:7, depth in 8:31
    //   normally, or depth in 0:23 and zeros in 24:31 with packed_only_depth.
    // - For a stencil bit, stencil in 0:7.
    // Otherwise, the fragment data or fragment depth / stencil output must be
    // written to directly by the reached control flow path.
    spv::Id packed = spv::NoResult;
    bool packed_only_depth = false;
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (mode.output == TransferOutput::kStencilBit) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                false) {
              source_color[0][0] = LinearToPWLGamma(source_color[0][0], true);
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
          } else if (dest_is_color &&
                     (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
                      dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            if (source_color_format != dest_color_format) {
              // Color space conversion between k_8_8_8_8 and
              // k_8_8_8_8_GAMMA.
              if (dest_color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) {
                for (uint32_t i = 0; i < 3; ++i) {
                  source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
                }
              } else {
                for (uint32_t i = 0; i < 3; ++i) {
                  source_color[0][i] = PWLGammaToLinear(source_color[0][i], true);
                }
              }
            }
            // Same or converted format - passthrough.
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else if (mode.output == TransferOutput::kDepth) {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                false) {
              for (uint32_t i = output_fragment_stencil_ref != spv::NoResult ? 0 : 1; i < 3; ++i) {
                source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
              }
            }
            // When need only depth, not stencil, skip the red component.
            packed_only_depth = true;
            if (output_fragment_stencil_ref != spv::NoResult) {
              // Write the red component to the stencil reference.
              builder.createStore(
                  builder.createUnaryOp(
                      spv::OpBitcast, type_int,
                      builder.createUnaryOp(
                          spv::OpConvertFToU, type_uint,
                          builder.createBinOp(
                              spv::OpFAdd, type_float,
                              builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                  builder.makeFloatConstant(255.0f)),
                              builder.makeFloatConstant(0.5f)))),
                  output_fragment_stencil_ref);
            }
            // Put depth in 0:23.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][1],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (uint32_t i = 2; i < 4; ++i) {
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                              builder.makeFloatConstant(255.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(8 * (i - 1)), component_width);
            }
          } else {
            if (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                false) {
              for (uint32_t i = 0; i < 3; ++i) {
                source_color[0][i] = LinearToPWLGamma(source_color[0][i], true);
              }
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        builder.makeFloatConstant(255.0f)),
                                    builder.makeFloatConstant(0.5f)));
            spv::Id component_width = builder.makeUintConstant(8);
            for (uint32_t i = 1; i < 4; ++i) {
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                              builder.makeFloatConstant(255.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(8 * i), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
               dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(spv::OpFAdd, type_float,
                                    builder.createBinOp(spv::OpFMul, type_float, source_color[0][0],
                                                        unorm_scale_rgb),
                                    unorm_round_offset));
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              for (uint32_t i = 1; i < 4; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(spv::OpFMul, type_float, source_color[0][i],
                                                i == 3 ? unorm_scale_a : unorm_scale_rgb),
                            unorm_round_offset)),
                    builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(
                builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                output_fragment_data);
          } else {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed = SpirvShaderTranslator::UnclampedFloat32To7e3(builder, source_color[0][0],
                                                                  ext_inst_glsl_std_450);
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 1; i < 3; ++i) {
                packed =
                    builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                         SpirvShaderTranslator::UnclampedFloat32To7e3(
                                             builder, source_color[0][i], ext_inst_glsl_std_450),
                                         builder.makeUintConstant(10 * i), width_rgb);
              }
              // Saturate and convert the alpha.
              spv::Id alpha_saturated = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp, source_color[0][3],
                  builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float, alpha_saturated,
                                              builder.makeFloatConstant(3.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(30), builder.makeUintConstant(2));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          // Route through packed uint32 so destination transfer type (integer
          // vs float fallback) is reconstructed consistently.
          packed = PackSource16ComponentToUint(source_color[0][0]);
          if (mode.output != TransferOutput::kStencilBit) {
            packed = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, packed,
                                          PackSource16ComponentToUint(source_color[0][1]),
                                          const_uint_16, const_uint_16);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          packed = source_color[0][0];
          if (!source_color_is_uint) {
            packed = builder.createUnaryOp(spv::OpBitcast, type_uint, packed);
          }
        } break;
      }
    } else if (source_depth_float[0] != spv::NoResult) {
      if (mode.output == TransferOutput::kDepth && dest_depth_format == source_depth_format) {
        builder.createStore(source_depth_float[0], output_fragment_depth);
      } else {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(spv::OpFMul, type_float, source_depth_float[0],
                                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            packed = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[0], false, true, ext_inst_glsl_std_450);
          } break;
        }
        if (mode.output == TransferOutput::kDepth) {
          packed_only_depth = true;
        } else {
          // Merge depth and stencil.
          packed = builder.createQuadOp(spv::OpBitFieldInsert, type_uint, source_stencil[0], packed,
                                        builder.makeUintConstant(8), builder.makeUintConstant(24));
        }
      }
    }
    switch (mode.output) {
      case TransferOutput::kColor: {
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output.
        if (packed != spv::NoResult) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              spv::Id component_width = builder.makeUintConstant(8);
              spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed,
                                            builder.makeUintConstant(8 * i), component_width)),
                    unorm_scale));
              }
              if (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA &&
                  false) {
                // 8_8_8_8_GAMMA is represented by linear stored in
                // R16G16B16A16_UNORM.
                for (uint32_t i = 0; i < 3; ++i) {
                  id_vector_temp[i] = PWLGammaToLinear(id_vector_temp[i], true);
                }
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_rgb = builder.makeFloatConstant(1.0f / 1023.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              spv::Id unorm_scale_a = builder.makeFloatConstant(1.0f / 3.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint, packed,
                                            builder.makeUintConstant(10 * i),
                                            i == 3 ? width_a : width_rgb)),
                    i == 3 ? unorm_scale_a : unorm_scale_rgb));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              id_vector_temp.clear();
              // Color.
              for (uint32_t i = 0; i < 3; ++i) {
                id_vector_temp.push_back(SpirvShaderTranslator::Float7e3To32(
                    builder, packed, 10 * i, false, ext_inst_glsl_std_450));
              }
              // Alpha.
              id_vector_temp.push_back(builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(spv::OpConvertUToF, type_float,
                                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                                            packed, builder.makeUintConstant(30),
                                                            builder.makeUintConstant(2))),
                  builder.makeFloatConstant(1.0f / 3.0f)));
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 2; ++i) {
                id_vector_temp.push_back(UnpackDest16ComponentFromUint32(packed, i));
              }
              builder.createStore(
                  builder.createCompositeConstruct(type_fragment_data, id_vector_temp),
                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Float32 is transferred as uint32 to preserve NaN encodings.
              // If the integer transfer format is unavailable for this sample
              // count, the transfer format is float and this bitcast path is
              // used.
              spv::Id float32 = packed;
              if (!dest_color_is_uint) {
                float32 = builder.createUnaryOp(spv::OpBitcast, type_float, float32);
              }
              builder.createStore(float32, output_fragment_data);
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
      } break;
      case TransferOutput::kDepth: {
        if (packed) {
          spv::Id guest_depth24 = packed;
          if (!packed_only_depth) {
            // Extract the depth bits.
            guest_depth24 = builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                                builder.makeUintConstant(8));
          }
          // Load the host float32 depth, check if, when converted to the guest
          // format, it's the same as the guest source, thus up to date, and if
          // it is, write host float32 depth, otherwise do the guest -> host
          // conversion.
          spv::Id host_depth32 = spv::NoResult;
          if (host_depth_source_texture != spv::NoResult) {
            // Convert position and sample index from within the destination
            // tile to within the host depth source tile, like for the guest
            // render target, but for 32bpp -> 32bpp only.
            spv::Id host_depth_source_sample_id = dest_sample_id;
            spv::Id host_depth_source_tile_pixel_x = dest_tile_pixel_x;
            spv::Id host_depth_source_tile_pixel_y = dest_tile_pixel_y;
            if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
              if (key.host_depth_source_msaa_samples >= xenos::MsaaSamples::k4X) {
                // 4x -> 1x/2x.
                if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
                  // 4x -> 2x.
                  // Horizontal pixels to samples. Vertical sample (1/0 in the
                  // first bit for native 2x or 0/1 in the second bit for 2x as
                  // 4x) to second sample bit.
                  if (true) {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                        builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        builder.makeUintConstant(1), builder.makeUintConstant(31));
                  } else {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                        builder.makeUintConstant(0), builder.makeUintConstant(1));
                  }
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                } else {
                  // 4x -> 1x.
                  // Pixels to samples.
                  host_depth_source_sample_id = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint,
                      builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1)),
                      dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_x =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                                          builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_y =
                      builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                          builder.makeUintConstant(1));
                }
              } else {
                // 1x/2x -> 1x/2x/4x (as long as they're different).
                // Only the X part - Y is handled by common code.
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // Horizontal samples to pixels.
                  host_depth_source_tile_pixel_x = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                      builder.makeUintConstant(1), builder.makeUintConstant(31));
                }
              }
              // Host depth source Y and sample index for 1x/2x AA sources.
              if (key.host_depth_source_msaa_samples < xenos::MsaaSamples::k4X) {
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // 1x/2x -> 4x.
                  if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 2x -> 4x.
                    // Vertical samples (second bit) of 4x destination to
                    // vertical sample (1, 0 for native 2x, or 0, 3 for 2x as
                    // 4x) of 2x source.
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1));
                    if (true) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                  } else {
                    // 1x -> 4x.
                    // Vertical samples (second bit) to Y pixels.
                    host_depth_source_tile_pixel_y = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint,
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                            builder.makeUintConstant(1)),
                        dest_tile_pixel_y, builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  }
                } else {
                  // 1x/2x -> different 1x/2x.
                  if (key.host_depth_source_msaa_samples == xenos::MsaaSamples::k2X) {
                    // 2x -> 1x.
                    // Vertical pixels of 2x destination to vertical samples (1,
                    // 0 for native 2x, or 0, 3 for 2x as 4x) of 1x source.
                    host_depth_source_sample_id =
                        builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                    if (true) {
                      host_depth_source_sample_id = builder.createBinOp(
                          spv::OpBitwiseXor, type_uint, host_depth_source_sample_id,
                          builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint, host_depth_source_sample_id,
                          host_depth_source_sample_id, builder.makeUintConstant(1),
                          builder.makeUintConstant(1));
                    }
                    host_depth_source_tile_pixel_y =
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                                            builder.makeUintConstant(1));
                  } else {
                    // 1x -> 2x.
                    // Vertical samples (1/0 in the first bit for native 2x or
                    // 0/1 in the second bit for 2x as 4x) of 2x destination to
                    // vertical pixels of 1x source.
                    if (true) {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    } else {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    }
                  }
                }
              }
            }
            assert_true(push_constants_member_host_depth_address != UINT32_MAX);
            id_vector_temp.clear();
            id_vector_temp.push_back(
                builder.makeIntConstant(int32_t(push_constants_member_host_depth_address)));
            spv::Id host_depth_address_constant =
                builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                             push_constants, id_vector_temp),
                                   spv::NoPrecision);
            // Transform the destination tile index into the host depth source.
            // After the addition, it may be negative - in which case, the
            // transfer is done across EDRAM addressing wrapping, and
            // xenos::kEdramTileCount must be added to it, but
            // `& (xenos::kEdramTileCount - 1)` handles that regardless of the
            // sign.
            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createUnaryOp(
                    spv::OpBitcast, type_uint,
                    builder.createBinOp(
                        spv::OpIAdd, type_int,
                        builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
                        builder.createTriOp(
                            spv::OpBitFieldSExtract, type_int,
                            builder.createUnaryOp(spv::OpBitcast, type_int,
                                                  host_depth_address_constant),
                            builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                            builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
            // Split the host depth source tile index into X and Y tile index
            // within the source image.
            spv::Id host_depth_source_pitch_tiles =
                builder.createTriOp(spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                                    builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                                    builder.makeUintConstant(xenos::kEdramPitchTilesBits));
            spv::Id host_depth_source_tile_index_y =
                builder.createBinOp(spv::OpUDiv, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            spv::Id host_depth_source_tile_index_x =
                builder.createBinOp(spv::OpUMod, type_uint, host_depth_source_tile_index,
                                    host_depth_source_pitch_tiles);
            // Finally calculate the host depth source texture coordinates.
            spv::Id host_depth_source_pixel_x_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_width_samples >>
                                                                 uint32_t(key.source_msaa_samples >=
                                                                          xenos::MsaaSamples::k4X)),
                                        host_depth_source_tile_index_x),
                    host_depth_source_tile_pixel_x));
            spv::Id host_depth_source_pixel_y_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_height_samples >>
                                                                 uint32_t(key.source_msaa_samples >=
                                                                          xenos::MsaaSamples::k2X)),
                                        host_depth_source_tile_index_y),
                    host_depth_source_tile_pixel_y));
            // Load the host depth source.
            spv::Builder::TextureParameters host_depth_source_texture_parameters = {};
            host_depth_source_texture_parameters.sampler =
                builder.createLoad(host_depth_source_texture, spv::NoPrecision);
            id_vector_temp.clear();
            id_vector_temp.push_back(host_depth_source_pixel_x_int);
            id_vector_temp.push_back(host_depth_source_pixel_y_int);
            host_depth_source_texture_parameters.coords =
                builder.createCompositeConstruct(type_int2, id_vector_temp);
            if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
              host_depth_source_texture_parameters.sample =
                  builder.createUnaryOp(spv::OpBitcast, type_int, host_depth_source_sample_id);
            } else {
              host_depth_source_texture_parameters.lod = builder.makeIntConstant(0);
            }
            host_depth32 = builder.createCompositeExtract(
                builder.createTextureCall(spv::NoPrecision, type_float4, false, true, false, false,
                                          false, host_depth_source_texture_parameters,
                                          spv::ImageOperandsMaskNone),
                type_float, 0);
          } else if (host_depth_source_buffer != spv::NoResult) {
            // Get the address in the EDRAM scratch buffer and load from there.
            // The beginning of the buffer is (0, 0) of the destination.
            // 40-sample columns are not swapped for addressing simplicity
            // (because this is used for depth -> depth transfers, where
            // swapping isn't needed).
            // Convert samples to pixels.
            assert_true(key.host_depth_source_msaa_samples == xenos::MsaaSamples::k1X);
            spv::Id dest_tile_sample_x = dest_tile_pixel_x;
            spv::Id dest_tile_sample_y = dest_tile_pixel_y;
            if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                // Horizontal sample index in bit 0.
                dest_tile_sample_x = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
                    builder.makeUintConstant(1), builder.makeUintConstant(31));
              }
              // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
              // or 1 in bit 1 for 4x or for 2x emulated as 4x.
              dest_tile_sample_y = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint,
                  builder.createBinOp((key.dest_msaa_samples == xenos::MsaaSamples::k2X &&
                                       true)
                                          ? spv::OpBitwiseXor
                                          : spv::OpShiftRightLogical,
                                      type_uint, dest_sample_id, builder.makeUintConstant(1)),
                  dest_tile_pixel_y, builder.makeUintConstant(1), builder.makeUintConstant(31));
            }
            // Combine the tile sample index and the tile index.
            // The tile index doesn't need to be wrapped, as the host depth is
            // written to the beginning of the buffer, without the base offset.
            spv::Id host_depth_offset = builder.createBinOp(
                spv::OpIAdd, type_uint,
                builder.createBinOp(
                    spv::OpIMul, type_uint,
                    builder.makeUintConstant(tile_width_samples * tile_height_samples),
                    dest_tile_index),
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(tile_width_samples),
                                        dest_tile_sample_y),
                    dest_tile_sample_x));
            id_vector_temp.clear();
            // The only SSBO structure member.
            id_vector_temp.push_back(builder.makeIntConstant(0));
            id_vector_temp.push_back(
                builder.createUnaryOp(spv::OpBitcast, type_int, host_depth_offset));
            // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is
            // generated, it's Uniform.
            host_depth32 = builder.createUnaryOp(
                spv::OpBitcast, type_float,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassUniform, host_depth_source_buffer,
                                              id_vector_temp),
                    spv::NoPrecision));
          }
          spv::Block* depth24_to_depth32_header = builder.getBuildPoint();
          spv::Id depth24_to_depth32_convert_id = spv::NoResult;
          spv::Block* depth24_to_depth32_merge = nullptr;
          spv::Id host_depth24 = spv::NoResult;
          if (host_depth32 != spv::NoResult) {
            // Convert the host depth value to the guest format and check if it
            // matches the value in the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct conversion, adding +0.5 and rounding towards zero
                // results in red instead of black in the 4D5307E6 clear shader.
                host_depth24 = builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createUnaryBuiltinCall(
                        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                        builder.createBinOp(spv::OpFMul, type_float, host_depth32,
                                            builder.makeFloatConstant(float(0xFFFFFF)))));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                host_depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                    builder, host_depth32, false, true, ext_inst_glsl_std_450);
              } break;
            }
            assert_true(host_depth24 != spv::NoResult);
            // Update the header block pointer after the conversion (to avoid
            // assuming that the conversion doesn't branch).
            depth24_to_depth32_header = builder.getBuildPoint();
            spv::Id host_depth_outdated =
                builder.createBinOp(spv::OpINotEqual, type_bool, guest_depth24, host_depth24);
            spv::Block& depth24_to_depth32_convert_entry = builder.makeNewBlock();
            {
              spv::Block& depth24_to_depth32_merge_block = builder.makeNewBlock();
              depth24_to_depth32_merge = &depth24_to_depth32_merge_block;
            }
            builder.createSelectionMerge(depth24_to_depth32_merge, spv::SelectionControlMaskNone);
            builder.createConditionalBranch(host_depth_outdated, &depth24_to_depth32_convert_entry,
                                            depth24_to_depth32_merge);
            builder.setBuildPoint(&depth24_to_depth32_convert_entry);
          }
          // Convert the guest 24-bit depth to float32 (in an open conditional
          // if the host depth is also loaded).
          spv::Id guest_depth32 = spv::NoResult;
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              guest_depth32 = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createBinOp(
                          spv::OpIAdd, type_uint, guest_depth24,
                          builder.createBinOp(spv::OpShiftRightLogical, type_uint, guest_depth24,
                                              builder.makeUintConstant(23)))),
                  builder.makeFloatConstant(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              guest_depth32 = SpirvShaderTranslator::Depth20e4To32(builder, guest_depth24, 0, true,
                                                                   false, ext_inst_glsl_std_450);
            } break;
          }
          assert_true(guest_depth32 != spv::NoResult);
          spv::Id fragment_depth32 = guest_depth32;
          if (host_depth32 != spv::NoResult) {
            assert_not_null(depth24_to_depth32_merge);
            spv::Id depth24_to_depth32_result_block_id = builder.getBuildPoint()->getId();
            builder.createBranch(depth24_to_depth32_merge);
            builder.setBuildPoint(depth24_to_depth32_merge);
            id_vector_temp.clear();
            id_vector_temp.push_back(guest_depth32);
            id_vector_temp.push_back(depth24_to_depth32_result_block_id);
            id_vector_temp.push_back(host_depth32);
            id_vector_temp.push_back(depth24_to_depth32_header->getId());
            fragment_depth32 = builder.createOp(spv::OpPhi, type_float, id_vector_temp);
          }
          builder.createStore(fragment_depth32, output_fragment_depth);
          // Unpack the stencil into the stencil reference output if needed and
          // not already written.
          if (!packed_only_depth && output_fragment_stencil_ref != spv::NoResult) {
            builder.createStore(
                builder.createUnaryOp(spv::OpBitcast, type_int,
                                      builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                                          builder.makeUintConstant(UINT8_MAX))),
                output_fragment_stencil_ref);
          }
        }
      } break;
      case TransferOutput::kStencilBit: {
        if (packed) {
          // Kill the sample if the needed stencil bit is not set.
          assert_true(push_constants_member_stencil_mask != UINT32_MAX);
          id_vector_temp.clear();
          id_vector_temp.push_back(
              builder.makeIntConstant(int32_t(push_constants_member_stencil_mask)));
          spv::Id stencil_mask_constant =
              builder.createLoad(builder.createAccessChain(spv::StorageClassPushConstant,
                                                           push_constants, id_vector_temp),
                                 spv::NoPrecision);
          SpirvBuilder::IfBuilder stencil_kill_if(
              builder.createBinOp(
                  spv::OpIEqual, type_bool,
                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed, stencil_mask_constant),
                  builder.makeUintConstant(0)),
              spv::SelectionControlMaskNone, builder);
          builder.createNoResultOp(spv::OpKill);
          // OpKill terminates the block.
          stencil_kill_if.makeEndIf(false);
        }
      } break;
    }
  }

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeOriginUpperLeft);
  if (output_fragment_depth != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeDepthReplacing);
  }
  if (output_fragment_stencil_ref != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeStencilRefReplacingEXT);
  }
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelFragment, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);


  return shader_code;
}
} // namespace
Format RexTransferShader::OwnershipFormat(graphics::xenos::ColorRenderTargetFormat f) {
  using F=graphics::xenos::ColorRenderTargetFormat;
  switch(f) {
    case F::k_16_16:case F::k_16_16_FLOAT:return Format::RG16Uint;
    case F::k_16_16_16_16:case F::k_16_16_16_16_FLOAT:return Format::RGBA16Uint;
    case F::k_32_FLOAT:return Format::R32Uint;
    case F::k_32_32_FLOAT:return Format::RG32Uint;
    default:return RexRenderTargets::NativeColorFormat(f);
  }
}
bool RexTransferShader::Generate(const Request& r,Identity id,ConvertedShader& result,std::string& error) {
  using M=Mode;using T=Generator::TransferMode;
  Generator::TransferShaderKey key;
  if(!id || !r.scale_x || !r.scale_y || r.scale_x>4 || r.scale_y>4 ||
     uint32_t(r.source.msaa_samples)>2 || uint32_t(r.destination.msaa_samples)>2) {
    error="invalid native ownership shader profile";return false;
  }
  bool source_depth=false,dest_depth=false,host_depth=false;
  switch(r.mode) {
    case M::ColorToDepth:key.mode=T::kColorToDepth;dest_depth=true;break;
    case M::ColorToColor:key.mode=T::kColorToColor;break;
    case M::DepthToDepth:key.mode=T::kDepthToDepth;source_depth=dest_depth=true;break;
    case M::DepthToColor:key.mode=T::kDepthToColor;source_depth=true;break;
    case M::ColorAndHostDepthToDepth:key.mode=T::kColorAndHostDepthToDepth;dest_depth=host_depth=true;break;
    case M::DepthAndHostDepthToDepth:key.mode=T::kDepthAndHostDepthToDepth;source_depth=dest_depth=host_depth=true;break;
    default:error="unknown native ownership transfer mode";return false;
  }
  if(bool(r.source.is_depth)!=source_depth || bool(r.destination.is_depth)!=dest_depth ||
     host_depth!=bool(r.host_depth) || (r.host_depth &&
       (!r.host_depth->is_depth || uint32_t(r.host_depth->msaa_samples)>2))) {
    error="native ownership transfer mode and target keys disagree";return false;
  }
  auto valid=[](RexRenderTargets::Key k) {
    return k.is_depth? k.resource_format<=1:
      RexRenderTargets::NativeColorFormat(k.GetColorFormat())!=Format::Invalid;
  };
  if(!valid(r.source) || !valid(r.destination) || (r.host_depth && !valid(*r.host_depth))) {
    error="unsupported native ownership transfer format";return false;
  }
  key.source_msaa_samples=r.source.msaa_samples;key.dest_msaa_samples=r.destination.msaa_samples;
  key.source_resource_format=r.source.resource_format;key.dest_resource_format=r.destination.resource_format;
  key.host_depth_source_msaa_samples=r.host_depth?r.host_depth->msaa_samples:graphics::xenos::MsaaSamples::k1X;
  key.dest_color_rt_index=0;
  auto words=Generator::Generate(key,r.scale_x,r.scale_y);
  return ConvertSpirv(words,id,Stage::Fragment,"main",result,error);
}
} // namespace rex::native
