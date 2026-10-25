// SPDX-License-Identifier: BSD-3-Clause
#include "rex/native/spirv.h"
#include <spirv_msl.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace rex::native {
namespace {
using namespace spirv_cross;
struct Failure:std::runtime_error {using std::runtime_error::runtime_error;};
void Require(bool ok,const char* message) {if(!ok)throw Failure(message);}
spv::ExecutionModel Model(Stage stage) {
  switch(stage) {
    case Stage::Vertex:return spv::ExecutionModelVertex;
    case Stage::Fragment:return spv::ExecutionModelFragment;
    case Stage::Compute:return spv::ExecutionModelGLCompute;
    default:throw Failure("unsupported shader stage");
  }
}
uint32_t ArrayCount(const SPIRType& type) {
  uint64_t count=1;
  for(size_t i=0;i<type.array.size();++i) {
    Require(type.array_size_literal[i] && type.array[i],"runtime/specialized descriptor arrays need explicit lowering");
    count*=type.array[i];Require(count<=128,"descriptor array exceeds native slot budget");
  }
  return uint32_t(count);
}
TextureKind ImageKind(const SPIRType& type) {
  switch(type.image.dim) {
    case spv::Dim1D:return type.image.arrayed?TextureKind::TwoDArray:TextureKind::TwoD;
    case spv::DimBuffer:Require(!type.image.arrayed && !type.image.ms,"invalid linear texel image shape");return TextureKind::TwoD;
    case spv::Dim2D:return type.image.arrayed?TextureKind::TwoDArray:TextureKind::TwoD;
    case spv::DimCube:return type.image.arrayed?TextureKind::CubeArray:TextureKind::Cube;
    case spv::Dim3D:Require(!type.image.arrayed,"3D arrays require lowering");return TextureKind::ThreeD;
    default:throw Failure("subpass images need explicit native lowering");
  }
}
struct Pending {Resource resource;ResourceSource source;ArgumentKind kind;bool storage=false;};
} // namespace

