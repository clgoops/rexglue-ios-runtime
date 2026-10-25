// SPDX-License-Identifier: BSD-3-Clause
// Guest draw decisions follow the SDK command processor and common processors.
// Only host resource creation and submission are replaced by native Metal.
#include "rex/native/rex_live.h"
#include "rex/native/rex_shader.h"
#include "rex/native/rex_targets.h"
#include "rex/native/rex_textures.h"
#include "rex/native/rex_primitives.h"
#include "rex/native/rex_state.h"
#include <rex/graphics/command_processor.h>
#include <rex/string/buffer.h>
#include <rex/logging.h>
#include <bit>
#include <cstring>

namespace rex::native {
namespace gfx=graphics;
namespace {
using Translator=gfx::SpirvShaderTranslator;
constexpr const char* kSwapVertex=R"(#include <metal_stdlib>
using namespace metal;
vertex float4 swap_vertex(uint i [[vertex_id]]) {
 return float4(i==1?3.0:-1.0,i==2?-3.0:1.0,0.0,1.0);
})";
constexpr const char* kSwapFragment=R"(#include <metal_stdlib>
using namespace metal;
struct Gamma {uint table[256];uint pwl[384];uint swizzle;uint use_pwl;uint2 padding;};
float select_channel(float4 value,uint i) {return i<4?value[i]:(i==5?1.0:0.0);}
float pwl_channel(constant Gamma& g,uint value,uint c) {
 uint p=g.pwl[(value>>3)*3+c];return clamp((float(p&65535u)+float((value&7u)*(p>>16))*0.125)*1.52737048e-5,0.0,1.0);
}
fragment float4 swap_fragment(float4 pos [[position]],texture2d_array<float,access::read> image [[texture(0)]],constant Gamma& g [[buffer(0)]]) {
 float4 raw=image.read(uint2(pos.xy),0);float3 color=float3(select_channel(raw,g.swizzle&7u),select_channel(raw,(g.swizzle>>3)&7u),select_channel(raw,(g.swizzle>>6)&7u));
 color=clamp(color,0.0,1.0);
 if(g.use_pwl) {uint3 v=uint3(color*1023.0+0.5);return float4(pwl_channel(g,v.r,0),pwl_channel(g,v.g,1),pwl_channel(g,v.b,2),1);}
 uint3 v=uint3(color*255.0+0.5);return float4(float((g.table[v.r]>>20)&1023u),float((g.table[v.g]>>10)&1023u),float(g.table[v.b]&1023u),1023.0)/1023.0;
})";
class NativeCommandProcessor final:public gfx::CommandProcessor {
 public:
  NativeCommandProcessor(gfx::GraphicsSystem* gs,system::KernelState* kernel,std::shared_ptr<NativeService> service)
    :CommandProcessor(gs,kernel),service_(std::move(service)) {}
  void IssueSwap(uint32_t,uint32_t width,uint32_t height) override;
  void InitializeShaderStorage(const std::filesystem::path& root,uint32_t title,bool blocking) override {
    CommandProcessor::InitializeShaderStorage(root,title,blocking);std::string error;
    if(!service_->Drain(error)) {Fail(error);return;}
    auto path=root/"native-metal"/std::to_string(title)/"pipelines.metalarc";
    std::lock_guard lock(service_->mutex);
    if(!service_->backend.ConfigureBinaryArchive(path.string(),error))REXLOG_WARN("Native Metal pipeline archive unavailable: {}",error);
    else REXLOG_INFO("Native Metal pipeline archive: {}; Vulkan shader caches are separate",path.string());
  }
  void ClearCaches() override {
    // EDRAM ownership and GPU shared memory must remain coherent with the guest.
    // Retire only after submitting and awaiting all outstanding native work.
    CallInThread([this] {std::string error;if(!Flush(error)||!service_->Drain(error)) {Fail(error);return;}
      if(textures_)textures_->ClearCache();});
  }
  void InvalidateGpuMemory() override {if(shared_)shared_->InvalidateAllPages();}
 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  gfx::Shader* LoadShader(gfx::xenos::ShaderType,uint32_t,const uint32_t*,uint32_t) override;
  bool IssueDraw(gfx::xenos::PrimitiveType,uint32_t,IndexBufferInfo*,bool) override;
  bool IssueCopy() override;
  void WriteRegister(uint32_t index,uint32_t value) override {
    CommandProcessor::WriteRegister(index,value);
    if(textures_&&index>=gfx::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0&&index<gfx::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0+192)
      textures_->TextureFetchConstantWritten((index-gfx::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0)/6);
  }
  void PrepareForWait() override {std::string error;if(!Flush(error))Fail(error);CommandProcessor::PrepareForWait();}
  void OnPrimaryBufferEnd() override {std::string error;if(!Flush(error))Fail(error);}
  void MakeCoherent() override {CommandProcessor::MakeCoherent();}
 private:
  bool Begin(std::string&);
  bool Flush(std::string&);
  bool Fail(const std::string& error) {service_->Fail(error);return false;}
  bool Readback(uint32_t,uint32_t,std::string&);
  bool Resources(RexShader&,const ConvertedShader&,std::vector<DescriptorResources>&,std::string&);
  const ConvertedShader* Compile(RexShader&,uint64_t,std::string&);
  std::shared_ptr<NativeService> service_;
  std::function<Identity()> next_;
  Capabilities caps_;
  std::unique_ptr<RexSharedMemory> shared_;
  std::unique_ptr<RexTextures> textures_;
  std::unique_ptr<RexPrimitives> primitives_;
  std::unique_ptr<RexRenderTargets> targets_;
  std::unique_ptr<Translator> translator_;
  RexConstantState constants_;RexConstantState::System system_{};
  std::shared_ptr<Frame> frame_;
  uint64_t submission_=0;
  std::shared_ptr<std::atomic<uint64_t>> guest_completed_=std::make_shared<std::atomic<uint64_t>>(0);
  std::map<std::pair<uint32_t,std::vector<uint32_t>>,std::unique_ptr<RexShader>> shaders_;
  std::map<Pipeline,Identity> pipelines_;
  std::map<Sampler,Identity> samplers_;
  std::optional<ConvertedShader> depth_only_;
  Shader swap_vs_,swap_fs_;Pipeline swap_pipeline_;std::array<std::optional<Surface>,3> output_;uint32_t output_index_=0;
};
bool NativeCommandProcessor::SetupContext() {
  if(!CommandProcessor::SetupContext())return false;
  std::string error;if(!service_||!service_->backend.Ready(error))return Fail(error);
  next_=[this]{return service_->Next();};caps_=service_->backend.GetDeviceCapabilities();
  Translator::Features features(false);features.spirv_version=spv::Spv_1_5;
  features.max_storage_buffer_range=caps_.max_buffer_length>=512ull<<20?512u<<20:256u<<20;
  features.full_draw_index_uint32=true;features.vertex_pipeline_stores_and_atomics=true;features.fragment_stores_and_atomics=true;
  features.clip_distance=true;features.cull_distance=false;features.image_view_format_swizzle=false;
  features.signed_zero_inf_nan_preserve_float32=true;features.denorm_flush_to_zero_float32=true;features.rounding_mode_rte_float32=true;
  features.sample_rate_shading=true;translator_=std::make_unique<Translator>(features,true,true,false);
  shared_=std::make_unique<RexSharedMemory>(*memory_);
  std::vector<Identity> chunks(1u<<translator_->GetSharedMemoryStorageBufferCountLog2());for(auto& id:chunks)id=next_();
  if(!shared_->Initialize(chunks,error))return Fail(error);
  textures_=std::make_unique<RexTextures>(*register_file_,*shared_,next_);
  primitives_=std::make_unique<RexPrimitives>(*register_file_,*memory_,*shared_,next_);if(!primitives_->Initialize())return Fail("native primitive processor initialization failed");
  targets_=std::make_unique<RexRenderTargets>(*register_file_,*memory_,next_);targets_->Initialize();
  swap_vs_={next_(),Stage::Vertex,"swap_vertex",{},kSwapVertex,{}};
  swap_fs_={next_(),Stage::Fragment,"swap_fragment",{},kSwapFragment,{{ArgumentKind::Texture,0,1,0,1,TextureKind::TwoDArray},{ArgumentKind::Buffer,0,1,2576,16}}};
  swap_pipeline_.id=next_();swap_pipeline_.vertex=swap_vs_.id;swap_pipeline_.fragment=swap_fs_.id;swap_pipeline_.colors[0]=Format::RGBA8;
  REXLOG_INFO("Native Metal guest GPU active: {} shared-memory chunks, original resolution, no Vulkan command submission",chunks.size());
  return true;
}
void NativeCommandProcessor::ShutdownContext() {
  std::string error;if(frame_&&!Flush(error))Fail(error);if(service_&&!service_->Drain(error))Fail(error);
  if(service_) {std::lock_guard lock(service_->mutex);if(!service_->backend.SaveBinaryArchive(error))REXLOG_WARN("Could not save native Metal pipeline archive: {}",error);}
  primitives_.reset();textures_.reset();targets_.reset();shared_.reset();translator_.reset();shaders_.clear();CommandProcessor::ShutdownContext();
}
bool NativeCommandProcessor::Begin(std::string& error) {
  if(service_->failed) {error="native Metal session stopped";return false;}if(frame_)return true;
  frame_=std::make_shared<Frame>();if(!shared_->BeginFrame(*frame_,error))return false;
  // Submission indices here describe guest cache lifetime, independently of UI
  // queue submissions. Queue completion of any later UI packet also retires it.
  textures_->BeginNativeFrame(*frame_,++submission_,guest_completed_->load());primitives_->BeginNativeFrame(*frame_);return true;
}
bool NativeCommandProcessor::Flush(std::string& error) {
  if(!frame_)return !service_->failed;
  auto frame=std::move(frame_);auto completed=guest_completed_;auto submission=submission_;
  gfx::PerformanceStats::Scope submit_time(service_->performance,gfx::PerformanceStats::Stage::kSubmit);
  bool accepted=service_->Submit(frame,error,[completed,submission]{completed->store(submission);});
  if(accepted)service_->performance.RecordQueueSubmission();
  shared_->EndFrame(accepted);textures_->EndNativeFrame();primitives_->EndNativeFrame();
  // Keep texture LRU retirement bounded to completed guest work. Waiting is
  // avoided in the draw path; texture owner references protect pending packets.
  if(accepted)textures_->CompletedSubmissionUpdated(guest_completed_->load());
  if(!accepted)Fail(error);return accepted;
}
gfx::Shader* NativeCommandProcessor::LoadShader(gfx::xenos::ShaderType type,uint32_t,const uint32_t* words,uint32_t count) {
  auto key=std::make_pair(uint32_t(type),std::vector<uint32_t>(words,words+count));auto i=shaders_.find(key);if(i!=shaders_.end())return i->second.get();
  uint64_t hash=14695981039346656037ull;for(uint32_t word:key.second) {hash^=word;hash*=1099511628211ull;}
  auto shader=std::make_unique<RexShader>(type,hash,words,count);string::StringBuffer disasm;shader->AnalyzeUcode(disasm);
  auto* result=shader.get();shaders_.emplace(std::move(key),std::move(shader));return result;
}
const ConvertedShader* NativeCommandProcessor::Compile(RexShader& shader,uint64_t mod,std::string& error) {
  auto* translation=static_cast<RexShader::NativeTranslation*>(shader.GetOrCreateTranslation(mod));
  if(!translation->Prepare(*translator_,next_,error))return nullptr;auto converted=translation->converted();Declare(frame_->shaders,converted->shader);return converted.get();
}
bool NativeCommandProcessor::Resources(RexShader& source,const ConvertedShader&,std::vector<DescriptorResources>& descriptors,std::string& error) {
  uint32_t set=source.type()==gfx::xenos::ShaderType::kVertex?Translator::kDescriptorSetTexturesVertex:Translator::kDescriptorSetTexturesPixel;
  const auto& textures=source.GetTextureBindingsAfterTranslation();
  for(uint32_t i=0;i<textures.size();++i) {const auto& binding=textures[i];SurfaceView view;
    if(!textures_->Active(binding.fetch_constant,binding.dimension,binding.is_signed,view,error))return false;
    descriptors.push_back({ResourceSource::Descriptor,set,i,{},{view},{}});
  }
  const auto& samplers=source.GetSamplerBindingsAfterTranslation();
  for(uint32_t i=0;i<samplers.size();++i) {
    auto sampler=RexSamplerState(*register_file_,samplers[i]);auto fetch=register_file_->GetTextureFetch(samplers[i].fetch_constant);
    if(!caps_.float32_filtering&&(fetch.format==gfx::xenos::TextureFormat::k_32_FLOAT||fetch.format==gfx::xenos::TextureFormat::k_32_32_FLOAT||fetch.format==gfx::xenos::TextureFormat::k_32_32_32_32_FLOAT))
      {sampler.min=sampler.mag=sampler.mip=Filter::Nearest;sampler.anisotropy=1;}
    // Apple's packed A1BGR5 does not support opaque-black border color.
    if(fetch.format==gfx::xenos::TextureFormat::k_1_5_5_5&&sampler.border==Border::OpaqueBlack)sampler.border=Border::TransparentBlack;
    auto [it,added]=samplers_.try_emplace(sampler);if(added)it->second=next_();sampler.id=it->second;Declare(frame_->samplers,sampler);
    descriptors.push_back({ResourceSource::Descriptor,set,uint32_t(textures.size())+i,{},{},{sampler.id}});
  }
  return true;
}
bool NativeCommandProcessor::IssueDraw(gfx::xenos::PrimitiveType,uint32_t,IndexBufferInfo*,bool) {
  std::string error;auto& regs=*register_file_;if(regs.Get<gfx::reg::RB_MODECONTROL>().edram_mode==gfx::xenos::EdramMode::kCopy)return IssueCopy();
  if(service_->failed)return false;
  gfx::PerformanceStats::Scope draw_time(service_->performance,gfx::PerformanceStats::Stage::kDraw);
  auto* vs=static_cast<RexShader*>(active_vertex_shader());if(!vs)return Fail("draw has no vertex shader");
  bool polygonal=gfx::draw_util::IsPrimitivePolygonal(regs),raster=gfx::draw_util::IsRasterizationPotentiallyDone(regs,polygonal);
  if(raster&&!regs.Get<gfx::reg::RB_SURFACE_INFO>().surface_pitch)return true;
  auto* ps=raster&&regs.Get<gfx::reg::RB_MODECONTROL>().edram_mode==gfx::xenos::EdramMode::kColorDepth?static_cast<RexShader*>(active_pixel_shader()):nullptr;
  if(ps&&!gfx::draw_util::IsPixelShaderNeededWithRasterization(*ps,regs))ps=nullptr;
  bool memexport=vs->memexport_eM_written()||(ps&&ps->memexport_eM_written());if(!raster&&!memexport)return true;
  if(!Begin(error))return Fail(error);
  gfx::PrimitiveProcessor::ProcessingResult primitive{};if(!primitives_->Process(primitive))return Fail("native primitive conversion failed");if(!primitive.host_draw_vertex_count)return true;
  uint32_t param=UINT32_MAX;uint32_t interpolators=ps?vs->writes_interpolators()&ps->GetInterpolatorInputMask(regs.Get<gfx::reg::SQ_PROGRAM_CNTL>(),regs.Get<gfx::reg::SQ_CONTEXT_MISC>(),param):0;
  auto depth=gfx::draw_util::GetNormalizedDepthControl(regs);auto profile=targets_->Profile();
  auto host_type=raster?primitive.host_vertex_shader_type:gfx::Shader::HostVertexShaderType::kMemExportCompute;
  const auto* vertex=Compile(*vs,RexVertexModification(regs,profile,*translator_,*vs,host_type,interpolators,param!=UINT32_MAX).value,error);if(!vertex)return Fail(error);
  const ConvertedShader* pixel=nullptr;
  if(ps) {pixel=Compile(*ps,RexPixelModification(regs,profile,*translator_,*ps,interpolators,param,depth).value,error);if(!pixel)return Fail(error);}
  else if(raster&&depth.z_enable&&regs.Get<gfx::reg::RB_DEPTH_INFO>().depth_format==gfx::xenos::DepthRenderTargetFormat::kD24FS8) {
    if(!depth_only_) {auto bytes=translator_->CreateDepthOnlyFragmentShader(Translator::Modification::DepthStencilMode::kFloat24Truncating);
      ConvertedShader converted;if(!ConvertSpirv({reinterpret_cast<const uint32_t*>(bytes.data()),bytes.size()/4},next_(),Stage::Fragment,"main",converted,error))return Fail(error);depth_only_=std::move(converted);}
    pixel=&*depth_only_;Declare(frame_->shaders,pixel->shader);
  }
  uint32_t mask=ps?gfx::draw_util::GetNormalizedColorMask(regs,ps->writes_color_targets()):0;
  uint32_t used=vs->GetUsedTextureMaskAfterTranslation()|(ps?ps->GetUsedTextureMaskAfterTranslation():0);RexTextureState texture_state;
  if(!textures_->Prepare(used,texture_state,error))return Fail(error);
  Pass pass;if(raster&&!targets_->PrepareDraw(*frame_,true,depth,mask,*vs,pass,error))return Fail(error);
  gfx::draw_util::ViewportInfo viewport{};gfx::draw_util::GetHostViewportInfo(regs,1,1,true,16384,16384,true,depth,true,true,ps&&ps->writes_depth(),viewport);
  UpdateRexSystemConstants(regs,profile,texture_state,system_,polygonal,primitive,false,raster?0:primitive.guest_draw_vertex_count,viewport,used,depth,mask);
  // Upload every currently used fetch range; dirty tracking coalesces the
  // unchanged pages, including GPU-written resolve contents.
  const auto& vm=vs->constant_register_map();
  for(uint32_t bank=0;bank<std::size(vm.vertex_fetch_bitmap);++bank)for(uint32_t bits=vm.vertex_fetch_bitmap[bank];bits;bits&=bits-1) {
    auto fetch=regs.GetVertexFetch(bank*32+std::countr_zero(bits));
    if(fetch.type!=gfx::xenos::FetchConstantType::kVertex&&fetch.type!=gfx::xenos::FetchConstantType::kInvalidVertex)return Fail("invalid native vertex fetch type");
    if(fetch.size&&!shared_->RequestRange(fetch.address<<2,fetch.size<<2))return Fail("native vertex memory upload failed");
  }
  std::vector<gfx::draw_util::MemExportRange> exports;
  if(vs->memexport_eM_written())gfx::draw_util::AddMemExportRanges(regs,*vs,exports);if(ps&&ps->memexport_eM_written())gfx::draw_util::AddMemExportRanges(regs,*ps,exports);
  for(const auto& r:exports)if(!shared_->RequestRange(r.base_address_dwords<<2,r.size_bytes))return Fail("native memexport memory upload failed");
  if(memexport&&exports.empty()&&!shared_->RequestRange(0,gfx::SharedMemory::kBufferSize))return Fail("native unknown memexport residency failed");
  if(!raster&&primitive.index_buffer_type!=gfx::PrimitiveProcessor::kNone) {
    uint32_t length=primitive.guest_draw_vertex_count*(regs.Get<gfx::reg::VGT_DRAW_INITIATOR>().index_size==gfx::xenos::IndexFormat::kInt16?2:4);
    if(!shared_->RequestRange(primitive.guest_index_base,length))return Fail("native compute index memory upload failed");
  }
  std::vector<const ConvertedShader*> stages={vertex};if(pixel)stages.push_back(pixel);
  std::vector<DescriptorResources> resources={shared_->Descriptor()};gfx::Shader::ConstantRegisterMap empty_map{};
  if(!constants_.Prepare(system_,regs,vm,ps?ps->constant_register_map():empty_map,stages,next_,*frame_,resources,error)||!Resources(*vs,*vertex,resources,error)||(ps&&!Resources(*ps,*pixel,resources,error)))return Fail(error);
  Bindings vertex_bindings,pixel_bindings;std::optional<Buffer> aux;
  if(!BindConverted(*vertex,resources,next_(),vertex_bindings,aux,error))return Fail(error);if(aux)frame_->buffers.push_back(*aux);
  if(pixel) {if(!BindConverted(*pixel,resources,next_(),pixel_bindings,aux,error))return Fail(error);if(aux)frame_->buffers.push_back(*aux);}
  if(!raster) {
    Dispatch dispatch;dispatch.shader=vertex->shader.id;dispatch.bindings=std::move(vertex_bindings);dispatch.threads=vertex->workgroup_size;
    dispatch.groups={(primitive.guest_draw_vertex_count+dispatch.threads[0]-1)/dispatch.threads[0],1,1};frame_->commands.push_back(std::move(dispatch));
  } else {
    Pipeline pipeline;Draw draw;if(!RexPipelineState(regs,depth,mask,polygonal,viewport,pass,*frame_,vertex->shader.id,pixel?pixel->shader.id:Identity{},pipeline,draw,error)) {
      if(error=="both polygon faces culled")return true;return Fail(error);
    }
    if(primitive.index_buffer_type==gfx::PrimitiveProcessor::kGuestDMA) {
      uint32_t length=primitive.host_draw_vertex_count*(primitive.host_index_format==gfx::xenos::IndexFormat::kInt16?2:4);
      if(!shared_->RequestRange(primitive.guest_index_base,length))return Fail("native index memory upload failed");
    }
    if(!primitives_->Bind(primitive,draw,error))return Fail(error);
    if(memexport&&primitive.index_buffer_type==gfx::PrimitiveProcessor::kGuestDMA&&draw.indices) {
      BufferStorage snapshot{next_(),draw.indices->length,true,{}};snapshot.owner=std::make_shared<const Identity>(snapshot.id);frame_->buffer_storage.push_back(snapshot);
      BufferView view{snapshot.id,0,snapshot.size};frame_->commands.push_back(BufferCopy{*draw.indices,view,snapshot.size});draw.indices=view;
    }
    auto [it,added]=pipelines_.try_emplace(pipeline);if(added)it->second=next_();pipeline.id=it->second;Declare(frame_->pipelines,pipeline);
    draw.pipeline=pipeline.id;draw.bindings[0]=std::move(vertex_bindings);draw.bindings[1]=std::move(pixel_bindings);
    pass.mergeable=!memexport;pass.commands.push_back(std::move(draw));AppendPass(*frame_,std::move(pass));
  }
  service_->performance.RecordDrawCall();
  if(memexport) {
    if(exports.empty())shared_->RangeWrittenByGpu(0,gfx::SharedMemory::kBufferSize);
    for(const auto& r:exports)shared_->RangeWrittenByGpu(r.base_address_dwords<<2,r.size_bytes);
    if(IsReadbackMemexportEnabled(false))for(const auto& r:exports)if(!Readback(r.base_address_dwords<<2,r.size_bytes,error))return Fail(error);
  }
  if(!frame_)return true;
  uint64_t staging=0;for(const auto& b:frame_->buffer_storage)if(b.size!=(512ull<<20)&&b.size!=(256ull<<20))staging+=b.size;
  if(staging>64ull<<20||frame_->commands.size()>4096)if(!Flush(error))return Fail(error);
  return true;
}
bool NativeCommandProcessor::Readback(uint32_t address,uint32_t length,std::string& error) {
  while(length) {
    uint32_t amount=std::min(length,32u<<20);if(!Begin(error))return false;
    auto source=shared_->ContiguousView(address,amount,next_,error);if(!source)return false;
    auto readback_owner=frame_; // Preserve a cross-chunk staging allocation through the CPU copy.
    if(!Flush(error))return false;std::lock_guard lock(service_->mutex);
    if(!service_->backend.Drain(std::chrono::seconds(30),error))return false;
    auto bytes=service_->backend.ReadBuffer(*source,error);if(bytes.size()!=amount)return false;
    std::memcpy(memory_->TranslatePhysical(address),bytes.data(),amount);address+=amount;length-=amount;
  }
  return true;
}
bool NativeCommandProcessor::IssueCopy() {
  gfx::PerformanceStats::Scope resolve_time(service_->performance,gfx::PerformanceStats::Stage::kResolve);
  service_->performance.RecordResolveCall();
  std::string error;if(!Begin(error))return Fail(error);uint32_t address=0,length=0;
  if(!targets_->Resolve(*frame_,*memory_,*shared_,*textures_,address,length,error))return Fail(error);
  if(length&&GetReadbackResolveMode(false)!=gfx::ReadbackResolveMode::kDisabled&&!Readback(address,length,error))return Fail(error);
  return true;
}
void NativeCommandProcessor::IssueSwap(uint32_t,uint32_t width,uint32_t height) {
  std::string error;if(!Begin(error)) {Fail(error);return;}SurfaceView source;uint32_t swizzle;
  if(!textures_->Swap(source,swizzle,error)) {Fail("native swap: "+error);return;}
  const auto s=std::find_if(frame_->surfaces.begin(),frame_->surfaces.end(),[&](const Surface& s){return s.id==source.surface;});
  if(s==frame_->surfaces.end()) {Fail("native swap texture is missing");return;}
  width=width?std::min(width,s->width):s->width;height=height?std::min(height,s->height):s->height;
  if(!width||!height) {Fail("native swap size is zero");return;}
  auto& slot=output_[output_index_++%output_.size()];
  if(!slot||slot->width!=width||slot->height!=height) {Surface target;target.id=next_();target.width=width;target.height=height;target.format=Format::RGBA8;target.renderable=true;target.owner=std::make_shared<const Identity>(target.id);slot=target;}
  Surface image=*slot;Declare(frame_->surfaces,image);Declare(frame_->shaders,swap_vs_);Declare(frame_->shaders,swap_fs_);Declare(frame_->pipelines,swap_pipeline_);
  struct alignas(16) Gamma {uint32_t table[256],pwl[384],swizzle,use_pwl,padding[2];};Gamma gamma{};
  std::memcpy(gamma.table,gamma_ramp_256_entry_table(),sizeof(gamma.table));std::memcpy(gamma.pwl,gamma_ramp_pwl_rgb(),sizeof(gamma.pwl));gamma.swizzle=swizzle;
  auto format=register_file_->GetTextureFetch(0).format;gamma.use_pwl=format==gfx::xenos::TextureFormat::k_2_10_10_10||format==gfx::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
  auto bytes=std::make_shared<std::vector<std::byte>>(sizeof(gamma));std::memcpy(bytes->data(),&gamma,sizeof(gamma));Buffer constants{next_(),bytes,true};frame_->buffers.push_back(constants);
  Pass pass;pass.width=width;pass.height=height;pass.colors[0]=Attachment{{image.id},Load::Discard};Draw draw;draw.pipeline=swap_pipeline_.id;draw.vertices=3;draw.viewport={0,0,double(width),double(height),0,1};draw.scissor={0,0,width,height};
  draw.bindings[1].textures={{0,source}};draw.bindings[1].buffers={{0,{constants.id,0,sizeof(gamma)}}};pass.commands.push_back(std::move(draw));AppendPass(*frame_,std::move(pass));
  if(!Flush(error)) {Fail(error);return;}
  if(auto* presenter=graphics_system_->presenter())presenter->RefreshGuestOutput(width,height,width,height,[image](ui::Presenter::GuestOutputRefreshContext& c) {
    auto& context=static_cast<NativeRefreshContext&>(c);context.output=image;context.SetIs8bpc(true);return true;
  });
}
}
std::unique_ptr<gfx::CommandProcessor> CreateNativeCommandProcessor(gfx::GraphicsSystem* gs,system::KernelState* kernel,std::shared_ptr<NativeService> service) {
  return std::make_unique<NativeCommandProcessor>(gs,kernel,std::move(service));
}
} // namespace rex::native
