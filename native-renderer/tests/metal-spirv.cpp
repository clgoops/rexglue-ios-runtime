// SPDX-License-Identifier: BSD-3-Clause
#include "fixtures.h"
#include "rex/native/metal_backend.h"
#include "rex/native/spirv.h"
#include "fullscreen_cw_vs.h"
#include "apply_gamma_table_ps.h"
#include "resolve_clear_32bpp_cs.h"
#include <iostream>
#include <stdexcept>
using namespace test;
static unsigned checks=0;
static std::string error;
static void Check(bool value) {++checks;if(!value)throw std::runtime_error("Converted Metal check "+std::to_string(checks)+": "+error);}
static ConvertedShader Convert(std::span<const uint32_t> words,Identity id,Stage stage) {
  ConvertedShader out;Check(ConvertSpirv(words,id,stage,"main",out,error));return out;
}
static Bindings Bind(const ConvertedShader& s,std::vector<DescriptorResources> resources) {
  Bindings out;std::optional<Buffer> auxiliary;Check(BindConverted(s,resources,{},out,auxiliary,error));Check(!auxiliary);return out;
}
static void Pixel(MetalBackend& backend,std::array<int,4> expected) {
  auto bytes=backend.ReadRGBA8(View(1),error);Check(bytes.size()==1024);
  for(unsigned i=0;i<4;++i)Check(std::abs(int(std::to_integer<uint8_t>(bytes[(8*16+8)*4+i]))-expected[i])<=2);
}
int main() {try {
  MetalBackend backend;if(!backend.Ready(error)) {std::cout<<"GPU_UNAVAILABLE: "<<error<<'\n';return 77;}
  auto vertex=Convert(fullscreen_cw_vs,Id(100),Stage::Vertex);
  auto gamma=Convert(apply_gamma_table_ps,Id(101),Stage::Fragment);
  {
    auto f=Triangle();f.shaders={vertex.shader,gamma.shader};f.buffers.clear();
    Surface lut=Target(2);lut.width=256;lut.height=1;lut.renderable=false;
    Surface image=Target(3);image.renderable=false;f.surfaces.push_back(lut);f.surfaces.push_back(image);
    std::vector<std::byte> lut_bytes(1024),pixels(1024);
    for(unsigned i=0;i<256;++i) {lut_bytes[i*4]=lut_bytes[i*4+1]=lut_bytes[i*4+2]=std::byte(i);lut_bytes[i*4+3]=std::byte{255};}
    for(unsigned i=0;i<256;++i) {pixels[i*4]=std::byte{64};pixels[i*4+1]=std::byte{128};pixels[i*4+2]=std::byte{192};pixels[i*4+3]=std::byte{255};}
    f.commands.insert(f.commands.begin(),TextureUpload{View(2),Data(std::span<const std::byte>(lut_bytes)),1024,1024});
    f.commands.insert(f.commands.begin(),TextureUpload{View(3),Data(std::span<const std::byte>(pixels)),64,1024});
    auto& draw=std::get<Draw>(std::get<Pass>(f.commands.back()).commands[0]);
    draw.bindings[1]=Bind(gamma,{{ResourceSource::Descriptor,0,0,{}, {View(2)},{}},
                              {ResourceSource::Descriptor,1,0,{}, {View(3)},{}}});
    Check(backend.Submit(std::make_shared<const Frame>(std::move(f)),{},error));Check(backend.Drain(std::chrono::seconds(30),error));
    Pixel(backend,{64,128,192,255});std::cout<<"Converted GPU case 1: real SDK fullscreen and gamma shader, linear texel lowering\n";
  }
  {
    auto clear=Convert(resolve_clear_32bpp_cs,Id(102),Stage::Compute);
    auto f=Triangle(2);f.shaders[0]=vertex.shader;f.buffers.clear();
    f.buffer_storage.push_back({Id(500),16384,true,std::make_shared<const int>(1)});
    // First uint4 in the actual SDK EDRAM clear kernel becomes float4(1).
    f.buffers.push_back({Id(301),Data(std::array<uint32_t,4>{0x3f800000,0,1,32}),true});
    auto& fragment=f.shaders[1];fragment.id=Id(103);fragment.arguments[0].minimum_bytes=16;
    f.pipelines[0].id=Id(201);f.pipelines[0].fragment=fragment.id;
    fragment.msl="#include <metal_stdlib>\nusing namespace metal;fragment float4 fragment_main(device const float4* v[[buffer(0)]]){return v[0];}";
    auto& draw=std::get<Draw>(std::get<Pass>(f.commands[0]).commands[0]);draw.pipeline=Id(201);draw.bindings[1].buffers={{0,{Id(500),0,16384}}};
    Dispatch dispatch;dispatch.shader=clear.shader.id;dispatch.threads=clear.workgroup_size;
    dispatch.bindings=Bind(clear,{{ResourceSource::Descriptor,0,0,{{Id(500),0,16384}},{},{}},
                                {ResourceSource::PushConstants,0,0,{{Id(301),0,16}},{},{}}});
    f.shaders.push_back(clear.shader);f.commands.insert(f.commands.begin(),dispatch);
    Check(backend.Submit(std::make_shared<const Frame>(std::move(f)),{},error));Check(backend.Drain(std::chrono::seconds(30),error));
    Pixel(backend,{255,255,255,255});std::cout<<"Converted GPU case 2: real SDK compute clear, reflected push constants and ordered storage read\n";
  }
  std::cout<<checks<<" actual SDK shader Metal pixel checks passed\n";return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
