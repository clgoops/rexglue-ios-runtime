// SPDX-License-Identifier: BSD-3-Clause
#import <Foundation/Foundation.h>
#include "fixtures.h"
#include "rex/native/metal_backend.h"
#include "rex/native/surface_pool.h"
#include <atomic>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <future>
using namespace test;
static int checks=0,cases=0;
static std::string failure;
static void Check(bool value){++checks;if(!value)throw std::runtime_error("Native Metal check "+std::to_string(checks)+": "+failure);}
static void Submit(MetalBackend& backend,Frame frame){
  Check(backend.Submit(std::make_shared<const Frame>(std::move(frame)),{},failure));
  Check(backend.Drain(std::chrono::seconds(30),failure));
}
static void Pixel(MetalBackend& backend,SurfaceView view,uint32_t x,uint32_t y,std::array<int,4> expected,int tolerance=2){
  auto pixels=backend.ReadRGBA8(view,failure);Check(pixels.size()==16*16*4);
  for(size_t i=0;i<4;++i)Check(std::abs(int(std::to_integer<uint8_t>(pixels[(y*16+x)*4+i]))-expected[i])<=tolerance);
}
static void Done(const char* name){++cases;std::cout<<"GPU case "<<cases<<": "<<name<<std::endl;}
static Shader Textured(){
  Shader s;s.id=Id(101);s.stage=Stage::Fragment;s.entry="fragment_main";
  s.arguments={{ArgumentKind::Texture,0,1,0,1,TextureKind::TwoD},{ArgumentKind::Sampler,0}};
  s.msl="#include <metal_stdlib>\nusing namespace metal;fragment float4 fragment_main(texture2d<float> image[[texture(0)]],sampler s[[sampler(0)]]){return image.sample(s,float2(.5,.5));}";return s;
}
static Frame TexturedFrame(Surface image,Bytes bytes,uint64_t row,uint64_t size){
  Frame f=Triangle();f.surfaces.push_back(std::move(image));f.shaders[1]=Textured();f.buffers.clear();
  auto& d=std::get<Draw>(std::get<Pass>(f.commands[0]).commands[0]);d.bindings[1].buffers.clear();d.bindings[1].textures={{0,View(2)}};d.bindings[1].samplers={{0,Id(600)}};
  Sampler sampler;sampler.id=Id(600);f.samplers.push_back(sampler);
  TextureUpload upload;upload.destination=View(2);upload.bytes=std::move(bytes);upload.bytes_per_row=row;upload.bytes_per_image=size;
  f.commands.insert(f.commands.begin(),upload);return f;
}
int main(){@autoreleasepool {
  MetalBackend available;if(!available.Ready(failure)){std::cout<<"GPU_UNAVAILABLE: "<<failure<<std::endl;return 77;}
  {
    MetalBackend backend;auto f=Triangle();auto& pass=std::get<Pass>(f.commands[0]);pass.commands.push_back(pass.commands[0]);Submit(backend,std::move(f));Pixel(backend,View(1),8,8,{255,0,0,255});
    Check(backend.Stats().avoided_bindings>=3);Done("native draw, reflected constants and redundant binding reuse");
  }
  {
    uint64_t unit;
    {MetalBackend probe;Submit(probe,Triangle());unit=probe.Stats().surface_bytes;Check(unit>0);}
    BackendOptions options;options.surface_budget=128*unit;MetalBackend backend(options);
    std::vector<Surface> retained;Frame first;first.sequence=1;
    for(uint64_t id=1;id<=128;++id) {
      auto image=Target(id);first.surfaces.push_back(image);
      if(id<=64)retained.push_back(std::move(image));
    }
    first.commands.push_back(ColorPass(1,{1,0,0,1}));Submit(backend,std::move(first));
    Check(backend.Stats().surface_bytes==128*unit);
    // Incremental sweeping visits the first 64 live owners. Pressure must find
    // the expired second half rather than rejecting this new target.
    Frame second;second.sequence=2;second.surfaces={retained[0],Target(129)};
    second.commands.push_back(ColorPass(129,{0,1,0,1}));Submit(backend,std::move(second));
    Check(backend.Stats().surface_bytes==65*unit);
    Pixel(backend,View(1),8,8,{255,0,0,255});Pixel(backend,View(129),8,8,{0,255,0,255});
    Done("surface pressure reclaims expired entries beyond 64 while preserving live pixels");
  }
  {
    uint64_t unit;
    {MetalBackend probe;Submit(probe,Triangle());unit=probe.Stats().surface_bytes;}
    BackendOptions options;options.surface_budget=2*unit;MetalBackend backend(options);
    auto retained=Target(1),other=Target(2);Frame first;first.sequence=1;first.surfaces={retained,other};
    first.commands.push_back(ColorPass(1,{1,0,0,1}));Submit(backend,std::move(first));
    Frame rejected;rejected.sequence=2;rejected.surfaces={Target(3)};
    rejected.commands.push_back(ColorPass(3,{0,1,0,1}));
    Check(!backend.Submit(std::make_shared<const Frame>(std::move(rejected)),{},failure));
    Check(failure.find("Native surface budget exhausted: live=")==0);
    Check(backend.Drain(std::chrono::seconds(1),failure));Pixel(backend,View(1),8,8,{255,0,0,255});
    Done("live images remain protected and the surface budget remains bounded");
  }
  {
    MetalBackend backend;Frame f;f.sequence=1;f.surfaces={Target(1)};auto pass=ColorPass(1,{0,0,1,1});
    RectClear clear;clear.rectangle={4,4,8,8};clear.colors=1;clear.color[0]={1,0,0,1};pass.commands.emplace_back(clear);f.commands.emplace_back(pass);
    Submit(backend,std::move(f));Pixel(backend,View(1),5,5,{255,0,0,255});Pixel(backend,View(1),1,1,{0,0,255,255});Done("partial clear preserves the surrounding target");
  }
  for(uint32_t samples:{1u,4u}) {
    MetalBackend backend;auto f=Triangle(1,Target(1,Format::RGBA8,samples));f.buffers.clear();
    f.buffer_storage={{Id(500),16,true,std::make_shared<const int>(500)}};
    f.shaders[0].arguments={{ArgumentKind::Buffer,0,1,16,16}};
    f.shaders[0].msl="#include <metal_stdlib>\nusing namespace metal;struct O{float4 p[[position]];float4 c;};vertex O vertex_main(uint i[[vertex_id]],const device float4& c[[buffer(0)]]){float2 p=float2((i<<1)&2,i&2);return O{float4(p*2-1,.5,1),c};}";
    f.shaders[1].arguments.clear();f.shaders[1].msl="#include <metal_stdlib>\nusing namespace metal;struct O{float4 p[[position]];float4 c;};fragment float4 fragment_main(O in[[stage_in]]){return in.c;}";
    auto& first=std::get<Pass>(f.commands[0]);auto& draw=std::get<Draw>(first.commands[0]);
    draw.bindings[1].buffers.clear();draw.bindings[0].buffers={{0,{Id(500),0,16}}};draw.scissor={0,0,8,16};
    Pass second=first;second.colors[0]->load=Load::Load;std::get<Draw>(second.commands[0]).scissor={8,0,8,16};
    f.commands.insert(f.commands.begin(),BufferUpload{{Id(500),0,16},Data(std::array<float,4>{1,0,0,1})});
    f.commands.push_back(BufferUpload{{Id(500),0,16},Data(std::array<float,4>{0,1,0,1})});f.commands.push_back(second);
    if(samples>1){f.surfaces.push_back(Target(2));f.commands.push_back(Resolve{View(1),View(2),ResolveFilter::Average});f.output=View(2);}
    Submit(backend,std::move(f));const auto view=samples>1?View(2):View(1);
    Pixel(backend,view,4,8,{255,0,0,255});Pixel(backend,view,12,8,{0,255,0,255});
    Check(backend.Stats().passes==1&&backend.Stats().inline_uploads==1);
    Done(samples==1?"in-pass vertex upload preserves old/new draw values":"4x MSAA in-pass upload preserves old/new draw values");
  }
  {
    MetalBackend backend;auto f=Triangle();f.buffer_storage={{Id(500),16,true,std::make_shared<const int>(500)}};
    auto& first=std::get<Pass>(f.commands[0]);auto& draw=std::get<Draw>(first.commands[0]);draw.indices=BufferView{Id(500),0,16};draw.index_bytes=4;draw.index_count=3;draw.vertices=0;draw.scissor={0,0,8,16};
    Pass second=first;second.colors[0]->load=Load::Load;auto& d=std::get<Draw>(second.commands[0]);d.scissor={8,0,8,16};
    f.buffers.push_back({Id(301),Data(std::array<float,4>{0,1,0,1}),true});d.bindings[1].buffers={{0,{Id(301),0,16}}};
    f.commands.insert(f.commands.begin(),BufferUpload{{Id(500),0,16},Data(std::array<uint32_t,4>{0,1,2,0})});
    f.commands.push_back(BufferUpload{{Id(500),0,16},Data(std::array<uint32_t,4>{})});f.commands.push_back(second);
    Submit(backend,std::move(f));Pixel(backend,View(1),4,8,{255,0,0,255});Pixel(backend,View(1),12,8,{0,0,0,255});
    Check(backend.Stats().passes==1&&backend.Stats().inline_uploads==1);Done("in-pass upload synchronizes subsequent hardware index fetch");
  }
  {
    MetalBackend backend;auto f=Triangle();f.buffers.clear();f.buffer_storage={{Id(500),16,true,std::make_shared<const int>(500)}};
    auto& first=std::get<Pass>(f.commands[0]);auto& draw=std::get<Draw>(first.commands[0]);draw.scissor={0,0,8,16};draw.bindings[1].buffers={{0,{Id(500),0,16}}};
    Pass second=first;second.colors[0]->load=Load::Load;std::get<Draw>(second.commands[0]).scissor={8,0,8,16};
    f.commands.insert(f.commands.begin(),BufferUpload{{Id(500),0,16},Data(std::array<float,4>{1,0,0,1})});
    f.commands.push_back(BufferUpload{{Id(500),0,16},Data(std::array<float,4>{0,1,0,1})});f.commands.push_back(second);
    Submit(backend,std::move(f));Pixel(backend,View(1),4,8,{255,0,0,255});Pixel(backend,View(1),12,8,{0,255,0,255});
    Check(backend.Stats().passes==2&&backend.Stats().inline_uploads==0);Done("fragment read/write hazard retains its encoder boundary");
  }
  {
    MetalBackend backend;SurfacePool pool(1024);pool.Remember(123,Target(1));auto image=pool.Find(123);
    Submit(backend,Triangle(1,*image));auto reused=pool.Find(123);Check(reused->id==image->id&&reused->owner==image->owner);
    Frame f;f.sequence=2;f.surfaces={*reused};Pass pass;pass.width=pass.height=16;pass.colors[0]=Attachment{View(1)};
    RectClear clear;clear.rectangle={0,0,4,4};clear.colors=1;clear.color[0]={0,1,0,1};pass.commands.push_back(clear);f.commands.push_back(pass);
    Submit(backend,std::move(f));Pixel(backend,View(1),1,1,{0,255,0,255});Pixel(backend,View(1),12,12,{255,0,0,255});
    Check(backend.Stats().surface_allocations==1&&backend.Stats().reduced_passes==1);
    Done("exact-key surface reuse and cropped render area preserve surrounding pixels");
  }
  for(uint32_t samples:{2u,4u}) {
    MetalBackend backend;Surface image=Target(1,Format::RGBA8,samples),output=Target(2);
    auto first=Triangle(1,image);first.surfaces.push_back(output);first.output=View(2);first.commands.push_back(Resolve{View(1),View(2),ResolveFilter::Average});Submit(backend,std::move(first));
    Frame second;second.sequence=2;second.surfaces={image,output};Pass pass;pass.width=pass.height=16;pass.samples=samples;pass.colors[0]=Attachment{View(1)};
    RectClear clear;clear.rectangle={0,0,4,4};clear.colors=1;clear.color[0]={0,1,0,1};pass.commands.push_back(clear);second.commands.push_back(pass);
    second.commands.push_back(Resolve{View(1),View(2),ResolveFilter::Average});Submit(backend,std::move(second));
    Pixel(backend,View(2),1,1,{0,255,0,255});Pixel(backend,View(2),12,12,{255,0,0,255});
    Check(backend.Stats().reduced_passes==1);Done(samples==2?"2x MSAA cropped pass preserves surrounding samples":"4x MSAA cropped pass preserves surrounding samples");
  }
  {
    // A controlled workload isolates encoder/store-load overhead. It is not
    // a game benchmark, and its timing ratio is not a device FPS prediction.
    constexpr unsigned count=128,rounds=3;double elapsed[2]{};
    for(unsigned mode=0;mode<2;++mode) {
      BackendOptions options;options.inline_buffer_uploads=bool(mode);options.tight_render_area=false;options.timing=true;MetalBackend backend(options);
      Surface image=Target(1,Format::RGBA8,4);image.width=1280;image.height=720;auto f=Triangle(1,image);f.output.reset();
      f.buffer_storage={{Id(500),16,true,std::make_shared<const int>(500)}};
      auto first=std::get<Pass>(f.commands[0]);first.width=1280;first.height=720;f.commands={first};
      auto next=first;next.colors[0]->load=Load::Load;
      for(unsigned i=1;i<count;++i) {f.commands.push_back(BufferUpload{{Id(500),0,16},Data(std::array<uint32_t,4>{i,0,0,0})});f.commands.push_back(next);}
      for(unsigned round=0;round<rounds;++round) {
        f.sequence=round+1;std::promise<Completion> promise;auto result=promise.get_future();
        Check(backend.Submit(std::make_shared<const Frame>(f),[&](Completion c){promise.set_value(std::move(c));},failure));
        Check(backend.Drain(std::chrono::seconds(30),failure));Check(result.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        auto c=result.get();Check(c.success&&c.timing_available);if(round)elapsed[mode]+=c.native_window_ms;
      }
      Check(backend.Stats().passes==rounds*(mode?1:count));
    }
    std::cout<<"SYNTHETIC_UPLOAD_BATCH_GPU_MS baseline="<<elapsed[0]/2<<" batched="<<elapsed[1]/2<<std::endl;
    Done("128-pass upload workload preserves ordering while using one render encoder");
  }
  {
    MetalBackend backend;auto f=Triangle();f.pipelines[0].blend[0].write_mask=1;std::get<Pass>(f.commands[0]).colors[0]->color={0,0,1,1};
    Submit(backend,std::move(f));Pixel(backend,View(1),8,8,{255,0,255,255});Done("color mask preserves non-written channels");
  }
  {
    MetalBackend backend;auto f=Triangle();f.buffers[0].bytes=Data(std::array<float,4>{1,0,0,.5f});auto& b=f.pipelines[0].blend[0];b.enabled=true;b.source_rgb=BlendFactor::SourceAlpha;b.destination_rgb=BlendFactor::OneMinusSourceAlpha;
    std::get<Pass>(f.commands[0]).colors[0]->color={0,0,1,1};Submit(backend,std::move(f));Pixel(backend,View(1),8,8,{128,0,128,128});Done("source-alpha blending");
  }
  {
    MetalBackend backend;auto f=Triangle();f.surfaces.push_back(Target(2,Format::Depth32Float));auto& pipeline=f.pipelines[0];pipeline.depth=Format::Depth32Float;pipeline.depth_compare=Compare::Less;pipeline.depth_write=true;
    auto& vertex=f.shaders[0];vertex.arguments={{ArgumentKind::Buffer,0,1,16,16}};
    vertex.msl="#include <metal_stdlib>\nusing namespace metal;vertex float4 vertex_main(uint i[[vertex_id]],constant float4& z[[buffer(0)]]){float2 p=float2((i<<1)&2,i&2);return float4(p*2-1,z.z,1);}";
    f.buffers.push_back({Id(301),Data(std::array<float,4>{0,0,.2f,0}),true});f.buffers.push_back({Id(302),Data(std::array<float,4>{0,0,.8f,0}),true});f.buffers.push_back({Id(303),Data(std::array<float,4>{0,1,0,1}),true});
    auto& pass=std::get<Pass>(f.commands[0]);pass.depth=Color(2);pass.depth->depth=1;auto& first=std::get<Draw>(pass.commands[0]);first.bindings[0].buffers={{0,{Id(301),0,16}}};
    Draw second=first;second.bindings[0].buffers={{0,{Id(302),0,16}}};second.bindings[1].buffers={{0,{Id(303),0,16}}};pass.commands.push_back(second);
    Submit(backend,std::move(f));Pixel(backend,View(1),8,8,{255,0,0,255});Done("depth comparison and ordered constant snapshots");
  }
  {
    MetalBackend backend;auto f=Triangle();f.surfaces.push_back(Target(2,Format::Depth32Stencil8));auto& pipeline=f.pipelines[0];pipeline.depth=pipeline.stencil=Format::Depth32Stencil8;pipeline.stencil_enabled=true;pipeline.front.compare=pipeline.back.compare=Compare::Equal;pipeline.front.write_mask=pipeline.back.write_mask=0;
    auto& pass=std::get<Pass>(f.commands[0]);pass.colors[0]->color={0,0,1,1};pass.depth=Color(2);pass.stencil=Color(2);pass.stencil->stencil=0;
    RectClear clear;clear.rectangle={4,4,8,8};clear.stencil=true;clear.stencil_value=3;auto draw=std::get<Draw>(pass.commands[0]);draw.stencil_front=draw.stencil_back=3;pass.commands={clear,draw};
    Submit(backend,std::move(f));Pixel(backend,View(1),5,5,{255,0,0,255});Pixel(backend,View(1),1,1,{0,0,255,255});Done("partial stencil clear and stencil comparison");
  }
  {
    MetalBackend backend;Surface image=Target(2);image.renderable=false;std::vector<std::byte> pixels(1024);
    for(size_t i=0;i<pixels.size();i+=4){pixels[i+1]=pixels[i+3]=std::byte{255};}
    Submit(backend,TexturedFrame(image,Data(std::span<const std::byte>(pixels)),64,1024));Pixel(backend,View(1),8,8,{0,255,0,255});Done("ordered texture upload and reflected texture/sampler slots");
  }
  {
    MetalBackend backend;Frame f;f.sequence=1;f.surfaces={Target(1),Target(2)};f.commands.emplace_back(ColorPass(1,{0,0,1,1}));f.commands.emplace_back(ColorPass(2,{1,0,0,1}));ImageCopy copy;copy.source=View(1);copy.destination=View(2);copy.width=copy.height=8;f.commands.emplace_back(copy);
    Submit(backend,std::move(f));Pixel(backend,View(2),2,2,{0,0,255,255});Pixel(backend,View(2),14,14,{255,0,0,255});Done("exact partial image copy and destination preservation");
  }
  {
    MetalBackend backend;auto f=Triangle(1,Target(1,Format::RGBA8,4));f.surfaces.push_back(Target(2));f.commands.emplace_back(Resolve{View(1),View(2),ResolveFilter::Average});f.output=View(2);
    Submit(backend,std::move(f));Pixel(backend,View(2),8,8,{255,0,0,255});Done("native 4x MSAA color resolve");
  }
  {
    MetalBackend backend;Frame f;f.sequence=1;Surface target=Target(1);target.storage=true;target.renderable=false;f.surfaces={target};Shader shader;shader.id=Id(100);shader.stage=Stage::Compute;shader.entry="compute_main";shader.arguments={{ArgumentKind::Texture,0,1,0,1,TextureKind::TwoD,false,true}};
    shader.msl="#include <metal_stdlib>\nusing namespace metal;kernel void compute_main(texture2d<float,access::write> image[[texture(0)]],uint2 p[[thread_position_in_grid]]){if(p.x<16&&p.y<16)image.write(float4(0,1,0,1),p);}";f.shaders={shader};Dispatch d;d.shader=shader.id;d.groups={2,2,1};d.threads={8,8,1};d.bindings.textures={{0,View(1)}};f.commands.emplace_back(d);
    Submit(backend,std::move(f));Pixel(backend,View(1),8,8,{0,255,0,255});Done("native compute texture writes");
  }
  {
    MetalBackend backend;auto f=Triangle();f.buffers.clear();f.buffer_storage.push_back({Id(500),16,true,std::make_shared<const int>(1)});BufferUpload upload{{Id(500),0,16},Data(std::array<float,4>{})};
    Shader shader;shader.id=Id(102);shader.stage=Stage::Compute;shader.entry="compute_main";shader.arguments={{ArgumentKind::Buffer,0,1,16,16,TextureKind::TwoD,false,true}};shader.msl="#include <metal_stdlib>\nusing namespace metal;kernel void compute_main(device float4* data[[buffer(0)]]){data[0]=float4(0,1,0,1);}";f.shaders.push_back(shader);
    Dispatch d;d.shader=shader.id;d.bindings.buffers={{0,{Id(500),0,16}}};auto& draw=std::get<Draw>(std::get<Pass>(f.commands[0]).commands[0]);draw.bindings[1].buffers={{0,{Id(500),0,16}}};f.commands.insert(f.commands.begin(),d);f.commands.insert(f.commands.begin(),upload);
    Submit(backend,std::move(f));Pixel(backend,View(1),8,8,{0,255,0,255});Done("mutable guest-buffer upload, compute write and fragment read ordering");
  }
  {
    MetalBackend backend;Frame f;f.sequence=1;auto image=Target(1);image.renderable=false;f.surfaces={image};
    f.buffer_storage={{Id(500),2048,true,std::make_shared<const int>(1)},{Id(501),2048,true,std::make_shared<const int>(2)}};
    Shader shader;shader.id=Id(100);shader.stage=Stage::Compute;shader.entry="compute_main";
    shader.arguments={{ArgumentKind::Buffer,0,1,2048,4,TextureKind::TwoD,false,true}};
    shader.msl="#include <metal_stdlib>\nusing namespace metal;kernel void compute_main(device uint* data[[buffer(0)]],uint2 p[[thread_position_in_grid]]){if(p.x<16&&p.y<16)data[4+p.y*20+p.x]=0xff00ff00u;}";
    f.shaders={shader};Dispatch dispatch;dispatch.shader=shader.id;dispatch.groups={2,2,1};dispatch.threads={8,8,1};
    dispatch.bindings.buffers={{0,{Id(500),0,2048}}};f.commands.push_back(dispatch);
    f.commands.push_back(BufferCopy{{Id(500),0,2048},{Id(501),0,2048},2048});
    f.commands.push_back(BufferToTexture{{Id(501),16,1280},View(1),80,0,0,0,16,16});
    Submit(backend,std::move(f));Pixel(backend,View(1),8,8,{0,255,0,255});
    auto read=backend.ReadBuffer({Id(501),16,4},failure);Check(read.size()==4);uint32_t word;std::memcpy(&word,read.data(),4);Check(word==0xff00ff00u);
    Check(backend.GetDeviceCapabilities().max_buffer_length>=256ull*1024*1024);
    Done("GPU conversion buffer becomes a texture with explicit row pitch and offset");
  }
  {
    MetalBackend backend;Frame f;f.sequence=1;auto image=Target(1);image.renderable=false;image.kind=TextureKind::ThreeD;image.depth=2;f.surfaces={image};
    std::array<uint32_t,512> data;std::fill_n(data.begin(),256,0xff0000ffu);std::fill_n(data.begin()+256,256,0xffff0000u);
    f.buffers={{Id(500),Data(data),false}};auto view=View(1);
    f.commands.push_back(BufferToTexture{{Id(500),0,2048},view,64,1024,0,0,16,16,2});
    Submit(backend,std::move(f));Pixel(backend,view,8,8,{255,0,0,255});view.plane=1;Pixel(backend,view,8,8,{0,0,255,255});Done("GPU buffer copy preserves 3D image strides");
  }
  {
    BackendOptions options;options.maximum_frames=1;options.constant_budget=256;MetalBackend backend(options);auto f=Triangle();Surface target=f.surfaces[0];Submit(backend,f);Pixel(backend,View(1),8,8,{255,0,0,255});
    f.sequence=2;f.buffers[0].id=Id(301);f.buffers[0].bytes=Data(std::array<float,4>{0,1,0,1});std::get<Draw>(std::get<Pass>(f.commands[0]).commands[0]).bindings[1].buffers[0].view.buffer=Id(301);
    Submit(backend,std::move(f));Pixel(backend,View(1),8,8,{0,255,0,255});Check(backend.Stats().constant_pool_reuses>=1);Check(backend.Stats().pipeline_compiles==1);Done("retired constant storage reuse preserves geometry and warm pipelines");
  }
  {
    MetalBackend backend;auto f=Triangle();f.shaders[1].arguments.clear();Check(!backend.Submit(std::make_shared<const Frame>(std::move(f)),{},failure));Check(backend.Drain(std::chrono::seconds(1),failure));Done("unknown compiled bindings reject without submitting a partial frame");
  }
  {
    MetalBackend backend;auto f=Triangle();f.shaders[1].arguments[0].minimum_bytes=1;std::get<Draw>(std::get<Pass>(f.commands[0]).commands[0]).bindings[1].buffers[0].view.length=1;
    Check(!backend.Submit(std::make_shared<const Frame>(std::move(f)),{},failure));Check(backend.Drain(std::chrono::seconds(1),failure));Done("actual Metal reflection catches an undersized declared buffer");
  }
  {
    MetalBackend backend;auto f=Triangle();Submit(backend,f);f.sequence=2;f.buffers[0].bytes=Data(std::array<float,4>{0,1,0,1});
    Check(!backend.Submit(std::make_shared<const Frame>(std::move(f)),{},failure));Check(backend.Drain(std::chrono::seconds(1),failure));Pixel(backend,View(1),8,8,{255,0,0,255});Done("changed bytes cannot borrow an old buffer identity");
  }
  {
    NSURL* file=[NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"rex-native-%@/pipelines.metalarc",NSUUID.UUID.UUIDString]]];
    {MetalBackend backend;Check(backend.ConfigureBinaryArchive(file.path.UTF8String,failure));Submit(backend,Triangle());Check(backend.SaveBinaryArchive(failure));}
    {MetalBackend backend;Check(backend.ConfigureBinaryArchive(file.path.UTF8String,failure));Submit(backend,Triangle());Pixel(backend,View(1),8,8,{255,0,0,255});}
    [NSFileManager.defaultManager removeItemAtURL:[file URLByDeletingLastPathComponent] error:nil];Done("native pipeline archive save and reload");
  }
  {
    MetalBackend backend;auto f=Triangle();f.output.reset();f.surfaces.clear();f.buffers.clear();
    auto& pipeline=f.pipelines[0];pipeline.colors={};auto& pass=std::get<Pass>(f.commands[0]);pass.colors={};pass.mergeable=false;
    f.buffer_storage={{Id(500),1024,true,std::make_shared<const int>(1)}};
    auto& fragment=f.shaders[1];fragment.arguments={{ArgumentKind::Buffer,0,1,1024,4,TextureKind::TwoD,false,true}};
    fragment.msl="#include <metal_stdlib>\nusing namespace metal;fragment void pixel_main(float4 p[[position]],device uint* output[[buffer(0)]]){output[uint(p.y)*16+uint(p.x)]=1u;}";
    fragment.entry="pixel_main";std::get<Draw>(pass.commands[0]).bindings[1].buffers={{0,{Id(500),0,1024}}};
    Submit(backend,std::move(f));auto bytes=backend.ReadBuffer({Id(500),0,1024},failure);Check(bytes.size()==1024);
    for(unsigned i=0;i<256;++i) {uint32_t value;std::memcpy(&value,bytes.data()+i*4,4);Check(value==1);}
    Done("attachmentless fragment memory export with explicit raster dimensions");
  }
  std::cout<<cases<<" native GPU cases and "<<checks<<" checks passed"<<std::endl;
}}