bool ConvertSpirv(std::span<const uint32_t> words,Identity id,Stage stage,std::string_view entry,
                  ConvertedShader& output,std::string& error,const SpirvOptions& options) {
  output={};error.clear();
  try {
    Require(bool(id),"shader identity is missing");
    Require(words.size()>=5 && words[0]==0x07230203,"invalid SPIR-V header");
    Require(words.size()<=4*1024*1024,"SPIR-V exceeds conversion budget");
    Require(words[3] && words[3]<=1024*1024,"SPIR-V ID bound exceeds conversion budget");
    Require(options.buffer_size_slot<31 && options.max_texture_slots<=128 && options.max_sampler_slots<=16,
            "native resource slot limit is invalid");
    // Bound instruction lengths before handing malformed input to the parser.
    for(size_t i=5;i<words.size();) {
      uint32_t count=words[i]>>16;
      Require(count && count<=words.size()-i,"truncated SPIR-V instruction");i+=count;
    }
    CompilerMSL compiler(words.data(),words.size());
    const auto model=Model(stage);
    compiler.set_entry_point(std::string(entry),model);
    auto active=compiler.get_active_interface_variables();
    auto resources=compiler.get_shader_resources(active);
    compiler.set_enabled_interface_variables(std::move(active));
    Require(resources.subpass_inputs.empty() && resources.atomic_counters.empty() &&
            resources.acceleration_structures.empty() && resources.gl_plain_uniforms.empty() &&
            resources.tensors.empty() && resources.shader_record_buffers.empty(),
            "unsupported SPIR-V resource class");
    Require(stage!=Stage::Vertex || resources.stage_inputs.empty(),
            "vertex attributes require lowering to the buffer-pull ABI");
    CompilerMSL::Options msl;
    msl.platform=CompilerMSL::Options::iOS;
    msl.set_msl_version(2,3);
    msl.argument_buffers=false;
    msl.texture_1D_as_2D=true;
    msl.texture_buffer_native=false;
    msl.texel_buffer_texture_width=0;
    msl.enable_decoration_binding=false;
    msl.buffer_size_buffer_index=options.buffer_size_slot;
    msl.ios_support_base_vertex_instance=true;
    // The working renderer depends on full precision; do not enable unsafe
    // algebraic rewrites or the previous lighting-breaking precision changes.
    msl.invariant_float_math=true;
    compiler.set_msl_options(msl);
    std::vector<Pending> pending;
    auto add=[&](const auto& list,ArgumentKind kind,bool storage=false,ResourceSource source=ResourceSource::Descriptor) {
      for(const auto& r:list)pending.push_back({r,source,kind,storage});
    };
    add(resources.uniform_buffers,ArgumentKind::Buffer);
    add(resources.storage_buffers,ArgumentKind::Buffer,true);
    add(resources.push_constant_buffers,ArgumentKind::Buffer,false,ResourceSource::PushConstants);
    add(resources.separate_images,ArgumentKind::Texture);
    add(resources.storage_images,ArgumentKind::Texture,true);
    add(resources.separate_samplers,ArgumentKind::Sampler);
    add(resources.sampled_images,ArgumentKind::Texture);
    add(resources.sampled_images,ArgumentKind::Sampler);
    auto descriptor=[&](const Pending& p) {
      if(p.source==ResourceSource::PushConstants)return std::pair{uint32_t(~0u),uint32_t(0)};
      Require(compiler.has_decoration(p.resource.id,spv::DecorationDescriptorSet) &&
              compiler.has_decoration(p.resource.id,spv::DecorationBinding),"descriptor lacks set or binding decoration");
      return std::pair{compiler.get_decoration(p.resource.id,spv::DecorationDescriptorSet),
                       compiler.get_decoration(p.resource.id,spv::DecorationBinding)};
    };
    std::sort(pending.begin(),pending.end(),[&](const Pending& a,const Pending& b) {
      return std::tuple{descriptor(a),a.kind}<std::tuple{descriptor(b),b.kind};
    });
    std::map<std::pair<uint32_t,uint32_t>,MSLResourceBinding> remaps;
    std::set<std::tuple<uint32_t,uint32_t,ArgumentKind>> seen;
    uint32_t buffer_slot=0,texture_slot=0,sampler_slot=0;
    ConvertedShader converted;converted.shader.id=id;converted.shader.stage=stage;
    for(const auto& p:pending) {
      auto [set,binding]=descriptor(p);
      Require(seen.insert({set,binding,p.kind}).second,"duplicate descriptor slot in selected stage");
      const auto& type=compiler.get_type(p.resource.type_id);
      Argument a;a.kind=p.kind;a.count=ArrayCount(type);
      auto& remap=remaps[{set,binding}];remap.stage=model;remap.desc_set=set;remap.binding=binding;remap.count=a.count;
      if(p.kind==ArgumentKind::Buffer) {
        // Keep the auxiliary slot unoccupied even if a future translation adds
        // more uniform banks. Metal reflection will check real struct alignment.
        if(buffer_slot+a.count>options.buffer_size_slot && buffer_slot<=options.buffer_size_slot)
          buffer_slot=options.buffer_size_slot+1;
        Require(buffer_slot+a.count<=31,"shader exceeds native buffer slot budget");
        a.index=buffer_slot;buffer_slot+=a.count;remap.msl_buffer=a.index;
        const auto& base=compiler.get_type(p.resource.base_type_id);
        a.minimum_bytes=compiler.get_declared_struct_size_runtime_array(base,1);
        Require(a.minimum_bytes>0,"buffer block has no declared storage");
        a.writable=p.storage && !compiler.get_buffer_block_flags(p.resource.id).get(spv::DecorationNonWritable);
      } else if(p.kind==ArgumentKind::Texture) {
        Require(texture_slot+a.count<=options.max_texture_slots,"shader exceeds native texture slot budget");
        a.index=texture_slot;texture_slot+=a.count;remap.msl_texture=a.index;
        a.texture_kind=ImageKind(type);a.multisampled=type.image.ms;
        a.writable=p.storage && !compiler.has_decoration(p.resource.id,spv::DecorationNonWritable);
      } else {
        Require(sampler_slot+a.count<=options.max_sampler_slots,"shader exceeds native sampler slot budget");
        a.index=sampler_slot;sampler_slot+=a.count;remap.msl_sampler=a.index;
      }
      auto lowering=p.kind!=ArgumentKind::Texture?TextureLowering::None:
        type.image.dim==spv::DimBuffer?TextureLowering::LinearTexels:
        type.image.dim==spv::Dim1D?TextureLowering::OneDAsRow:TextureLowering::None;
      converted.resources.push_back({p.source,set,binding,a,p.resource.id,lowering});
    }
    for(const auto& [key,remap]:remaps)compiler.add_msl_resource_binding(remap);
    for(const auto& c:compiler.get_specialization_constants()) {
      const auto& value=compiler.get_constant(c.id);
      const auto& type=compiler.get_type(value.constant_type);
      Require(type.vecsize==1 && type.columns==1 && type.array.empty(),"non-scalar specialization constant requires lowering");
      Shader::ConstantType kind;
      if(type.basetype==SPIRType::Boolean)kind=Shader::ConstantType::Bool;
      else {
        Require(type.width==32,"specialization constant is not 32-bit");
        switch(type.basetype) {
          case SPIRType::UInt:kind=Shader::ConstantType::UInt;break;
          case SPIRType::Int:kind=Shader::ConstantType::Int;break;
          case SPIRType::Float:kind=Shader::ConstantType::Float;break;
          default:throw Failure("unsupported specialization constant scalar type");
        }
      }
      converted.shader.constants.push_back({c.constant_id,value.scalar(),kind});
    }
    converted.shader.msl=compiler.compile();
    converted.shader.entry=compiler.get_cleansed_entry_point_name(std::string(entry),model);
    for(const auto& r:converted.resources) {
      Require(compiler.is_msl_resource_binding_used(model,r.descriptor_set,r.binding),
              "reflected resource was not realized with its explicit Metal binding");
      converted.shader.arguments.push_back(r.argument);
    }
    if(compiler.needs_buffer_size_buffer()) {
      Argument sizes;sizes.index=options.buffer_size_slot;sizes.minimum_bytes=31*sizeof(uint32_t);sizes.alignment=4;
      converted.resources.push_back({ResourceSource::BufferSizes,0,0,sizes,0});
      converted.shader.arguments.push_back(sizes);
    }
    Require(!compiler.needs_swizzle_buffer() && !compiler.needs_view_mask_buffer() &&
            !compiler.needs_dispatch_base_buffer() && !compiler.needs_output_buffer(),
            "shader requires an unimplemented implicit Metal resource");
    if(stage==Stage::Compute) {
      // LocalSizeId needs the frontend to specialize before conversion; do not
      // silently dispatch with a different group size from the compiled kernel.
      auto& ep=compiler.get_entry_point(std::string(entry),model);
      SpecializationConstant sx{},sy{},sz{};
      compiler.get_work_group_size_specialization_constants(sx,sy,sz);
      Require(!ep.workgroup_size.id_x && !ep.workgroup_size.id_y && !ep.workgroup_size.id_z,
              "specialized workgroup size requires explicit lowering");
      Require(!sx.id && !sy.id && !sz.id,"specialized builtin workgroup size requires explicit lowering");
      converted.workgroup_size={ep.workgroup_size.x,ep.workgroup_size.y,ep.workgroup_size.z};
      if(ep.workgroup_size.constant) {
        const auto& value=compiler.get_constant(ep.workgroup_size.constant);
        Require(!value.specialization,"specialized builtin workgroup size requires explicit lowering");
        for(uint32_t i=0;i<3;++i)converted.workgroup_size[i]=value.scalar(0,i);
      }
      Require(converted.workgroup_size[0] && converted.workgroup_size[1] && converted.workgroup_size[2],
              "compute workgroup size is missing");
    }
    output=std::move(converted);return true;
  } catch(const std::exception& e) {error=e.what();return false;}
}

