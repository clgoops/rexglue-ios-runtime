// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/spirv.h"
#include <fstream>
#include <iostream>
#include <sstream>

static std::string Quote(std::string_view value) {
  std::ostringstream out;out<<'"';
  for(unsigned char c:value) {
    if(c=='"' || c=='\\')out<<'\\'<<c;
    else if(c=='\n')out<<"\\n";
    else if(c<32) {constexpr char hex[]="0123456789abcdef";out<<"\\u00"<<hex[c>>4]<<hex[c&15];}
    else out<<c;
  }
  out<<'"';return out.str();
}
int main(int argc,char** argv) {
  using namespace rex::native;
  if(argc!=5) {std::cerr<<"usage: native-shader-export vertex|fragment|compute input.spv output.metal output.json\n";return 2;}
  Stage stage;
  if(std::string_view(argv[1])=="vertex")stage=Stage::Vertex;
  else if(std::string_view(argv[1])=="fragment")stage=Stage::Fragment;
  else if(std::string_view(argv[1])=="compute")stage=Stage::Compute;
  else {std::cerr<<"invalid stage\n";return 2;}
  std::ifstream file(argv[2],std::ios::binary|std::ios::ate);
  auto length=file.tellg();
  if(length<20 || length>16*1024*1024 || length%4) {std::cerr<<"invalid input length\n";return 2;}
  std::vector<uint32_t> words(size_t(length)/4);file.seekg(0);
  if(!file.read(reinterpret_cast<char*>(words.data()),length)) {std::cerr<<"cannot read input\n";return 2;}
  ConvertedShader converted;std::string error;
  if(!ConvertSpirv(words,{1,1},stage,"main",converted,error)) {std::cerr<<error<<'\n';return 1;}
  std::ofstream msl(argv[3],std::ios::binary),manifest(argv[4],std::ios::binary);
  msl<<converted.shader.msl;
  manifest<<"{\n  \"entry\": "<<Quote(converted.shader.entry)<<",\n  \"stage\": "<<Quote(argv[1])<<",\n  \"workgroup_size\": [";
  for(int i=0;i<3;++i)manifest<<(i?", ":"")<<converted.workgroup_size[i];
  manifest<<"],\n  \"resources\": [";
  for(size_t i=0;i<converted.resources.size();++i) {
    const auto& r=converted.resources[i];const auto& a=r.argument;
    manifest<<(i?",":"")<<"\n    {\"source\": "<<int(r.source)<<", \"set\": "<<r.descriptor_set<<", \"binding\": "<<r.binding
      <<", \"kind\": "<<int(a.kind)<<", \"index\": "<<a.index<<", \"count\": "<<a.count
      <<", \"minimum_bytes\": "<<a.minimum_bytes<<", \"alignment\": "<<a.alignment
      <<", \"texture_kind\": "<<int(a.texture_kind)<<", \"multisampled\": "<<(a.multisampled?"true":"false")
      <<", \"writable\": "<<(a.writable?"true":"false")<<", \"texture_lowering\": "<<int(r.texture_lowering)<<"}";
  }
  manifest<<"\n  ],\n  \"constants\": [";
  for(size_t i=0;i<converted.shader.constants.size();++i) {
    const auto& c=converted.shader.constants[i];manifest<<(i?", ":"")<<"{\"index\": "<<c.index<<", \"bits\": "<<c.bits<<", \"type\": "<<int(c.type)<<"}";
  }
  manifest<<"]\n}\n";
  msl.close();manifest.close();
  if(!msl || !manifest) {std::cerr<<"cannot write outputs\n";return 2;}
  std::cout<<converted.shader.entry<<": "<<converted.resources.size()<<" explicit resource mappings\n";
}
