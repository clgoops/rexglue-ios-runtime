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

#include "rex/native/rex_textures.h"
namespace rex::native {
namespace xenos=graphics::xenos;
const RexTextures::HostFormatPair RexTextures::best_formats[64] = {
    // k_1_REVERSE
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8
    {{kLoadShaderIndex8bpb, Format::R8},
     {kLoadShaderIndex8bpb, Format::R8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_1_5_5_5
    // Red and blue swapped in the load shader for simplicity.
    {{kLoadShaderIndexR5G5B5A1ToB5G5R5A1, Format::BGR5A1},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    // Red and blue swapped in the load shader for simplicity.
    {{kLoadShaderIndexR5G6B5ToB5G6R5, Format::BGR565},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    // On the host, green bits in blue, blue bits in green.
    {{kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, Format::BGR565},
     {kLoadShaderIndexUnknown},
     XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {{kLoadShaderIndex32bpb, Format::RGBA8},
     {kLoadShaderIndex32bpb, Format::RGBA8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_2_10_10_10
    // Signed 2_10_10_10 is unsupported on D3D12, keep Vulkan behavior aligned.
    {{kLoadShaderIndex32bpb, Format::RGB10A2},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {{kLoadShaderIndex8bpb, Format::R8},
     {kLoadShaderIndex8bpb, Format::R8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_8_B
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_8
    {{kLoadShaderIndex16bpb, Format::RG8},
     {kLoadShaderIndex16bpb, Format::RG8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_Cr_Y1_Cb_Y0_REP
    // Format::Invalid (added in
    // VK_KHR_sampler_ycbcr_conversion and promoted to Vulkan 1.1) is
    // optional.
    // Signed variant is unsupported on D3D12.
    {{kLoadShaderIndex32bpb, Format::Invalid, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    // Format::Invalid (added in
    // VK_KHR_sampler_ycbcr_conversion and promoted to Vulkan 1.1) is
    // optional.
    // Signed variant is unsupported on D3D12.
    {{kLoadShaderIndex32bpb, Format::Invalid, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_A
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_4_4_4_4
    // Components swapped in the load shader for simplicity.
    {{kLoadShaderIndexRGBA4ToARGB4, Format::ABGR4},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    // Runtime fallback to float16 may be enabled if normalized RGBA16 host
    // formats are not linearly filterable.
    {{kLoadShaderIndexR11G11B10ToRGBA16, Format::RGBA16Unorm},
     {kLoadShaderIndexR11G11B10ToRGBA16SNorm, Format::RGBA16Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    // Runtime fallback to float16 may be enabled if normalized RGBA16 host
    // formats are not linearly filterable.
    {{kLoadShaderIndexR10G11B11ToRGBA16, Format::RGBA16Unorm},
     {kLoadShaderIndexR10G11B11ToRGBA16SNorm, Format::RGBA16Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    // Format::BC1 is optional.
    {{kLoadShaderIndex64bpb, Format::BC1, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    // Format::BC2 is optional.
    {{kLoadShaderIndex128bpb, Format::BC2, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    // Format::BC3 is optional.
    {{kLoadShaderIndex128bpb, Format::BC3, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_24_8
    // Keep signed and unsigned sampling on the same R32 float descriptor path
    // for parity with D3D12.
    {{kLoadShaderIndexDepthUnorm, Format::R32Float},
     {kLoadShaderIndexDepthUnorm, Format::R32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_24_8_FLOAT
    // Keep signed and unsigned sampling on the same R32 float descriptor path
    // for parity with D3D12.
    {{kLoadShaderIndexDepthFloat, Format::R32Float},
     {kLoadShaderIndexDepthFloat, Format::R32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16
    // Format::R16Unorm and Format::R16Snorm are optional.
    {{kLoadShaderIndex16bpb, Format::R16Unorm},
     {kLoadShaderIndex16bpb, Format::R16Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16
    // Format::RG16Unorm and Format::RG16Snorm are optional.
    {{kLoadShaderIndex32bpb, Format::RG16Unorm},
     {kLoadShaderIndex32bpb, Format::RG16Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16
    // Format::RGBA16Unorm and Format::RGBA16Snorm are
    // optional.
    {{kLoadShaderIndex64bpb, Format::RGBA16Unorm},
     {kLoadShaderIndex64bpb, Format::RGBA16Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_16_EXPAND
    {{kLoadShaderIndex16bpb, Format::R16Float},
     {kLoadShaderIndex16bpb, Format::R16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16_EXPAND
    {{kLoadShaderIndex32bpb, Format::RG16Float},
     {kLoadShaderIndex32bpb, Format::RG16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16_EXPAND
    {{kLoadShaderIndex64bpb, Format::RGBA16Float},
     {kLoadShaderIndex64bpb, Format::RGBA16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_16_FLOAT
    {{kLoadShaderIndex16bpb, Format::R16Float},
     {kLoadShaderIndex16bpb, Format::R16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_16_16_FLOAT
    {{kLoadShaderIndex32bpb, Format::RG16Float},
     {kLoadShaderIndex32bpb, Format::RG16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_16_16_16_16_FLOAT
    {{kLoadShaderIndex64bpb, Format::RGBA16Float},
     {kLoadShaderIndex64bpb, Format::RGBA16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_FLOAT
    {{kLoadShaderIndex32bpb, Format::R32Float},
     {kLoadShaderIndex32bpb, Format::R32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR,
     true},
    // k_32_32_FLOAT
    {{kLoadShaderIndex64bpb, Format::RG32Float},
     {kLoadShaderIndex64bpb, Format::RG32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG,
     true},
    // k_32_32_32_32_FLOAT
    {{kLoadShaderIndex128bpb, Format::RGBA32Float},
     {kLoadShaderIndex128bpb, Format::RGBA32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_32_AS_8
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_MPEG
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_MPEG_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG_INTERLACED
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXN
    // Format::BC5 is optional.
    {{kLoadShaderIndex128bpb, Format::BC5, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {{kLoadShaderIndex32bpb, Format::RGBA8},
     {kLoadShaderIndex32bpb, Format::RGBA8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     true},
    // k_DXT1_AS_16_16_16_16
    // Format::BC1 is optional.
    {{kLoadShaderIndex64bpb, Format::BC1, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    // Format::BC2 is optional.
    {{kLoadShaderIndex128bpb, Format::BC2, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    // Format::BC3 is optional.
    {{kLoadShaderIndex128bpb, Format::BC3, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    // Signed 2_10_10_10 is unsupported on D3D12, keep Vulkan behavior aligned.
    {{kLoadShaderIndex32bpb, Format::RGB10A2},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    // Runtime fallback to float16 may be enabled if normalized RGBA16 host
    // formats are not linearly filterable.
    {{kLoadShaderIndexR11G11B10ToRGBA16, Format::RGBA16Unorm},
     {kLoadShaderIndexR11G11B10ToRGBA16SNorm, Format::RGBA16Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    // Runtime fallback to float16 may be enabled if normalized RGBA16 host
    // formats are not linearly filterable.
    {{kLoadShaderIndexR10G11B11ToRGBA16, Format::RGBA16Unorm},
     {kLoadShaderIndexR10G11B11ToRGBA16SNorm, Format::RGBA16Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT3A
    {{kLoadShaderIndexDXT3A, Format::R8},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    // Format::BC4 is optional.
    {{kLoadShaderIndex64bpb, Format::BC4, true},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {{kLoadShaderIndexCTX1, Format::RG8},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {{kLoadShaderIndexDXT3AAs1111ToARGB4, Format::ABGR4},
     {kLoadShaderIndexUnknown},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    {{kLoadShaderIndexUnknown}, {kLoadShaderIndexUnknown}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};
RexTextures::HostFormatPair RexTextures::Formats(xenos::TextureFormat f,bool native_bc) {
  auto p=best_formats[uint32_t(f)];
  if(native_bc&&p.format_unsigned.block_compressed&&p.format_unsigned.format>=Format::BC1&&p.format_unsigned.format<=Format::BC7Srgb)return p;
  using F=xenos::TextureFormat;
  auto decoded=[&](LoadIndex index,Format format) {p.format_unsigned={index,format,false,true};p.format_signed={};p.unsigned_signed_compatible=false;};
  switch(f) {
    case F::k_DXT1:case F::k_DXT1_AS_16_16_16_16:decoded(kLoadShaderIndexDXT1ToRGBA8,Format::RGBA8);break;
    case F::k_DXT2_3:case F::k_DXT2_3_AS_16_16_16_16:decoded(kLoadShaderIndexDXT3ToRGBA8,Format::RGBA8);break;
    case F::k_DXT4_5:case F::k_DXT4_5_AS_16_16_16_16:decoded(kLoadShaderIndexDXT5ToRGBA8,Format::RGBA8);break;
    case F::k_DXN:decoded(kLoadShaderIndexDXNToRG8,Format::RG8);break;
    case F::k_DXT5A:decoded(kLoadShaderIndexDXT5AToR8,Format::R8);break;
    case F::k_Cr_Y1_Cb_Y0_REP:decoded(kLoadShaderIndexGBGR8ToRGB8,Format::RGBA8);break;
    case F::k_Y1_Cr_Y0_Cb_REP:decoded(kLoadShaderIndexBGRG8ToRGB8,Format::RGBA8);break;
    case F::k_5_6_5:decoded(kLoadShaderIndexR5G6B5ToRGBA8,Format::RGBA8);break;
    case F::k_4_4_4_4:decoded(kLoadShaderIndexRGBA4ToRGBA8,Format::RGBA8);break;
    default:break;
  }
  // The SDK's ARGB4 storage maps to Metal ABGR4 with B/R exchanged, exactly as
  // in the pinned MoltenVK pixel-format table. Fold it into shader swizzling.
  if(p.format_unsigned.format==Format::ABGR4) {
    uint32_t adjusted=0;for(unsigned i=0;i<4;++i) {
      uint32_t component=(p.swizzle>>(3*i))&7u;if(component==0)component=2;else if(component==2)component=0;
      adjusted|=component<<(3*i);
    }
    p.swizzle=adjusted;
  }
  return p;
}
RexTextures::HostFormatPair RexTextures::Formats(Key key,bool native_bc) {
  // Keep volume, 1D and irregular base dimensions on the decoded path.
  // BC mip tails below 4x4 are legal within an aligned 2D/cube allocation.
  const bool shape=(key.dimension==xenos::DataDimension::k2DOrStacked||key.dimension==xenos::DataDimension::kCube)&&
    key.GetWidth()>=4&&key.GetHeight()>=4&&!(key.GetWidth()%4)&&!(key.GetHeight()%4);
  return Formats(key.format,native_bc&&shape);
}
uint64_t RexTextures::StorageBytes(const Surface& image) {
  const auto f=Describe(image.format);uint64_t bytes=0;
  for(uint32_t level=0;level<image.levels;++level)bytes+=
    uint64_t((std::max(1u,image.width>>level)+f.block_width-1)/f.block_width)*
    ((std::max(1u,image.height>>level)+f.block_height-1)/f.block_height)*
    std::max(1u,image.depth>>level)*image.layers*image.samples*f.bytes;
  return bytes;
}
} // namespace rex::native
