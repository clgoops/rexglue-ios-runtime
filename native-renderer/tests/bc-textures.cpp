// SPDX-License-Identifier: BSD-3-Clause
// Actual SDK untile/endian kernels and the production upload planner feed both
// native BC textures and decoded references. No title data is used.
#include "fixtures.h"
#include "rex/native/rex_textures.h"
#include "rex/native/metal_backend.h"
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <future>
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <source_location>
#include <stdexcept>
using namespace rex::native;
namespace gfx=rex::graphics;
namespace x=gfx::xenos;
static unsigned checks=0,cases=0;static uint64_t next_id=1;static std::string error;
static Identity Next(){return {next_id++,1};}
static void Check(bool value,std::source_location where=std::source_location::current()){++checks;if(!value)throw std::runtime_error("BC check "+std::to_string(checks)+" at line "+std::to_string(where.line())+": "+error);}
static std::array<uint8_t,4> Color(x::TextureFormat format,unsigned mip,unsigned layer,unsigned bx,unsigned by,bool cube) {
  constexpr std::array<std::array<uint8_t,4>,6> colors={{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255},{255,0,255,255},{0,255,255,255}}};
  auto c=colors[(mip+layer+(cube?0:bx/3+by/3))%colors.size()];
  if(format==x::TextureFormat::k_DXT2_3)c[3]=uint8_t(((mip+layer)%16)*17);
  if(format==x::TextureFormat::k_DXT4_5)c[3]=uint8_t(64+(mip+layer)*13);
  if(format==x::TextureFormat::k_DXN)c={uint8_t(32+(mip+layer)*19),uint8_t(48+(mip+layer)*17),0,255};
  if(format==x::TextureFormat::k_DXT5A)c={uint8_t(64+(mip+layer)*13),0,0,255};
  return c;
}
static std::vector<std::byte> Block(x::TextureFormat format,std::array<uint8_t,4> color) {
  const unsigned size=gfx::FormatInfo::Get(format)->bytes_per_block();std::vector<std::byte> b(size);
  unsigned offset=0;
  if(format==x::TextureFormat::k_DXT2_3){std::fill_n(b.begin(),8,std::byte((color[3]/17)*17));offset=8;}
  if(format==x::TextureFormat::k_DXT4_5){b[0]=std::byte(color[3]);offset=8;}
  if(format==x::TextureFormat::k_DXN){b[0]=std::byte(color[0]);b[8]=std::byte(color[1]);return b;}
  if(format==x::TextureFormat::k_DXT5A){b[0]=std::byte(color[0]);return b;}
  const uint16_t c=uint16_t((color[0]>>3)<<11|(color[1]>>2)<<5|(color[2]>>3));
  std::memcpy(b.data()+offset,&c,2);return b;
}
struct Guest {Bytes base,mips;};
static Guest Input(RexTextures::Key key) {
  const auto layout=key.GetGuestLayout();const uint32_t bpb=gfx::FormatInfo::Get(key.format)->bytes_per_block();
  auto base=std::make_shared<std::vector<std::byte>>((layout.base.level_data_extent_bytes+15)&~15u);
  auto mips=std::make_shared<std::vector<std::byte>>((layout.mips_total_extent_bytes+15)&~15u);
  for(unsigned level=0;level<=key.mip_max_level;++level) {
    auto& bytes=level?*mips:*base;const auto& g=level?layout.mips[std::min(level,layout.packed_level)]:layout.base;
    uint32_t ox=0,oy=0,oz=0;
    if(level>=layout.packed_level)gfx::texture_util::GetPackedMipOffset(key.GetWidth(),key.GetHeight(),1,key.format,level,ox,oy,oz);
    Check(oz==0);const unsigned w=(std::max(1u,key.GetWidth()>>level)+3)/4,h=(std::max(1u,key.GetHeight()>>level)+3)/4;
    for(unsigned layer=0;layer<key.GetDepthOrArraySize();++layer)for(unsigned y=0;y<h;++y)for(unsigned z=0;z<w;++z) {
      const uint64_t offset=(level?layout.mip_offsets_bytes[std::min(level,layout.packed_level)]:0)+uint64_t(layer)*g.array_slice_stride_bytes+
        (key.tiled?uint32_t(gfx::texture_util::GetTiledOffset2D(z+ox,y+oy,g.row_pitch_bytes/bpb,std::countr_zero(bpb))):uint64_t(y+oy)*g.row_pitch_bytes+uint64_t(z+ox)*bpb);
      auto block=Block(key.format,Color(key.format,level,layer,z,y,key.dimension==x::DataDimension::kCube));
      Check(offset+block.size()<=bytes.size());std::memcpy(bytes.data()+offset,block.data(),block.size());
    }
  }
  const unsigned swap=key.endianness==x::Endian::k8in16?1:key.endianness==x::Endian::k8in32?3:key.endianness==x::Endian::k16in32?2:0;
  if(swap)for(auto bytes:{base,mips})for(size_t i=0;i<bytes->size();i+=4) {
    std::array<std::byte,4> v;std::memcpy(v.data(),bytes->data()+i,4);for(unsigned j=0;j<4;++j)(*bytes)[i+j]=v[j^swap];
  }
  return {base,mips};
}
struct Packet {Frame frame;Surface output,image;uint64_t staging=0;};
static Packet Build(RexTextures::Key key,const Guest& guest,bool native) {
  Packet p;auto& f=p.frame;f.sequence=1;
  const auto host=RexTextures::Formats(key,native).format_unsigned;const auto& info=RexTextures::LoadMetadata(host.load_shader);
  RexTextures::LoadPlan plan;Check(RexTextures::PlanLoad(key,key.GetGuestLayout(),info,host,true,true,plan,error));p.staging=plan.size;
  auto& image=p.image;image.id=Next();image.width=key.GetWidth();image.height=key.GetHeight();image.levels=key.mip_max_level+1;image.format=host.format;
  image.kind=key.dimension==x::DataDimension::kCube?TextureKind::Cube:TextureKind::TwoDArray;image.layers=key.GetDepthOrArraySize();image.owner=std::make_shared<const Identity>(image.id);
  f.surfaces.push_back(image);Buffer input_base{Next(),guest.base,false},input_mips{Next(),guest.mips,false};f.buffers={input_base,input_mips};
  BufferStorage scratch{Next(),plan.size,true,{}};scratch.owner=std::make_shared<const Identity>(scratch.id);f.buffer_storage.push_back(scratch);
  ConvertedShader kernel;Check(ConvertSpirv(RexTextures::LoadKernel(host.load_shader),Next(),Stage::Compute,"main",kernel,error));f.shaders.push_back(kernel.shader);
  for(const auto& step:plan.loads) {
    auto data=std::make_shared<std::vector<std::byte>>(256);std::memcpy(data->data(),&step.constants,sizeof(step.constants));Buffer constants{Next(),data,true};f.buffers.push_back(constants);
    const auto& input=step.base?input_base:input_mips;std::vector<DescriptorResources> resources={
      {ResourceSource::Descriptor,0,0,{{scratch.id,0,scratch.size}},{},{}},
      {ResourceSource::Descriptor,1,0,{{input.id,0,input.bytes->size()}},{},{}},
      {ResourceSource::PushConstants,0,0,{{constants.id,0,256}},{},{}}};
    Dispatch d;d.shader=kernel.shader.id;d.threads=kernel.workgroup_size;d.groups=step.groups;std::optional<Buffer> aux;
    Check(BindConverted(kernel,resources,Next(),d.bindings,aux,error));if(aux)f.buffers.push_back(*aux);f.commands.push_back(d);
  }
  for(auto copy:plan.copies){copy.source.buffer=scratch.id;copy.destination.surface=image.id;f.commands.push_back(copy);}
  auto& output=p.output;output=test::Target(1);output.id=Next();output.width=16*(key.mip_max_level+1)*image.layers;output.owner=std::make_shared<const Identity>(output.id);f.surfaces.push_back(output);
  auto vs=test::Vertex();vs.id=Next();Shader ps;ps.id=Next();ps.stage=Stage::Fragment;ps.entry="sample_main";
  ps.arguments={{ArgumentKind::Buffer,0,1,16,16},{ArgumentKind::Texture,0,1,0,1,image.kind},{ArgumentKind::Sampler,0}};
  const std::string header="#include <metal_stdlib>\nusing namespace metal;struct P{uint mip,layer,origin,pad;};";
  if(image.kind==TextureKind::Cube)ps.msl=header+
    "fragment float4 sample_main(float4 p[[position]],constant P& c[[buffer(0)]],texturecube<float> image[[texture(0)]],sampler s[[sampler(0)]]){float2 u=(p.xy-float2(c.origin,0))/8.0-1.0;float3 d;switch(c.layer){case 0:d=float3(1,-u.y,-u.x);break;case 1:d=float3(-1,-u.y,u.x);break;case 2:d=float3(u.x,1,u.y);break;case 3:d=float3(u.x,-1,-u.y);break;case 4:d=float3(u.x,-u.y,1);break;default:d=float3(-u.x,-u.y,-1);}return image.sample(s,d,level(float(c.mip)));}";
  else ps.msl=header+
    "fragment float4 sample_main(float4 p[[position]],constant P& c[[buffer(0)]],texture2d_array<float> image[[texture(0)]],sampler s[[sampler(0)]]){float2 u=(p.xy-float2(c.origin,0))/16.0;return image.sample(s,u,c.layer,level(float(c.mip)));}";
  f.shaders.push_back(vs);f.shaders.push_back(ps);Pipeline pipeline;pipeline.id=Next();pipeline.vertex=vs.id;pipeline.fragment=ps.id;pipeline.colors[0]=Format::RGBA8;f.pipelines.push_back(pipeline);
  Sampler sampler;sampler.id=Next();f.samplers.push_back(sampler);Pass pass;pass.width=output.width;pass.height=16;pass.colors[0]=Attachment{{output.id},Load::Clear};
  unsigned cell=0;for(unsigned layer=0;layer<image.layers;++layer)for(unsigned mip=0;mip<image.levels;++mip,++cell) {
    Buffer constants{Next(),test::Data(std::array<uint32_t,4>{mip,layer,cell*16,0}),true};f.buffers.push_back(constants);
    Draw d;d.pipeline=pipeline.id;d.vertices=3;d.viewport={double(cell*16),0,16,16,0,1};d.scissor={cell*16,0,16,16};
    d.bindings[1].buffers={{0,{constants.id,0,16}}};d.bindings[1].textures={{0,{image.id}}};d.bindings[1].samplers={{0,sampler.id}};pass.commands.push_back(d);
  }
  f.commands.push_back(pass);f.output=SurfaceView{output.id};return p;
}
static std::vector<std::byte> Run(Packet& packet) {
  MetalBackend backend;
  {
    std::promise<Completion> done;auto result=done.get_future();
    Check(backend.Submit(std::make_shared<const Frame>(packet.frame),[&](Completion c){done.set_value(std::move(c));},error));
    Check(backend.Drain(std::chrono::seconds(30),error));Check(result.wait_for(std::chrono::seconds(5))==std::future_status::ready);
    auto c=result.get();error=c.error;Check(c.success);
  }
  auto pixels=backend.ReadRGBA8({packet.output.id},error);Check(pixels.size()==size_t(packet.output.width)*16*4);return pixels;
}
int main(){
  MetalBackend device;if(!device.Ready(error)||!device.GetDeviceCapabilities().bc_texture_compression){std::cout<<"BC_GPU_UNAVAILABLE: native BC sampling not validated\n";return 77;}
  using F=x::TextureFormat;using D=x::DataDimension;
  for(F format:{F::k_DXT1,F::k_DXT2_3,F::k_DXT4_5,F::k_DXN,F::k_DXT5A})
    for(auto config:std::array<std::array<uint32_t,6>,4>{{{64,32,1,6,0,2},{64,32,3,6,1,1},{64,64,6,6,1,0},{4,4,1,2,1,3}}}) {
      RexTextures::Key key;key.base_page=1;key.mip_page=2;key.format=format;key.dimension=config[2]==6?D::kCube:D::k2DOrStacked;
      key.width_minus_1=config[0]-1;key.height_minus_1=config[1]-1;key.depth_or_array_size_minus_1=config[2]-1;key.pitch=(config[0]+31)/32;
      key.packed_mips=1;key.mip_max_level=config[3];key.tiled=config[4];key.endianness=x::Endian(config[5]);
      std::cout<<"BC begin format="<<uint32_t(format)<<" width="<<key.GetWidth()<<" layers="<<key.GetDepthOrArraySize()<<" tiled="<<key.tiled<<std::endl;
      auto guest=Input(key);auto native=Build(key,guest,true),decoded=Build(key,guest,false);
      auto pixels=Run(native),reference=Run(decoded);Check(pixels.size()==reference.size());
      for(size_t i=0;i<pixels.size();++i)Check(std::abs(int(std::to_integer<uint8_t>(pixels[i]))-int(std::to_integer<uint8_t>(reference[i])))<=2);
      unsigned cell=0;for(unsigned layer=0;layer<key.GetDepthOrArraySize();++layer)for(unsigned mip=0;mip<=key.mip_max_level;++mip,++cell)
        for(unsigned y=0;y<16;++y)for(unsigned z=0;z<16;++z) {
          auto expected=Color(format,mip,layer,uint32_t((z+.5)/16*std::max(1u,key.GetWidth()>>mip))/4,uint32_t((y+.5)/16*std::max(1u,key.GetHeight()>>mip))/4,key.dimension==D::kCube);
          const size_t offset=(size_t(y)*native.output.width+cell*16+z)*4;
          for(unsigned c=0;c<4;++c)Check(std::abs(int(std::to_integer<uint8_t>(pixels[offset+c]))-expected[c])<=2);
        }
      // Fixed 4x4 BC blocks can cost more than an R8/RG8 decoded mip tail.
      // Resource savings are expected for the larger fixtures, not every tail.
      if(key.GetWidth()>4) {
        Check(native.staging<decoded.staging);
        Check(RexTextures::StorageBytes(native.image)<RexTextures::StorageBytes(decoded.image));
      }
      std::cout<<"BC case "<<++cases<<": format="<<uint32_t(format)<<" tiled="<<key.tiled<<" layers="<<key.GetDepthOrArraySize()<<" mips="<<key.mip_max_level+1<<" native_bytes="<<RexTextures::StorageBytes(native.image)<<" decoded_bytes="<<RexTextures::StorageBytes(decoded.image)<<std::endl;
    }
  std::cout<<cases<<" SDK upload/native BC pixel cases and "<<checks<<" checks passed\n";
}
