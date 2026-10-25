// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/rex_constants.h"
#include "rex/native/rex_shader.h"
#include "rex/native/rex_transfer_shader.h"
#include "rex/native/rex_target_transfers.h"
#if REX_NATIVE_TEST_METAL
#include "rex/native/metal_backend.h"
#include "fixtures.h"
#endif
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/util/draw.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
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
  std::optional<ConvertedShader> gpu_pixel;RexConstantState::Map gpu_map{};
  if(argc>1)std::filesystem::create_directories(argv[1]);
  // Every legal color storage pair and sample-count pair must lower without
  // falling back to Vulkan. Selected cases are also compiled by Apple's MSL
  // compiler in CI; the local test does not imply a title frame was rendered.
  {
    using F=xenos::ColorRenderTargetFormat;using M=RexTransferShader::Mode;
    const F formats[]={F::k_8_8_8_8,F::k_8_8_8_8_GAMMA,F::k_2_10_10_10,
      F::k_2_10_10_10_AS_10_10_10_10,F::k_2_10_10_10_FLOAT,
      F::k_2_10_10_10_FLOAT_AS_16_16_16_16,F::k_16_16,F::k_16_16_16_16,
      F::k_16_16_FLOAT,F::k_16_16_16_16_FLOAT,F::k_32_FLOAT,F::k_32_32_FLOAT};
    unsigned number=0;
    auto generate=[&](RexTransferShader::Request r,bool export_case) {
      ConvertedShader converted;check(RexTransferShader::Generate(r,next(),converted,error));
      check(converted.shader.stage==Stage::Fragment && !converted.shader.msl.empty());
      if(argc>1 && export_case)std::ofstream(std::filesystem::path(argv[1])/
        ("transfer-"+std::to_string(number)+".metal"))<<converted.shader.msl;
      ++number;
    };
    for(F source:formats)for(F dest:formats)for(unsigned ss=0;ss<3;++ss)for(unsigned ds=0;ds<3;++ds) {
      RexTransferShader::Request r;r.source.resource_format=uint32_t(source);r.destination.resource_format=uint32_t(dest);
      r.source.msaa_samples=xenos::MsaaSamples(ss);r.destination.msaa_samples=xenos::MsaaSamples(ds);
      generate(r,(ss==0&&ds==0)||(source==F::k_16_16&&dest==F::k_16_16_FLOAT));
    }
    for(bool to_depth:{false,true})for(unsigned sf=0;sf<(to_depth?12u:2u);++sf)
      for(unsigned df=0;df<(to_depth?2u:12u);++df)for(unsigned ss=0;ss<3;++ss)for(unsigned ds=0;ds<3;++ds) {
        RexTransferShader::Request r;r.mode=to_depth?M::ColorToDepth:M::DepthToColor;
        r.source.is_depth=!to_depth;r.destination.is_depth=to_depth;
        r.source.resource_format=to_depth?uint32_t(formats[sf]):sf;
        r.destination.resource_format=to_depth?df:uint32_t(formats[df]);
        r.source.msaa_samples=xenos::MsaaSamples(ss);r.destination.msaa_samples=xenos::MsaaSamples(ds);
        generate(r,ss==ds && (to_depth?sf<2:df<2));
      }
    for(unsigned sf=0;sf<2;++sf)for(unsigned df=0;df<2;++df)
      for(unsigned ss=0;ss<3;++ss)for(unsigned ds=0;ds<3;++ds) {
        RexTransferShader::Request r;r.mode=M::DepthToDepth;r.source.is_depth=r.destination.is_depth=1;
        r.source.resource_format=sf;r.destination.resource_format=df;
        r.source.msaa_samples=xenos::MsaaSamples(ss);r.destination.msaa_samples=xenos::MsaaSamples(ds);
        generate(r,true);
        r.mode=M::DepthAndHostDepthToDepth;r.host_depth=r.destination;generate(r,ss==ds);
        r.mode=M::ColorAndHostDepthToDepth;r.source.is_depth=0;r.source.resource_format=uint32_t(F::k_8_8_8_8);
        generate(r,ss==ds);
      }
    RexTransferShader::Request invalid;invalid.destination.is_depth=1;ConvertedShader converted;
    check(!RexTransferShader::Generate(invalid,next(),converted,error));
    std::cout<<number<<" native EDRAM transfer shader variants converted\n";
    RexTargetTransfers transfers(next);
    Frame packet;packet.sequence=1;
    RexRenderTargets::OwnershipTransfer transfer{};
    transfer.source.pitch_tiles_at_32bpp=transfer.destination.pitch_tiles_at_32bpp=1;
    transfer.source.resource_format=uint32_t(F::k_16_16_FLOAT);
    transfer.destination.resource_format=uint32_t(F::k_8_8_8_8);
    auto target=[&](Format f) {Surface s;s.id=next();s.format=f;s.width=80;s.height=16;
      s.renderable=true;s.owner=std::make_shared<const Identity>(s.id);return s;};
    transfer.source_image=target(Format::RG16Float);transfer.destination_image=target(Format::RGBA8);
    transfer.first_tile=0;transfer.end_tile=1;transfer.rectangles={{0,0,80,16}};
    check(transfers.Encode(packet,transfer,error));check(Validate(packet,error));
    const auto shader_count=packet.shaders.size(),pipeline_count=packet.pipelines.size();
    transfer.source.base_tiles=1;transfer.destination.base_tiles=2;
    check(transfers.Encode(packet,transfer,error));check(Validate(packet,error));
    check(packet.shaders.size()==shader_count && packet.pipelines.size()==pipeline_count);
    transfer.destination.is_depth=1;transfer.destination.resource_format=0;
    transfer.destination_image=target(Format::Depth32Stencil8);
    transfer.host_depth_source=transfer.destination;transfer.host_depth_image=transfer.destination_image;
    check(transfers.Encode(packet,transfer,error));check(Validate(packet,error));
    // The snapshot pass must precede the depth transfer; the destination
    // cannot be sampled while it is attached, including through stencil views.
    check(packet.surfaces.size()==4);
    transfer.rectangles={{79,0,2,16}};check(!transfers.Encode(packet,transfer,error));
  }
  for(uint32_t version:{spv::Spv_1_0,spv::Spv_1_3,spv::Spv_1_5}) {
    for(int variant=0;variant<3;++variant)for(int stage=0;stage<3;++stage) {
      SpirvShaderTranslator::Features features(false);features.spirv_version=version;
      features.signed_zero_inf_nan_preserve_float32=true;
      features.denorm_flush_to_zero_float32=true;features.rounding_mode_rte_float32=true;
      SpirvShaderTranslator translator(features,true,true,false);
      std::vector<uint8_t> bytes;RexConstantState::Map map{};std::shared_ptr<const ConvertedShader> prepared;
      if(stage==2)bytes=translator.CreateDepthOnlyFragmentShader();
      else {
        const uint32_t dest=stage?0:62;
        const uint32_t ucode[]={1u|(3u<<12),2u<<12,0,15u<<16,0,1u<<8,
          1u|(15u<<16),0,(1u<<24)|(1u<<8)|(variant==2?0x40u<<16:0u)|(1u<<31),
          dest|(1u<<15)|(15u<<16),0,(2u<<24)|(1u<<16)|(1u<<8)|(3u<<30)};
        RexShader source(stage?xenos::ShaderType::kPixel:xenos::ShaderType::kVertex,0,
          variant?ucode:nullptr,variant?std::size(ucode):0,std::endian::native);
        string::StringBuffer disasm;source.AnalyzeUcode(disasm);
        auto modification=stage?translator.GetDefaultPixelShaderModification(variant==2?8:0):
                                translator.GetDefaultVertexShaderModification(variant==2?8:0);
        auto* translated=dynamic_cast<RexShader::NativeTranslation*>(source.GetOrCreateTranslation(modification));check(translated!=nullptr);
        check(translated->Prepare(translator,next,error));prepared=translated->converted();check(bool(prepared));
        auto counter=object;check(translated->Prepare(translator,next,error));check(object==counter && prepared==translated->converted());
        bytes=translated->translated_binary();map=source.constant_register_map();
      }
      std::vector<uint32_t> words(bytes.size()/4);memcpy(words.data(),bytes.data(),bytes.size());
      ConvertedShader converted;
      check(ConvertSpirv(words,next(),stage==0?Stage::Vertex:Stage::Fragment,"main",converted,error));
      if(prepared)check(prepared->shader.msl==converted.shader.msl && prepared->resources==converted.resources);
      if(version==spv::Spv_1_5 && variant==1 && stage==1) {gpu_pixel=converted;gpu_map=map;}
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
#if REX_NATIVE_TEST_METAL
  if(argc>2 && std::string_view(argv[2])=="--metal") {
    MetalBackend backend;if(!backend.Ready(error)) {std::cout<<"GPU_UNAVAILABLE: "<<error<<'\n';return 77;}
    check(bool(gpu_pixel));RegisterFile gpu_regs;
    float quarter=.25f;for(unsigned i=0;i<8;++i)memcpy(&gpu_regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X+i],&quarter,4);
    RexConstantState gpu_state;RexConstantState::System gpu_system{};
    gpu_system.flags=SpirvShaderTranslator::kSysFlag_AlphaPassIfLess|SpirvShaderTranslator::kSysFlag_AlphaPassIfEqual|SpirvShaderTranslator::kSysFlag_AlphaPassIfGreater;
    for(auto& v:gpu_system.color_exp_bias)v=1;
    auto build=[&](Frame f) {
      f.shaders[1]=gpu_pixel->shader;f.pipelines[0].fragment=gpu_pixel->shader.id;f.buffers.clear();
      std::vector<DescriptorResources> descriptors;const ConvertedShader* list[]={&*gpu_pixel};
      check(gpu_state.Prepare(gpu_system,gpu_regs,gpu_map,gpu_map,list,next,f,descriptors,error));
      auto& draw=std::get<Draw>(std::get<Pass>(f.commands[0]).commands[0]);std::optional<Buffer> aux;
      check(BindConverted(*gpu_pixel,descriptors,next(),draw.bindings[1],aux,error));check(!aux);return f;
    };
    auto f=test::Triangle();f.shaders[0].id=next();f.pipelines[0].id=next();f.pipelines[0].vertex=f.shaders[0].id;
    std::get<Draw>(std::get<Pass>(f.commands[0]).commands[0]).pipeline=f.pipelines[0].id;
    auto first=std::make_shared<const Frame>(build(f));
    check(backend.Submit(first,{},error));
    float half=.5f;for(unsigned i=4;i<8;++i)memcpy(&gpu_regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X+i],&half,4);
    f.sequence=2;f.surfaces[0]=test::Target(2);f.output=test::View(2);std::get<Pass>(f.commands[0]).colors[0]->view=test::View(2);
    auto second=std::make_shared<const Frame>(build(f));check(backend.Submit(second,{},error));
    check(backend.Drain(std::chrono::seconds(30),error));
    for(unsigned frame=1;frame<=2;++frame) {
      auto pixels=backend.ReadRGBA8(test::View(frame),error);check(pixels.size()==1024);
      for(unsigned channel=0;channel<4;++channel)check(std::abs(int(std::to_integer<uint8_t>(pixels[(8*16+8)*4+channel]))-(frame==1?32:96))<=2);
    }
    std::cout<<"Actual ReX microcode -> SPIR-V -> MSL -> native draw pixels passed for two in-flight constant snapshots\n";
    // Bit reinterpretation tests use actual SDK ownership shaders on hardware.
    // The expected bytes come from IEEE half(1)=0x3c00 and half(2)=0x4000.
    RexTargetTransfers transfers(next);
    for(unsigned samples:{1u,2u,4u}) {
      Frame packet;packet.sequence=10+samples;
      auto target=[&](Format f,unsigned sample_count) {Surface s;s.id=next();s.format=f;s.width=80;s.height=16;
        s.samples=sample_count;s.renderable=true;s.owner=std::make_shared<const Identity>(s.id);return s;};
      auto source=target(Format::RG16Float,samples),destination=target(Format::RGBA8,samples);
      packet.surfaces={source,destination};
      Pass clear;clear.width=80;clear.height=16;clear.samples=samples;
      Attachment a;a.view={source.id};a.load=Load::Clear;a.color={1,2,0,1};clear.colors[0]=a;packet.commands.push_back(clear);
      RexRenderTargets::OwnershipTransfer t{};t.source.pitch_tiles_at_32bpp=t.destination.pitch_tiles_at_32bpp=1;
      t.source.resource_format=uint32_t(xenos::ColorRenderTargetFormat::k_16_16_FLOAT);
      t.destination.resource_format=uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
      t.source.msaa_samples=t.destination.msaa_samples=xenos::MsaaSamples(std::countr_zero(samples));
      t.source_image=source;t.destination_image=destination;t.first_tile=0;t.end_tile=1;t.rectangles={{0,0,80,16}};
      check(transfers.Encode(packet,t,error));
      SurfaceView output{destination.id};
      if(samples>1) {auto resolved=target(Format::RGBA8,1);packet.surfaces.push_back(resolved);
        packet.commands.push_back(Resolve{output,{resolved.id},ResolveFilter::Average});output={resolved.id};}
      auto retained=std::make_shared<const Frame>(std::move(packet));check(backend.Submit(retained,{},error));
      check(backend.Drain(std::chrono::seconds(30),error));auto pixels=backend.ReadRGBA8(output,error);
      check(pixels.size()==80*16*4);const int expected[]={0,60,0,64};
      for(unsigned c=0;c<4;++c)check(std::to_integer<uint8_t>(pixels[(8*80+40)*4+c])==expected[c]);
    }
    // Host depth is the destination: preserve its full float32 value through
    // a separate native snapshot before the ownership pass attaches it.
    {
      Frame packet;packet.sequence=20;
      auto target=[&](Format f) {Surface s;s.id=next();s.format=f;s.width=80;s.height=16;
        s.renderable=true;s.owner=std::make_shared<const Identity>(s.id);return s;};
      auto color=target(Format::RGBA8),depth=target(Format::Depth32Stencil8),output=target(Format::RGBA8);
      packet.surfaces={color,depth,output};
      Pass pc;pc.width=80;pc.height=16;Attachment a;a.view={color.id};a.load=Load::Clear;
      a.color={179.0/255,0,0,128.0/255};pc.colors[0]=a;packet.commands.push_back(pc);
      Pass pd;pd.width=80;pd.height=16;a={};a.view={depth.id};a.load=Load::Clear;a.depth=.5;a.stencil=179;
      pd.depth=pd.stencil=a;packet.commands.push_back(pd);
      RexRenderTargets::OwnershipTransfer t{};t.source.pitch_tiles_at_32bpp=t.destination.pitch_tiles_at_32bpp=1;
      t.source.resource_format=uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);t.destination.is_depth=1;
      t.source_image=color;t.destination_image=depth;t.host_depth_source=t.destination;t.host_depth_image=depth;
      t.rectangles={{0,0,80,16}};t.first_tile=0;t.end_tile=1;
      check(transfers.Encode(packet,t,error));
      // Convert it back to color to verify depth and stencil, including the
      // different depth/color 40-sample column arrangement.
      t.source=t.destination;t.destination=t.host_depth_source.value();t.destination.is_depth=0;
      t.destination.resource_format=uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
      t.source_image=depth;t.destination_image=output;t.host_depth_source.reset();t.host_depth_image.reset();
      check(transfers.Encode(packet,t,error));check(backend.Submit(std::make_shared<const Frame>(std::move(packet)),{},error));
      check(backend.Drain(std::chrono::seconds(30),error));auto pixels=backend.ReadRGBA8({output.id},error);
      check(pixels.size()==80*16*4);const int expected[]={179,0,0,128};
      for(unsigned c=0;c<4;++c)check(std::abs(int(std::to_integer<uint8_t>(pixels[(8*80+40)*4+c]))-expected[c])<=1);
    }
    std::cout<<"Native target transfer pixels passed: half bits, integer views, 1x/2x/4x MSAA, depth/stencil and host-depth snapshot\n";
  }
#endif
}
