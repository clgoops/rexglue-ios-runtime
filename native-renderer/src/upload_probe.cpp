// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/upload_probe.h"
#include <cstring>
namespace rex::native {
namespace {
constexpr const char* kProbeVertex=R"(#include <metal_stdlib>
using namespace metal;struct O{float4 p[[position]];float4 c;};
vertex O probe_vertex(uint i[[vertex_id]],const device float4& c[[buffer(0)]]){float2 p=float2((i<<1)&2,i&2);return O{float4(p*2-1,.5,1),c};}
)";
constexpr const char* kProbeFragment=R"(#include <metal_stdlib>
using namespace metal;struct O{float4 p[[position]];float4 c;};fragment float4 probe_fragment(O in[[stage_in]]){return in.c;}
)";
constexpr const char* kProbeIndexVertex=R"(#include <metal_stdlib>
using namespace metal;vertex float4 probe_vertex(uint i[[vertex_id]]){float2 p=float2((i<<1)&2,i&2);return float4(p*2-1,.5,1);}
)";
constexpr const char* kProbeIndexFragment=R"(#include <metal_stdlib>
using namespace metal;fragment float4 probe_fragment(constant float4& c[[buffer(0)]]){return c;}
)";
template<class T> Bytes Data(const T& value) {
  auto bytes=std::make_shared<std::vector<std::byte>>(sizeof(T));std::memcpy(bytes->data(),&value,sizeof(T));return bytes;
}
}
Frame UploadProbe(bool indexed,uint32_t samples) {
  Frame f;f.sequence=1;
  Surface image;image.id={1,1};image.width=image.height=16;image.samples=samples;image.format=Format::RGBA8;image.renderable=true;image.owner=std::make_shared<const int>(1);f.surfaces={image};
  Shader vs{{100,1},Stage::Vertex,"probe_vertex",{},indexed?kProbeIndexVertex:kProbeVertex,{}};
  Shader ps{{101,1},Stage::Fragment,"probe_fragment",{},indexed?kProbeIndexFragment:kProbeFragment,{}};
  (indexed?ps:vs).arguments={{ArgumentKind::Buffer,0,1,16,16}};f.shaders={vs,ps};
  Pipeline pipeline;pipeline.id={200,1};pipeline.vertex=vs.id;pipeline.fragment=ps.id;pipeline.colors[0]=Format::RGBA8;pipeline.samples=samples;f.pipelines={pipeline};
  f.buffer_storage={{{500,1},16,true,std::make_shared<const int>(500)}};
  Pass pass;pass.width=pass.height=16;pass.samples=samples;Attachment a;a.view.surface=image.id;a.load=Load::Clear;a.color={0,0,0,1};pass.colors[0]=a;
  Draw draw;draw.pipeline=pipeline.id;draw.vertices=3;draw.viewport={0,0,16,16,0,1};draw.scissor={0,0,8,16};
  if(indexed) {
    draw.vertices=0;draw.indices=BufferView{{500,1},0,16};draw.index_bytes=4;draw.index_count=3;
    f.buffers={{{300,1},Data(std::array<float,4>{1,0,0,1}),true},{{301,1},Data(std::array<float,4>{0,1,0,1}),true}};
    draw.bindings[1].buffers={{0,{{300,1},0,16}}};
  } else draw.bindings[0].buffers={{0,{{500,1},0,16}}};
  pass.commands.push_back(draw);Pass second=pass;second.colors[0]->load=Load::Load;
  auto& next=std::get<Draw>(second.commands[0]);next.scissor={8,0,8,16};if(indexed)next.bindings[1].buffers={{0,{{301,1},0,16}}};
  Bytes first=indexed?Data(std::array<uint32_t,4>{0,1,2,0}):Data(std::array<float,4>{1,0,0,1});
  Bytes last=indexed?Data(std::array<uint32_t,4>{}):Data(std::array<float,4>{0,1,0,1});
  f.commands={BufferUpload{{{500,1},0,16},first},pass,BufferUpload{{{500,1},0,16},last},second};
  if(samples>1) {
    Surface output=image;output.id={2,1};output.samples=1;output.owner=std::make_shared<const int>(2);f.surfaces.push_back(output);
    f.commands.push_back(Resolve{{image.id},{output.id},ResolveFilter::Average});f.output=SurfaceView{output.id};
  } else f.output=SurfaceView{image.id};
  return f;
}
bool CheckUploadProbe(std::span<const std::byte> pixels,bool indexed) {
  if(pixels.size()!=16*16*4)return false;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const std::array<uint8_t,4> expected=x<8?std::array<uint8_t,4>{255,0,0,255}:
      indexed?std::array<uint8_t,4>{0,0,0,255}:std::array<uint8_t,4>{0,255,0,255};
    for(unsigned c=0;c<4;++c)if(std::to_integer<uint8_t>(pixels[(y*16+x)*4+c])!=expected[c])return false;
  }
  return true;
}
}
