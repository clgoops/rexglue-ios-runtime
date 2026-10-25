// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "rex/native/frame.h"
#include <cstring>
namespace test {
using namespace rex::native;
inline Bytes Data(std::span<const std::byte> bytes){return std::make_shared<const std::vector<std::byte>>(bytes.begin(),bytes.end());}
template<class T> Bytes Data(const T& value){return Data(std::as_bytes(std::span(&value,1)));}
inline Identity Id(uint64_t value){return {value,1};}
inline Surface Target(uint64_t id,Format format=Format::RGBA8,uint32_t samples=1){
  Surface s;s.id=Id(id);s.width=16;s.height=16;s.format=format;s.samples=samples;s.renderable=true;s.owner=std::make_shared<const Identity>(s.id);return s;
}
inline SurfaceView View(uint64_t id){SurfaceView v;v.surface=Id(id);return v;}
inline Attachment Color(uint64_t id,std::array<double,4> value={0,0,0,1}){Attachment a;a.view=View(id);a.load=Load::Clear;a.color=value;return a;}
inline Pass ColorPass(uint64_t id,std::array<double,4> value={0,0,0,1},uint32_t samples=1){Pass p;p.width=p.height=16;p.samples=samples;p.colors[0]=Color(id,value);return p;}
inline Shader Vertex(uint64_t id=100){
  Shader s;s.id=Id(id);s.stage=Stage::Vertex;s.entry="vertex_main";
  s.msl="#include <metal_stdlib>\nusing namespace metal; vertex float4 vertex_main(uint i[[vertex_id]]){float2 p=float2((i<<1)&2,i&2);return float4(p*2-1,.5,1);}";
  return s;
}
inline Shader Fragment(uint64_t id=101){
  Shader s;s.id=Id(id);s.stage=Stage::Fragment;s.entry="fragment_main";
  s.arguments.push_back({ArgumentKind::Buffer,0,1,16,16});
  s.msl="#include <metal_stdlib>\nusing namespace metal;fragment float4 fragment_main(constant float4& c[[buffer(0)]]){return c;}";return s;
}
inline Frame Triangle(uint64_t sequence=1,Surface target=Target(1)){
  Frame f;f.sequence=sequence;f.surfaces.push_back(std::move(target));f.shaders={Vertex(),Fragment()};
  Pipeline p;p.id=Id(200);p.vertex=Id(100);p.fragment=Id(101);p.colors[0]=f.surfaces[0].format;p.samples=f.surfaces[0].samples;f.pipelines.push_back(p);
  std::array<float,4> color{1,0,0,1};f.buffers.push_back({Id(300),Data(color),true});
  Draw d;d.pipeline=p.id;d.vertices=3;d.viewport={0,0,16,16,0,1};d.scissor={0,0,16,16};d.bindings[1].buffers.push_back({0,{Id(300),0,16}});
  Pass pass=ColorPass(f.surfaces[0].id.object,{0,0,0,1},f.surfaces[0].samples);pass.commands.emplace_back(d);f.commands.emplace_back(std::move(pass));f.output=View(f.surfaces[0].id.object);return f;
}
}
