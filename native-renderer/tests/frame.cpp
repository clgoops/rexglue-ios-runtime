// SPDX-License-Identifier: BSD-3-Clause
#include "fixtures.h"
#include "rex/native/lru.h"
#include <iostream>
#include <limits>
#include <stdexcept>
int main(){
  using namespace test;int checks=0;std::string error;
  auto require=[&](bool value){++checks;if(!value)throw std::runtime_error("Native frame check "+std::to_string(checks)+": "+error);};
  auto good=Triangle();require(Validate(good,error));
  auto reject=[&](auto change){auto f=good;change(f);require(!Validate(f,error)&&!error.empty());};
  reject([](Frame& f){f.sequence=0;});
  reject([](Frame& f){f.surfaces[0].id={};});
  reject([](Frame& f){f.surfaces[0].owner.reset();});
  reject([](Frame& f){f.buffers[0].id=f.surfaces[0].id;});
  reject([](Frame& f){f.buffers[0].bytes.reset();});
  reject([](Frame& f){f.surfaces[0].width=0;});
  reject([](Frame& f){f.surfaces[0].levels=30;});
  reject([](Frame& f){f.surfaces[0].samples=3;});
  reject([](Frame& f){f.surfaces[0].format=Format::BC1;});
  reject([](Frame& f){f.surfaces[0].kind=TextureKind::Cube;});
  reject([](Frame& f){f.pipelines[0].fragment=Id(100);});
  reject([](Frame& f){f.shaders[0].msl.clear();});
  reject([](Frame& f){f.shaders[0].library=Data(std::array<uint32_t,1>{1});});
  reject([](Frame& f){f.shaders[1].arguments[0].index=31;});
  reject([](Frame& f){f.shaders[1].arguments.push_back(f.shaders[1].arguments[0]);});
  reject([](Frame& f){f.shaders[1].arguments[0].alignment=3;});
  reject([](Frame& f){f.shaders[1].constants={{0,1,Shader::ConstantType::Bool},{0,0,Shader::ConstantType::Bool}};});
  reject([](Frame& f){f.shaders[1].constants={{0,2,Shader::ConstantType::Bool}};});
  auto draw=[](Frame& f)->Draw&{return std::get<Draw>(std::get<Pass>(f.commands[0]).commands[0]);};
  reject([&](Frame& f){draw(f).bindings[1].buffers.clear();});
  reject([&](Frame& f){draw(f).bindings[1].buffers[0].view.offset=1;});
  reject([&](Frame& f){draw(f).bindings[1].buffers[0].view.length=15;});
  reject([&](Frame& f){draw(f).indices=BufferView{Id(300),1,15};draw(f).index_count=3;});
  reject([&](Frame& f){draw(f).index_count=3;});
  reject([&](Frame& f){draw(f).scissor={0,0,UINT32_MAX,1};});
  reject([&](Frame& f){draw(f).viewport.width=std::numeric_limits<double>::quiet_NaN();});
  reject([&](Frame& f){draw(f).viewport.near_z=-1;});
  reject([&](Frame& f){draw(f).stencil_front=256;});
  reject([&](Frame& f){draw(f).bindings[1].textures.push_back({0,View(1)});});
  reject([](Frame& f){f.pipelines[0].samples=4;});
  reject([](Frame& f){f.pipelines[0].colors[0]=Format::RGBA16Float;});
  reject([](Frame& f){f.pipelines[0].blend[0].write_mask=16;});
  reject([](Frame& f){std::get<Pass>(f.commands[0]).depth=Color(1);});
  reject([](Frame& f){RectClear c;c.rectangle={0,0,16,16};c.colors=2;std::get<Pass>(f.commands[0]).commands.push_back(c);});
  reject([](Frame& f){f.output=View(999);});
  Frame copies;copies.sequence=1;copies.surfaces={Target(1),Target(2)};
  ImageCopy copy;copy.source=View(1);copy.destination=View(2);copy.width=copy.height=16;copies.commands.push_back(copy);require(Validate(copies,error));
  auto bad=copies;std::get<ImageCopy>(bad.commands[0]).source_x=UINT32_MAX;require(!Validate(bad,error));
  bad=copies;std::get<ImageCopy>(bad.commands[0]).destination=View(1);require(!Validate(bad,error));
  Frame resolved;resolved.sequence=1;resolved.surfaces={Target(1,Format::RGBA8,4),Target(2)};resolved.commands.emplace_back(Resolve{View(1),View(2),ResolveFilter::Average});require(Validate(resolved,error));
  std::get<Resolve>(resolved.commands[0]).filter=ResolveFilter::SampleZero;require(!Validate(resolved,error));
  Frame uploads;uploads.sequence=1;uploads.surfaces={Target(1)};
  TextureUpload u;u.destination=View(1);u.bytes=Data(std::array<std::byte,1024>{});u.bytes_per_row=64;u.bytes_per_image=1024;uploads.commands.push_back(u);require(Validate(uploads,error));
  std::get<TextureUpload>(uploads.commands[0]).bytes_per_row=UINT64_MAX;require(!Validate(uploads,error));
  uploads.commands.clear();auto owner=std::make_shared<const int>(1);uploads.buffer_storage.push_back({Id(10),64,true,owner});
  uploads.commands.push_back(BufferUpload{{Id(10),0,64},Data(std::array<uint32_t,16>{})});require(Validate(uploads,error));
  std::get<BufferUpload>(uploads.commands[0]).destination.offset=1;require(!Validate(uploads,error));
  auto compute=good;compute.commands.clear();Shader cs;cs.id=Id(400);cs.stage=Stage::Compute;cs.entry="compute_main";cs.msl="kernel void compute_main(){}";
  cs.arguments={{ArgumentKind::Buffer,0,1,16,16,TextureKind::TwoD,false,true}};compute.shaders.push_back(cs);
  Dispatch d;d.shader=cs.id;d.bindings.buffers.push_back({0,{Id(300),0,16}});compute.commands.emplace_back(d);require(!Validate(compute,error));
  compute.buffer_storage.push_back({Id(500),64,true,owner});std::get<Dispatch>(compute.commands[0]).bindings.buffers[0].view={Id(500),0,64};require(Validate(compute,error));
  std::get<Dispatch>(compute.commands[0]).threads={1024,1024,1024};require(!Validate(compute,error));
  Frame joined;joined.sequence=1;auto first=ColorPass(1);auto next=ColorPass(1);next.colors[0]->load=Load::Load;
  AppendPass(joined,first);AppendPass(joined,next);require(joined.commands.size()==1);
  next.colors[0]->load=Load::Clear;AppendPass(joined,next);require(joined.commands.size()==2);
  joined.commands.emplace_back(copy);next.colors[0]->load=Load::Load;AppendPass(joined,next);require(joined.commands.size()==4);
  joined.commands.clear();AppendPass(joined,first);next.colors[0]->view.level=1;AppendPass(joined,next);require(joined.commands.size()==2);
  joined.commands.clear();first.colors[0]->store=Store::Discard;AppendPass(joined,first);next.colors[0]->view.level=0;AppendPass(joined,next);require(joined.commands.size()==2);
  require(CompatibleView(Format::RGBA8,Format::RGBA8Srgb));require(!CompatibleView(Format::RGBA8,Format::BGRA8));
  Lru<Identity,int> cache;cache.Insert(Id(1),1);cache.Insert(Id(2),2);cache.Insert(Id(3),3);require(*cache.Find(Id(1))==1);cache.Trim(2);require(!cache.Contains(Id(2))&&cache.Contains(Id(1))&&cache.Contains(Id(3)));
  cache.Insert({1,2},4);cache.Trim(2);require(cache.Contains(Id(1))&&cache.Contains({1,2})&&!cache.Contains(Id(3)));
  std::cout<<checks<<" native frame contracts passed\n";
}