bool BindConverted(const ConvertedShader& shader,std::span<const DescriptorResources> descriptors,
                   Identity size_buffer_id,Bindings& output,std::optional<Buffer>& size_buffer,std::string& error) {
  output={};size_buffer.reset();error.clear();
  try {
    Bindings bindings;std::array<uint32_t,31> sizes{};
    const ResourceMapping* auxiliary=nullptr;
    for(size_t i=0;i<descriptors.size();++i)for(size_t j=0;j<i;++j)
      Require(std::tuple{descriptors[i].source,descriptors[i].descriptor_set,descriptors[i].binding}!=
              std::tuple{descriptors[j].source,descriptors[j].descriptor_set,descriptors[j].binding},
              "duplicate descriptor resources");
    for(const auto& r:shader.resources) {
      const auto& a=r.argument;
      Require(a.alignment && (a.alignment&(a.alignment-1))==0,"invalid reflected argument alignment");
      Require(a.kind<ArgumentKind::Count && a.count,"invalid reflected argument kind or count");
      if(r.source==ResourceSource::BufferSizes) {Require(!auxiliary,"duplicate auxiliary size buffer");auxiliary=&r;continue;}
      auto found=std::find_if(descriptors.begin(),descriptors.end(),[&](const DescriptorResources& d) {
        return d.source==r.source && (r.source==ResourceSource::PushConstants ||
          (d.descriptor_set==r.descriptor_set && d.binding==r.binding));
      });
      Require(found!=descriptors.end(),"required shader descriptor is missing");
      auto count=a.kind==ArgumentKind::Buffer?found->buffers.size():a.kind==ArgumentKind::Texture?found->textures.size():found->samplers.size();
      Require(count==a.count,"shader descriptor count differs from compiled ABI");
      for(uint32_t i=0;i<a.count;++i) {
        if(a.kind==ArgumentKind::Buffer) {
          const auto& v=found->buffers[i];
          Require(bool(v.buffer) && v.length>=a.minimum_bytes,"shader buffer view is missing or undersized");
          Require(v.offset%a.alignment==0,"shader buffer offset violates alignment");
          Require(a.index+i<sizes.size() && v.length<=UINT32_MAX,"buffer-size reflection exceeds native bounds");
          bindings.buffers.push_back({a.index+i,v});sizes[a.index+i]=uint32_t(v.length);
        } else if(a.kind==ArgumentKind::Texture) {
          Require(bool(found->textures[i].surface),"shader texture identity is missing");
          bindings.textures.push_back({a.index+i,found->textures[i]});
        } else {
          Require(bool(found->samplers[i]),"shader sampler identity is missing");
          bindings.samplers.push_back({a.index+i,found->samplers[i]});
        }
      }
    }
    std::optional<Buffer> generated;
    if(auxiliary) {
      Require(bool(size_buffer_id),"array-length buffer requires a fresh immutable identity");
      auto bytes=std::make_shared<std::vector<std::byte>>(sizeof(sizes));
      memcpy(bytes->data(),sizes.data(),sizeof(sizes));
      generated=Buffer{size_buffer_id,bytes,true};
      bindings.buffers.push_back({auxiliary->argument.index,{size_buffer_id,0,sizeof(sizes)}});
    }
    output=std::move(bindings);size_buffer=std::move(generated);return true;
  } catch(const std::exception& e) {error=e.what();return false;}
}
} // namespace rex::native
