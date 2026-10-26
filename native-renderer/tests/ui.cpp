// SPDX-License-Identifier: BSD-3-Clause
// Records through the actual SDK ImmediateDrawer API used by the touch overlay.
#include "fixtures.h"
#include "rex/native/rex_ui.h"
#if REX_NATIVE_TEST_METAL
#include "rex/native/metal_backend.h"
#endif
#include <iostream>
#include <stdexcept>

int main() {
  using namespace rex::native;using namespace test;
  uint64_t identity=1000;auto drawer=CreateNativeImmediateDrawer([&]{return Identity{identity++,1};});
  std::string error;unsigned checks=0;
  auto check=[&](bool result) {++checks;if(!result)throw std::runtime_error("Actual UI check "+std::to_string(checks)+": "+error);};
  const std::array<uint32_t,6> atlas={0xff00ff00,0xff00ff00,0xff00ff00,0xff00ff00,0xff00ff00,0xff00ff00};
  auto texture=drawer->CreateTexture(3,2,rex::ui::ImmediateTextureFilter::kNearest,false,reinterpret_cast<const uint8_t*>(atlas.data()));
  check(texture!=nullptr);
  // A fullscreen indexed quad exercises the actual packed vertex layout,
  // texture upload, sampler, blend state and both scissor/projection methods.
  const std::array<rex::ui::ImmediateVertex,4> vertices={{{0,0,0,0,0xffffffff},{16,0,1,0,0xffffffff},{16,16,1,1,0xffffffff},{0,16,0,1,0xffffffff}}};
  const std::array<uint16_t,6> indices={0,1,2,0,2,3};
  Surface output=Target(1);
  auto record=[&](uint64_t sequence,rex::ui::ImmediateTexture* image) {
    Frame frame;frame.sequence=sequence;frame.surfaces.push_back(output);frame.output=View(1);
    auto pass=ColorPass(1);NativeUIContext context(frame,pass);
    drawer->Begin(context,16,16);drawer->BeginDrawBatch({vertices.data(),4,indices.data(),6});
    rex::ui::ImmediateDraw draw;draw.count=6;draw.texture=image;draw.scissor=true;draw.scissor_right=draw.scissor_bottom=16;
    drawer->Draw(draw);drawer->EndDrawBatch();drawer->End();AppendPass(frame,std::move(pass));return frame;
  };
  auto first=record(1,texture.get());check(Validate(first,error));
  auto upload=std::find_if(first.commands.begin(),first.commands.end(),[](const Command& c){return std::holds_alternative<TextureUpload>(c);});
  check(upload!=first.commands.end());const auto& data=std::get<TextureUpload>(*upload);
  check(data.bytes_per_row==12&&data.bytes_per_image==24&&data.bytes->size()==24);
  auto broken=first;std::get<TextureUpload>(broken.commands[std::distance(first.commands.begin(),upload)]).bytes_per_image=0;
  check(!Validate(broken,error)&&error=="Texture upload pitch or payload is invalid");error.clear();
  auto second=record(2,texture.get());check(Validate(second,error));
  check(std::none_of(second.commands.begin(),second.commands.end(),[](const Command& c){return std::holds_alternative<TextureUpload>(c);}));
  auto white=record(3,nullptr);check(Validate(white,error));
  auto white_upload=std::find_if(white.commands.begin(),white.commands.end(),[](const Command& c){return std::holds_alternative<TextureUpload>(c);});
  check(white_upload!=white.commands.end());check(std::get<TextureUpload>(*white_upload).bytes_per_image==4);
#if REX_NATIVE_TEST_METAL
  MetalBackend backend;if(!backend.Ready(error)) {std::cout<<"GPU_UNAVAILABLE: "<<error<<'\n';return 77;}
  check(!backend.Submit(std::make_shared<const Frame>(broken),{},error));check(backend.Ready(error));error.clear();
  for(auto* frame:{&first,&second,&white}) {
    check(backend.Submit(std::make_shared<const Frame>(*frame),{},error));check(backend.Drain(std::chrono::seconds(30),error));
    auto pixels=backend.ReadRGBA8(View(1),error);check(pixels.size()==1024);
    const auto* pixel=pixels.data()+(8*16+8)*4;
    check(std::to_integer<uint8_t>(pixel[0])==(frame==&white?255:0));
    check(std::to_integer<uint8_t>(pixel[1])==255);check(std::to_integer<uint8_t>(pixel[2])==(frame==&white?255:0));check(std::to_integer<uint8_t>(pixel[3])==255);
  }
  std::cout<<"Actual native touch/UI pixels passed: first atlas upload, cached second frame and white fallback\n";
#endif
  std::cout<<checks<<" production UI drawer checks passed\n";
}
