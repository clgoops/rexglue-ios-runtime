// SPDX-License-Identifier: BSD-3-Clause
#if !__has_feature(objc_arc)
#error Native Metal ownership requires ARC
#endif
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#include "rex/native/metal_backend.h"
#include "rex/native/lru.h"
#include "rex/native/upload_probe.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <set>

namespace rex::native {
namespace {
constexpr const char* kUploadVertex=R"(#include <metal_stdlib>
using namespace metal;vertex void upload_vertex(uint i[[vertex_id]],const device uint4* src[[buffer(0)]],device uint4* dst[[buffer(1)]]){dst[i]=src[i];}
)";
bool Fail(std::string& error,const char* text){error=text;return false;}
std::string DescribeError(NSError* e){return e?std::string(e.localizedDescription.UTF8String):"Metal returned no object";}
MTLPixelFormat Pixel(Format f) {
  switch(f) {
    case Format::R8:return MTLPixelFormatR8Unorm;
    case Format::RG8:return MTLPixelFormatRG8Unorm;
    case Format::RGBA8:return MTLPixelFormatRGBA8Unorm;
    case Format::RGBA8Srgb:return MTLPixelFormatRGBA8Unorm_sRGB;
    case Format::BGRA8:return MTLPixelFormatBGRA8Unorm;
    case Format::BGRA8Srgb:return MTLPixelFormatBGRA8Unorm_sRGB;
    case Format::RGB10A2:return MTLPixelFormatRGB10A2Unorm;
    case Format::RG16Float:return MTLPixelFormatRG16Float;
    case Format::RGBA16Float:return MTLPixelFormatRGBA16Float;
    case Format::RG16Snorm:return MTLPixelFormatRG16Snorm;
    case Format::RGBA16Snorm:return MTLPixelFormatRGBA16Snorm;
    case Format::RG16Uint:return MTLPixelFormatRG16Uint;
    case Format::RGBA16Uint:return MTLPixelFormatRGBA16Uint;
    case Format::R32Uint:return MTLPixelFormatR32Uint;
    case Format::RG32Uint:return MTLPixelFormatRG32Uint;
    case Format::Stencil8View:return MTLPixelFormatX32_Stencil8;
    case Format::R8Snorm:return MTLPixelFormatR8Snorm;
    case Format::RG8Snorm:return MTLPixelFormatRG8Snorm;
    case Format::RGBA8Snorm:return MTLPixelFormatRGBA8Snorm;
    case Format::R16Float:return MTLPixelFormatR16Float;
    case Format::R16Unorm:return MTLPixelFormatR16Unorm;
    case Format::R16Snorm:return MTLPixelFormatR16Snorm;
    case Format::RG16Unorm:return MTLPixelFormatRG16Unorm;
    case Format::RGBA16Unorm:return MTLPixelFormatRGBA16Unorm;
#if TARGET_OS_IPHONE
    case Format::BGR565:return MTLPixelFormatB5G6R5Unorm;
    case Format::BGR5A1:return MTLPixelFormatBGR5A1Unorm;
    case Format::ABGR4:return MTLPixelFormatABGR4Unorm;
#else
    case Format::BGR565:case Format::BGR5A1:case Format::ABGR4:return MTLPixelFormatInvalid;
#endif
    case Format::R32Float:return MTLPixelFormatR32Float;
    case Format::RG32Float:return MTLPixelFormatRG32Float;
    case Format::RGBA32Float:return MTLPixelFormatRGBA32Float;
    case Format::Depth32Float:return MTLPixelFormatDepth32Float;
    case Format::Depth32Stencil8:return MTLPixelFormatDepth32Float_Stencil8;
    case Format::BC1:return MTLPixelFormatBC1_RGBA;
    case Format::BC1Srgb:return MTLPixelFormatBC1_RGBA_sRGB;
    case Format::BC2:return MTLPixelFormatBC2_RGBA;
    case Format::BC2Srgb:return MTLPixelFormatBC2_RGBA_sRGB;
    case Format::BC3:return MTLPixelFormatBC3_RGBA;
    case Format::BC3Srgb:return MTLPixelFormatBC3_RGBA_sRGB;
    case Format::BC4:return MTLPixelFormatBC4_RUnorm;
    case Format::BC4Signed:return MTLPixelFormatBC4_RSnorm;
    case Format::BC5:return MTLPixelFormatBC5_RGUnorm;
    case Format::BC5Signed:return MTLPixelFormatBC5_RGSnorm;
    case Format::BC6Float:return MTLPixelFormatBC6H_RGBUfloat;
    case Format::BC6SignedFloat:return MTLPixelFormatBC6H_RGBFloat;
    case Format::BC7:return MTLPixelFormatBC7_RGBAUnorm;
    case Format::BC7Srgb:return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    case Format::ASTC4x4:return MTLPixelFormatASTC_4x4_LDR;
    case Format::ASTC4x4Srgb:return MTLPixelFormatASTC_4x4_sRGB;
    default:return MTLPixelFormatInvalid;
  }
}
MTLTextureType TextureType(const Surface& s) {
  if(s.samples>1)return MTLTextureType2DMultisample;
  switch(s.kind) {
    case TextureKind::TwoD:return MTLTextureType2D;
    case TextureKind::TwoDArray:return MTLTextureType2DArray;
    case TextureKind::Cube:return MTLTextureTypeCube;
    case TextureKind::CubeArray:return MTLTextureTypeCubeArray;
    case TextureKind::ThreeD:return MTLTextureType3D;
    default:return MTLTextureType2D;
  }
}
MTLCompareFunction Comparison(Compare c){return MTLCompareFunction(uint32_t(c));}
MTLStencilOperation StencilOp(StencilOperation s){
  static constexpr MTLStencilOperation table[]={MTLStencilOperationKeep,MTLStencilOperationZero,MTLStencilOperationReplace,MTLStencilOperationIncrementClamp,MTLStencilOperationDecrementClamp,MTLStencilOperationInvert,MTLStencilOperationIncrementWrap,MTLStencilOperationDecrementWrap};
  return table[size_t(s)];
}
MTLBlendFactor Factor(BlendFactor f){
  static constexpr MTLBlendFactor table[]={MTLBlendFactorZero,MTLBlendFactorOne,MTLBlendFactorSourceColor,MTLBlendFactorOneMinusSourceColor,MTLBlendFactorSourceAlpha,MTLBlendFactorOneMinusSourceAlpha,MTLBlendFactorDestinationColor,MTLBlendFactorOneMinusDestinationColor,MTLBlendFactorDestinationAlpha,MTLBlendFactorOneMinusDestinationAlpha,MTLBlendFactorBlendColor,MTLBlendFactorOneMinusBlendColor,MTLBlendFactorBlendAlpha,MTLBlendFactorOneMinusBlendAlpha,MTLBlendFactorSourceAlphaSaturated};
  return table[size_t(f)];
}
MTLBlendOperation BlendOp(BlendOperation o){return MTLBlendOperation(uint32_t(o));}
MTLColorWriteMask WriteMask(uint8_t mask){
  return MTLColorWriteMask((mask&1?MTLColorWriteMaskRed:0)|(mask&2?MTLColorWriteMaskGreen:0)|
      (mask&4?MTLColorWriteMaskBlue:0)|(mask&8?MTLColorWriteMaskAlpha:0));
}
MTLPrimitiveType Topology(Primitive p){
  static constexpr MTLPrimitiveType table[]={MTLPrimitiveTypePoint,MTLPrimitiveTypeLine,MTLPrimitiveTypeLineStrip,MTLPrimitiveTypeTriangle,MTLPrimitiveTypeTriangleStrip};return table[size_t(p)];
}
MTLSamplerAddressMode Addressing(Address a){
  static constexpr MTLSamplerAddressMode table[]={MTLSamplerAddressModeClampToEdge,MTLSamplerAddressModeRepeat,
    MTLSamplerAddressModeMirrorRepeat,MTLSamplerAddressModeClampToZero,MTLSamplerAddressModeMirrorClampToEdge,
    MTLSamplerAddressModeClampToBorderColor};return table[size_t(a)];
}
bool SameOwner(const std::weak_ptr<const void>& weak,const std::shared_ptr<const void>& owner){
  auto old=weak.lock();return old&&!old.owner_before(owner)&&!owner.owner_before(old);
}
struct Pool {
  std::mutex mutex;
  uint64_t geometry=0,constants=0,geometry_limit=0,constant_limit=0;
  std::multimap<uint64_t,id<MTLBuffer>> free_constants;
};
struct Allocation {
  id<MTLBuffer> buffer=nil;
  std::shared_ptr<Pool> pool;
  bool constants=false;
  ~Allocation(){
    std::lock_guard lock(pool->mutex);
    if(constants)pool->free_constants.emplace(buffer.length,buffer);
    else pool->geometry-=buffer.length;
  }
};
struct SharedState {
  std::mutex mutex;std::condition_variable changed;
  uint32_t pending=0;bool lost=false;std::string loss;
  BackendStats stats;
};
struct FrameLease {
  std::shared_ptr<const Frame> frame;
  std::shared_ptr<SharedState> state;
  std::vector<id> gpu;
  std::vector<std::shared_ptr<Allocation>> allocations;
  std::atomic<bool> retired=false;
  void Retire(){
    if(retired.exchange(true))return;
    // Release source owners/resources before publishing retirement to Drain.
    gpu.clear();allocations.clear();frame.reset();
    {std::lock_guard lock(state->mutex);--state->pending;}
    state->changed.notify_all();
  }
  ~FrameLease(){Retire();}
};
struct ShaderObject {Shader source;id<MTLLibrary> library=nil;id<MTLFunction> function=nil;};
struct PipelineObject {Pipeline source;id<MTLFunction> vertex=nil,fragment=nil;id<MTLRenderPipelineState> pipeline=nil;id<MTLDepthStencilState> depth=nil;std::array<std::vector<Argument>,2> arguments;};
struct ComputeObject {Shader source;id<MTLComputePipelineState> pipeline=nil;std::vector<Argument> arguments;};
struct BufferObject {std::weak_ptr<const void> owner;bool constants=false;std::shared_ptr<Allocation> allocation;};
struct SurfaceObject {Surface source;std::weak_ptr<const void> owner;id<MTLTexture> texture=nil;Lru<SurfaceView,id<MTLTexture>> views;};
struct StorageObject {uint64_t size=0;bool writable=false;std::weak_ptr<const void> owner;id<MTLBuffer> buffer=nil;};
struct SamplerObject {Sampler source;id<MTLSamplerState> sampler=nil;};
struct ClearKey {
  std::array<Format,4> colors{};Format depth=Format::Invalid,stencil=Format::Invalid;
  uint32_t samples=1;uint8_t mask=0;bool clear_depth=false,clear_stencil=false;
  auto operator<=>(const ClearKey&) const=default;
};
struct ClearObject {id<MTLRenderPipelineState> pipeline=nil;id<MTLDepthStencilState> depth=nil;};
}

struct MetalBackend::Impl {
  BackendOptions options;
  id<MTLDevice> device=MTLCreateSystemDefaultDevice();id<MTLCommandQueue> queue=nil;
  CAMetalLayer* layer=nil;id<MTLBinaryArchive> archive=nil;NSURL* archive_url=nil;
  bool archive_dirty=false;
  std::shared_ptr<SharedState> state=std::make_shared<SharedState>();
  std::shared_ptr<Pool> pool=std::make_shared<Pool>();
  BackendStats counters;
  std::map<Identity,SurfaceObject> surfaces;
  std::map<Identity,StorageObject> storage;
  std::map<Identity,id<MTLBuffer>> active_buffers;
  std::map<Identity,PipelineObject> active_pipelines;
  std::map<Identity,SamplerObject> active_samplers;
  Lru<Identity,BufferObject> buffers;
  Lru<Identity,ShaderObject> shaders;
  Lru<Identity,PipelineObject> pipelines;
  Lru<Identity,ComputeObject> computes;
  Lru<Identity,SamplerObject> samplers;
  Lru<ClearKey,ClearObject> clears;
  Lru<ClearKey,id<MTLRenderPipelineState>> upload_pipelines;
  id<MTLFunction> upload_vertex=nil;
  std::map<Identity,SurfaceObject>::iterator surface_sweep=surfaces.end();
  std::map<Identity,StorageObject>::iterator storage_sweep=storage.end();
  uint64_t surface_bytes=0,storage_bytes=0;
  uint64_t last_sequence=0;
  std::string startup_error;
  id<MTLRenderPipelineState> present_pipeline=nil;
  MTLPixelFormat present_format=MTLPixelFormatInvalid;
  explicit Impl(BackendOptions o):options(o){
    if(!device){startup_error="No native Metal device is available";return;}
    if(!options.maximum_frames||options.maximum_frames>3||!options.maximum_pipelines||!options.maximum_shaders||!options.maximum_samplers){startup_error="Invalid native backend limits";return;}
    queue=[device newCommandQueue];if(!queue){startup_error="Metal command queue creation failed";return;}
    queue.label=@"ReX native ordered frames";
    pool->geometry_limit=options.geometry_budget;pool->constant_limit=options.constant_budget;
  }
  void Snapshot(){
    counters.surface_bytes=surface_bytes;
    {std::lock_guard lock(pool->mutex);counters.geometry_bytes=pool->geometry;counters.constant_bytes=pool->constants;}
    std::lock_guard lock(state->mutex);auto complete=state->stats.completed;state->stats=counters;state->stats.completed=complete;
  }
  void Sweep(bool full=false){
    const size_t surface_count=full?surfaces.size():std::min<size_t>(64,surfaces.size());
    for(size_t n=0;n<surface_count&&!surfaces.empty();++n){
      if(surface_sweep==surfaces.end())surface_sweep=surfaces.begin();
      auto i=surface_sweep++;
      if(!i->second.owner.expired())continue;
      surface_bytes-=i->second.texture.allocatedSize;
      surfaces.erase(i);
    }
    const size_t storage_count=full?storage.size():std::min<size_t>(64,storage.size());
    for(size_t n=0;n<storage_count&&!storage.empty();++n){
      if(storage_sweep==storage.end())storage_sweep=storage.begin();
      auto i=storage_sweep++;if(!i->second.owner.expired())continue;
      storage_bytes-=i->second.buffer.length;storage.erase(i);
    }
  }
  bool SurfaceCapacity(uint64_t requested,const Surface& spec,std::string& error){
    auto exceeds=[&]{return requested>options.surface_budget||surface_bytes>options.surface_budget-requested;};
    // Normal maintenance is incremental. Before rejecting a packet, examine
    // every cache entry so expired images behind 64 live entries are reclaimed.
    // Never discard a live owner or an image retained by an in-flight packet.
    if(exceeds())Sweep(true);
    if(exceeds()) {
      // Admission has reserved one slot for the packet being prepared, but it
      // has not been encoded yet. On pressure only, let older packets retire
      // before making the final decision; their frame leases may be the last
      // owners of targets just removed from the guest cache.
      std::unique_lock lock(state->mutex);
      if(!state->changed.wait_for(lock,std::chrono::seconds(5),[&]{return state->pending<=1||state->lost;}))
        return Fail(error,"Native surface pressure did not retire within five seconds");
      if(state->lost) {error=state->loss;return false;}
      lock.unlock();Sweep(true);
    }
    if(!exceeds())return true;
    error="Native surface budget exhausted: live="+std::to_string(surface_bytes)+
      " requested="+std::to_string(requested)+" budget="+std::to_string(options.surface_budget)+
      " bytes, surfaces="+std::to_string(surfaces.size())+" new="+std::to_string(spec.width)+
      "x"+std::to_string(spec.height)+" samples="+std::to_string(spec.samples)+
      " format="+std::to_string(uint32_t(spec.format));
    return false;
  }
  bool Surfaces(const Frame& f,std::shared_ptr<FrameLease> lease,std::string& error){
    Sweep();
    for(const Surface& spec:f.surfaces){
      auto found=surfaces.find(spec.id);Surface shape=spec;shape.owner.reset();
      if(found!=surfaces.end()){
        if(found->second.source!=shape||!SameOwner(found->second.owner,spec.owner))return Fail(error,"Surface identity was reused with different storage or ownership");
        lease->gpu.push_back(found->second.texture);continue;
      }
      if(spec.format>=Format::BC1&&spec.format<=Format::BC7Srgb&&!device.supportsBCTextureCompression)return Fail(error,"This device needs prepared texture formats instead of BC compression");
      if(![device supportsTextureSampleCount:spec.samples])return Fail(error,"Metal does not support the requested sample count");
      auto desc=[MTLTextureDescriptor new];desc.textureType=TextureType(spec);desc.pixelFormat=Pixel(spec.format);
      desc.width=spec.width;desc.height=spec.height;desc.depth=spec.depth;desc.mipmapLevelCount=spec.levels;
      desc.arrayLength=(spec.kind==TextureKind::Cube||spec.kind==TextureKind::CubeArray)?spec.layers/6:spec.layers;
      desc.sampleCount=spec.samples;desc.storageMode=MTLStorageModePrivate;
      desc.usage=MTLTextureUsagePixelFormatView|(spec.renderable?MTLTextureUsageRenderTarget:0)|(spec.sampled?MTLTextureUsageShaderRead:0)|(spec.storage?MTLTextureUsageShaderWrite:0);
      const auto estimate=[device heapTextureSizeAndAlignWithDescriptor:desc].size;
      if(!SurfaceCapacity(estimate,spec,error))return false;
      id<MTLTexture> texture=[device newTextureWithDescriptor:desc];if(!texture)return Fail(error,"Native texture allocation failed");
      const uint64_t actual=texture.allocatedSize;
      if(!SurfaceCapacity(actual,spec,error))return false;
      surface_bytes+=actual;++counters.surface_allocations;surfaces.emplace(spec.id,SurfaceObject{shape,spec.owner,texture});lease->gpu.push_back(texture);
    }
    for(const auto& spec:f.buffer_storage){
      auto i=storage.find(spec.id);
      if(i!=storage.end()){
        if(i->second.size!=spec.size||i->second.writable!=spec.writable||!SameOwner(i->second.owner,spec.owner))return Fail(error,"Mutable buffer identity or ownership changed without a generation change");
        lease->gpu.push_back(i->second.buffer);active_buffers[spec.id]=i->second.buffer;continue;
      }
      if(spec.size>device.maxBufferLength||spec.size>options.mutable_buffer_budget||storage_bytes>options.mutable_buffer_budget-spec.size)return Fail(error,"Mutable GPU buffer budget exceeded");
      id<MTLBuffer> buffer=[device newBufferWithLength:spec.size options:MTLResourceStorageModePrivate];
      if(!buffer)return Fail(error,"Mutable GPU buffer allocation failed");
      storage_bytes+=buffer.length;storage.emplace(spec.id,StorageObject{spec.size,spec.writable,spec.owner,buffer});lease->gpu.push_back(buffer);active_buffers[spec.id]=buffer;
    }
    return true;
  }
  id<MTLTexture> Texture(const SurfaceView& view,bool sample,std::string& error){
    auto i=surfaces.find(view.surface);if(i==surfaces.end()){Fail(error,"Unknown native texture");return nil;}
    auto base=i->second.texture;auto shape=i->second.source;
    SurfaceView key=view;if(!sample){key.level=key.slice=key.plane=0;}
    if(key.format==Format::Invalid)key.format=shape.format;
    if(!key.level&&!key.slice&&!key.plane&&key.format==shape.format)return base;
    if(auto* found=i->second.views.Find(key))return *found;
    const uint32_t slices=shape.kind==TextureKind::ThreeD?1:shape.layers-key.slice;
    auto texture=[base newTextureViewWithPixelFormat:Pixel(key.format) textureType:TextureType(shape)
        levels:NSMakeRange(key.level,shape.levels-key.level) slices:NSMakeRange(key.slice,slices)];
    if(!texture){Fail(error,"Native texture view creation failed");return nil;}
    i->second.views.Insert(key,texture);i->second.views.Trim(128);return texture;
  }
  bool Buffers(const Frame& f,std::shared_ptr<FrameLease> lease,std::string& error){
    for(const auto& spec:f.buffers){
      if(auto* cached=buffers.Find(spec.id)){
        if(cached->constants!=spec.constants||!SameOwner(cached->owner,spec.bytes))return Fail(error,"Immutable buffer identity changed without a generation change");
        lease->allocations.push_back(cached->allocation);active_buffers[spec.id]=cached->allocation->buffer;++counters.buffer_reuses;continue;
      }
      const uint64_t length=spec.constants?((spec.bytes->size()+255)&~uint64_t(255)):spec.bytes->size();
      if(length>device.maxBufferLength)return Fail(error,"Immutable buffer exceeds the Metal buffer limit");
      id<MTLBuffer> buffer=nil;
      if(spec.constants){
        std::lock_guard lock(pool->mutex);
        auto free=pool->free_constants.lower_bound(length);
        if(free!=pool->free_constants.end()){
          buffer=free->second;pool->free_constants.erase(free);++counters.constant_pool_reuses;
        }
      }
      if(!buffer){
        uint64_t limit=spec.constants?pool->constant_limit:pool->geometry_limit;
        for(;;){
          bool room;{std::lock_guard lock(pool->mutex);auto used=spec.constants?pool->constants:pool->geometry;room=length<=limit&&used<=limit-length;}
          if(room)break;
          if(spec.constants){
            std::lock_guard lock(pool->mutex);
            auto free=pool->free_constants.lower_bound(length);
            if(free!=pool->free_constants.end()){buffer=free->second;pool->free_constants.erase(free);++counters.constant_pool_reuses;break;}
            // Retired pages too small for this request can be released. Pages
            // still held by a GPU frame never enter this free list.
            if(!pool->free_constants.empty()){auto old=pool->free_constants.begin();pool->constants-=old->first;pool->free_constants.erase(old);continue;}
          }
          if(!buffers.Pop())return Fail(error,"Immutable GPU storage is still owned by pending frames or exceeds its budget");
        }
        if(!buffer){
          buffer=[device newBufferWithLength:length options:MTLResourceStorageModeShared];if(!buffer)return Fail(error,"Immutable GPU buffer allocation failed");
          std::lock_guard lock(pool->mutex);if(spec.constants)pool->constants+=buffer.length;else pool->geometry+=buffer.length;
        }
      }
      std::memcpy(buffer.contents,spec.bytes->data(),spec.bytes->size());
      auto allocation=std::make_shared<Allocation>();allocation->buffer=buffer;allocation->pool=pool;allocation->constants=spec.constants;
      buffers.Insert(spec.id,BufferObject{spec.bytes,spec.constants,allocation});lease->allocations.push_back(allocation);active_buffers[spec.id]=buffer;++counters.buffer_uploads;
    }
    return true;
  }
  id<MTLBuffer> BufferAt(const BufferView& view){
    auto i=active_buffers.find(view.buffer);return i==active_buffers.end()?nil:i->second;
  }
  id<MTLFunction> Function(const Shader& source,std::shared_ptr<FrameLease> lease,std::string& error){
    if(auto* cached=shaders.Find(source.id)){
      if(cached->source!=source){Fail(error,"Shader identity changed without a generation change");return nil;}
      lease->gpu.push_back(cached->function);return cached->function;
    }
    NSError* failure=nil;id<MTLLibrary> library=nil;
    if(!source.msl.empty()){
      auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;options.languageVersion=MTLLanguageVersion2_3;
      library=[device newLibraryWithSource:[[NSString alloc] initWithBytes:source.msl.data() length:source.msl.size() encoding:NSUTF8StringEncoding] options:options error:&failure];
    }else{
      auto bytes=source.library;
      dispatch_data_t data=dispatch_data_create(bytes->data(),bytes->size(),nullptr,^{(void)bytes;});
      library=[device newLibraryWithData:data error:&failure];
    }
    if(!library){error=DescribeError(failure);return nil;}
    NSString* entry=[NSString stringWithUTF8String:source.entry.c_str()];id<MTLFunction> function=nil;
    if(source.constants.empty())function=[library newFunctionWithName:entry];
    else{
      auto values=[MTLFunctionConstantValues new];
      for(const auto& constant:source.constants){
        uint32_t bits=constant.bits;bool boolean=bits!=0;
        MTLDataType type=constant.type==Shader::ConstantType::Bool?MTLDataTypeBool:constant.type==Shader::ConstantType::UInt?MTLDataTypeUInt:constant.type==Shader::ConstantType::Int?MTLDataTypeInt:MTLDataTypeFloat;
        [values setConstantValue:constant.type==Shader::ConstantType::Bool?(const void*)&boolean:(const void*)&bits type:type atIndex:constant.index];
      }
      function=[library newFunctionWithName:entry constantValues:values error:&failure];
    }
    if(!function){error=failure?DescribeError(failure):"Metal shader entry point is absent";return nil;}
    const auto expected=source.stage==Stage::Vertex?MTLFunctionTypeVertex:source.stage==Stage::Fragment?MTLFunctionTypeFragment:MTLFunctionTypeKernel;
    if(function.functionType!=expected){Fail(error,"Metal shader stage differs from the declared stage");return nil;}
    if(source.stage==Stage::Vertex&&function.vertexAttributes.count){Fail(error,"Vertex attributes need explicit frontend lowering to the buffer-pull ABI");return nil;}
    shaders.Insert(source.id,ShaderObject{source,library,function});shaders.Trim(options.maximum_shaders);lease->gpu.push_back(function);return function;
  }
  bool Reflect(const Shader& shader,NSArray<MTLArgument*>* reflection,std::vector<Argument>& output,std::string& error){
    output.clear();
    for(MTLArgument* actual in reflection){
      if(!actual.active)continue;
      ArgumentKind kind;
      if(actual.type==MTLArgumentTypeBuffer)kind=ArgumentKind::Buffer;
      else if(actual.type==MTLArgumentTypeTexture)kind=ArgumentKind::Texture;
      else if(actual.type==MTLArgumentTypeSampler)kind=ArgumentKind::Sampler;
      else return Fail(error,"Native shader has an unsupported reflected argument type");
      auto declared=std::find_if(shader.arguments.begin(),shader.arguments.end(),[&](const Argument& a){return a.kind==kind&&actual.index>=a.index&&actual.index-a.index<a.count;});
      if(declared==shader.arguments.end())return Fail(error,"An active Metal argument is absent from the reflected shader ABI");
      Argument checked=*declared;checked.index=uint32_t(actual.index);checked.count=uint32_t(std::max(NSUInteger(1),actual.arrayLength));
      if(checked.count>declared->count-(checked.index-declared->index))return Fail(error,"Metal argument array exceeds the declared ABI");
      if(kind==ArgumentKind::Buffer){checked.minimum_bytes=std::max(checked.minimum_bytes,uint64_t(actual.bufferDataSize));checked.alignment=std::max(checked.alignment,uint64_t(actual.bufferAlignment));}
      if(kind!=ArgumentKind::Sampler&&actual.access!=MTLArgumentAccessReadOnly&&!checked.writable)return Fail(error,"Metal shader writes were not declared in its ABI");
      if(kind==ArgumentKind::Texture){
        TextureKind shape;bool multisampled=false;
        switch(actual.textureType){
          case MTLTextureType2D:shape=TextureKind::TwoD;break;
          case MTLTextureType2DMultisample:shape=TextureKind::TwoD;multisampled=true;break;
          case MTLTextureType2DArray:shape=TextureKind::TwoDArray;break;
          case MTLTextureTypeCube:shape=TextureKind::Cube;break;
          case MTLTextureTypeCubeArray:shape=TextureKind::CubeArray;break;
          case MTLTextureType3D:shape=TextureKind::ThreeD;break;
          default:return Fail(error,"Unsupported Metal texture shape in the shader ABI");
        }
        if(shape!=checked.texture_kind||multisampled!=checked.multisampled)return Fail(error,"Metal texture reflection differs from the exported ABI");
      }
      output.push_back(checked);
    }
    return true;
  }
  const Shader& ShaderAt(const Frame& frame,Identity id){return *std::find_if(frame.shaders.begin(),frame.shaders.end(),[&](const Shader& s){return s.id==id;});}
  PipelineObject* PipelineAt(const Frame& frame,const Pipeline& source,std::shared_ptr<FrameLease> lease,std::string& error){
    const Shader& vertex=ShaderAt(frame,source.vertex);const Shader* fragment=source.fragment?&ShaderAt(frame,source.fragment):nullptr;
    id<MTLFunction> vs=Function(vertex,lease,error);if(!vs)return nullptr;
    id<MTLFunction> ps=fragment?Function(*fragment,lease,error):nil;if(fragment&&!ps)return nullptr;
    if(auto* cached=pipelines.Find(source.id)){
      if(cached->source!=source){Fail(error,"Pipeline identity changed without a generation change");return nullptr;}
      if(cached->vertex==vs&&cached->fragment==ps){++counters.pipeline_hits;lease->gpu.push_back(cached->pipeline);lease->gpu.push_back(cached->depth);return cached;}
    }
    auto descriptor=[MTLRenderPipelineDescriptor new];descriptor.vertexFunction=vs;descriptor.fragmentFunction=ps;
    for(size_t i=0;i<4;++i){
      auto a=descriptor.colorAttachments[i];a.pixelFormat=Pixel(source.colors[i]);const auto& b=source.blend[i];
      a.writeMask=WriteMask(b.write_mask);a.blendingEnabled=b.enabled;
      a.sourceRGBBlendFactor=Factor(b.source_rgb);a.destinationRGBBlendFactor=Factor(b.destination_rgb);
      a.sourceAlphaBlendFactor=Factor(b.source_alpha);a.destinationAlphaBlendFactor=Factor(b.destination_alpha);
      a.rgbBlendOperation=BlendOp(b.rgb);a.alphaBlendOperation=BlendOp(b.alpha);
    }
    descriptor.depthAttachmentPixelFormat=Pixel(source.depth);descriptor.stencilAttachmentPixelFormat=Pixel(source.stencil);
    descriptor.rasterSampleCount=source.samples;descriptor.alphaToCoverageEnabled=source.alpha_to_coverage;
    if(archive){descriptor.binaryArchives=@[archive];NSError* e=nil;if([archive addRenderPipelineFunctionsWithDescriptor:descriptor error:&e])archive_dirty=true;}
    NSError* e=nil;MTLRenderPipelineReflection* reflection=nil;
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:descriptor options:MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&e];
    if(!pipeline&&archive){descriptor.binaryArchives=nil;e=nil;pipeline=[device newRenderPipelineStateWithDescriptor:descriptor options:MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&e];}
    if(!pipeline){error=DescribeError(e);return nullptr;}
    PipelineObject object;object.source=source;object.pipeline=pipeline;object.vertex=vs;object.fragment=ps;
    if(!Reflect(vertex,reflection.vertexArguments,object.arguments[0],error)||(fragment&&!Reflect(*fragment,reflection.fragmentArguments,object.arguments[1],error)))return nullptr;
    auto depth=[MTLDepthStencilDescriptor new];depth.depthCompareFunction=Comparison(source.depth_compare);depth.depthWriteEnabled=source.depth_write;
    if(source.stencil_enabled){
      auto convert=[](const Stencil& s){auto out=[MTLStencilDescriptor new];out.stencilCompareFunction=Comparison(s.compare);out.stencilFailureOperation=StencilOp(s.fail);out.depthFailureOperation=StencilOp(s.depth_fail);out.depthStencilPassOperation=StencilOp(s.pass);out.readMask=s.read_mask;out.writeMask=s.write_mask;return out;};
      depth.frontFaceStencil=convert(source.front);depth.backFaceStencil=convert(source.back);
    }
    object.depth=[device newDepthStencilStateWithDescriptor:depth];if(!object.depth){Fail(error,"Native depth/stencil state creation failed");return nullptr;}
    lease->gpu.push_back(pipeline);lease->gpu.push_back(object.depth);++counters.pipeline_compiles;
    pipelines.Insert(source.id,std::move(object));pipelines.Trim(options.maximum_pipelines);return pipelines.Find(source.id);
  }
  bool Prepare(const Frame& frame,std::shared_ptr<FrameLease> lease,std::string& error){
    active_buffers.clear();active_pipelines.clear();active_samplers.clear();
    if(!Surfaces(frame,lease,error)||!Buffers(frame,lease,error))return false;
    for(const auto& pipeline:frame.pipelines){auto* object=PipelineAt(frame,pipeline,lease,error);if(!object)return false;active_pipelines.emplace(pipeline.id,*object);}
    for(const auto& s:frame.samplers){
      auto* object=samplers.Find(s.id);
      if(object&&object->source!=s)return Fail(error,"Sampler identity changed without a generation change");
      if(!object){
        auto d=[MTLSamplerDescriptor new];d.minFilter=s.min==Filter::Linear?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
        d.magFilter=s.mag==Filter::Linear?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
        d.mipFilter=s.mip==Filter::Linear?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;
        d.sAddressMode=Addressing(s.u);d.tAddressMode=Addressing(s.v);d.rAddressMode=Addressing(s.w);
        d.lodMinClamp=s.min_lod;d.lodMaxClamp=s.max_lod;d.maxAnisotropy=s.anisotropy;
        d.borderColor=s.border==Border::OpaqueWhite?MTLSamplerBorderColorOpaqueWhite:
          s.border==Border::OpaqueBlack?MTLSamplerBorderColorOpaqueBlack:MTLSamplerBorderColorTransparentBlack;
        id<MTLSamplerState> native=[device newSamplerStateWithDescriptor:d];if(!native)return Fail(error,"Native sampler creation failed");
        object=&samplers.Insert(s.id,SamplerObject{s,native});samplers.Trim(options.maximum_samplers);object=samplers.Find(s.id);
      }
      active_samplers.emplace(s.id,*object);lease->gpu.push_back(object->sampler);
    }
    return true;
  }
  bool ActualBuffers(const Bindings& bindings,const std::vector<Argument>& reflected,std::string& error){
    for(const auto& a:reflected)if(a.kind==ArgumentKind::Buffer)for(uint32_t i=0;i<a.count;++i){
      auto b=std::find_if(bindings.buffers.begin(),bindings.buffers.end(),[&](const BufferBinding& b){return b.index==a.index+i;});
      if(b==bindings.buffers.end()||b->view.length<a.minimum_bytes||!a.alignment||b->view.offset%a.alignment||!BufferAt(b->view))return Fail(error,"Buffer view does not satisfy the compiled Metal argument reflection");
    }
    return true;
  }
  struct EncoderBindings {
    struct BoundBuffer {id<MTLBuffer> buffer=nil;uint64_t offset=0;};
    std::array<std::array<BoundBuffer,31>,2> buffers{};
    std::array<std::array<id<MTLTexture>,128>,2> textures{};
    std::array<std::array<id<MTLSamplerState>,16>,2> samplers{};
    id<MTLRenderPipelineState> pipeline=nil;id<MTLDepthStencilState> depth=nil;
    bool depth_set=false;
  };
  bool Bind(id<MTLRenderCommandEncoder> encoder,const Bindings& bindings,size_t stage,EncoderBindings& cached,
      std::shared_ptr<FrameLease> lease,std::string& error){
    for(const auto& b:bindings.buffers){
      id<MTLBuffer> buffer=BufferAt(b.view);if(!buffer)return Fail(error,"Prepared native buffer is absent");
      auto& old=cached.buffers[stage][b.index];
      if(old.buffer==buffer&&old.offset==b.view.offset){++counters.avoided_bindings;continue;}
      if(old.buffer==buffer){if(stage==0)[encoder setVertexBufferOffset:b.view.offset atIndex:b.index];else [encoder setFragmentBufferOffset:b.view.offset atIndex:b.index];}
      else {if(stage==0)[encoder setVertexBuffer:buffer offset:b.view.offset atIndex:b.index];else [encoder setFragmentBuffer:buffer offset:b.view.offset atIndex:b.index];}
      old={buffer,b.view.offset};
    }
    for(const auto& t:bindings.textures){
      auto texture=Texture(t.view,true,error);if(!texture)return false;lease->gpu.push_back(texture);
      auto& old=cached.textures[stage][t.index];if(old==texture){++counters.avoided_bindings;continue;}
      if(stage==0)[encoder setVertexTexture:texture atIndex:t.index];else [encoder setFragmentTexture:texture atIndex:t.index];old=texture;
    }
    for(const auto& s:bindings.samplers){
      auto sampler=active_samplers.at(s.sampler).sampler;auto& old=cached.samplers[stage][s.index];
      if(old==sampler){++counters.avoided_bindings;continue;}
      if(stage==0)[encoder setVertexSamplerState:sampler atIndex:s.index];else [encoder setFragmentSamplerState:sampler atIndex:s.index];old=sampler;
    }
    return true;
  }
  void Attachment(MTLRenderPassAttachmentDescriptor* destination,const native::Attachment& a,std::string& error){
    auto texture=Texture(a.view,false,error);destination.texture=texture;
    destination.level=a.view.level;destination.slice=a.view.slice;destination.depthPlane=a.view.plane;
    destination.loadAction=a.load==Load::Clear?MTLLoadActionClear:a.load==Load::Load?MTLLoadActionLoad:MTLLoadActionDontCare;
    destination.storeAction=a.store==Store::Store?MTLStoreActionStore:MTLStoreActionDontCare;
  }
  bool DrawCall(id<MTLRenderCommandEncoder> e,const Draw& d,EncoderBindings& cache,
      std::shared_ptr<FrameLease> lease,std::string& error){
    if(!d.scissor.width||!d.scissor.height)return true;
    const auto& p=active_pipelines.at(d.pipeline);
    if(!ActualBuffers(d.bindings[0],p.arguments[0],error)||!ActualBuffers(d.bindings[1],p.arguments[1],error))return false;
    if(cache.pipeline!=p.pipeline){[e setRenderPipelineState:p.pipeline];cache.pipeline=p.pipeline;}else ++counters.avoided_bindings;
    if(!cache.depth_set||cache.depth!=p.depth){[e setDepthStencilState:p.depth];cache.depth=p.depth;cache.depth_set=true;}else ++counters.avoided_bindings;
    if(!Bind(e,d.bindings[0],0,cache,lease,error)||!Bind(e,d.bindings[1],1,cache,lease,error))return false;
    const auto& v=d.viewport;[e setViewport:MTLViewport{v.x,v.y,v.width,v.height,v.near_z,v.far_z}];
    [e setScissorRect:MTLScissorRect{d.scissor.x,d.scissor.y,d.scissor.width,d.scissor.height}];
    [e setCullMode:d.cull==Cull::Front?MTLCullModeFront:d.cull==Cull::Back?MTLCullModeBack:MTLCullModeNone];
    [e setFrontFacingWinding:d.clockwise?MTLWindingClockwise:MTLWindingCounterClockwise];
    [e setDepthClipMode:d.depth_clamp?MTLDepthClipModeClamp:MTLDepthClipModeClip];
    [e setStencilFrontReferenceValue:d.stencil_front backReferenceValue:d.stencil_back];
    [e setBlendColorRed:d.blend_color[0] green:d.blend_color[1] blue:d.blend_color[2] alpha:d.blend_color[3]];
    [e setDepthBias:d.depth_bias slopeScale:d.slope_bias clamp:0];
    if(!d.scissor.width||!d.scissor.height)return true;
    if(d.indices){if(d.index_count)[e drawIndexedPrimitives:Topology(d.primitive) indexCount:d.index_count indexType:d.index_bytes==2?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:BufferAt(*d.indices) indexBufferOffset:d.indices->offset instanceCount:d.instances baseVertex:d.base_vertex baseInstance:0];}
    else if(d.vertices)[e drawPrimitives:Topology(d.primitive) vertexStart:d.first_vertex vertexCount:d.vertices instanceCount:d.instances];
    ++counters.draws;return true;
  }
  ClearObject* ClearPipeline(const ClearKey& key,std::string& error){
    if(auto* p=clears.Find(key))return p;
    std::string source="#include <metal_stdlib>\nusing namespace metal;\nstruct C {float4 color[4];float depth;};\nvertex float4 clear_vs(uint i[[vertex_id]]){float2 p=float2((i<<1)&2,i&2);return float4(p*2-1,0,1);}\n";
    if(key.mask||key.clear_depth){
      source+="struct O{";
      auto integer=[](Format f){return f==Format::RG16Uint||f==Format::RGBA16Uint||f==Format::R32Uint||f==Format::RG32Uint;};
      for(size_t i=0;i<4;++i)if(key.mask&(1u<<i))source+=std::string(integer(key.colors[i])?"uint4 c":"float4 c")+std::to_string(i)+" [[color("+std::to_string(i)+")]];";
      if(key.clear_depth)source+="float d[[depth(any)]];";
      source+="};fragment O clear_ps(constant C& c[[buffer(0)]]){O o;";
      for(size_t i=0;i<4;++i)if(key.mask&(1u<<i))source+="o.c"+std::to_string(i)+"="+
        (integer(key.colors[i])?"as_type<uint4>(c.color[":"c.color[")+std::to_string(i)+(integer(key.colors[i])?"]);":"];\n");
      if(key.clear_depth)source+="o.d=c.depth;";
      source+="return o;}";
    } else source+="fragment void clear_ps(){}";
    auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;NSError* failure=nil;
    id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&failure];
    if(!library){error=DescribeError(failure);return nullptr;}
    auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=[library newFunctionWithName:@"clear_vs"];desc.fragmentFunction=[library newFunctionWithName:@"clear_ps"];
    for(size_t i=0;i<4;++i){desc.colorAttachments[i].pixelFormat=Pixel(key.colors[i]);desc.colorAttachments[i].writeMask=(key.mask&(1u<<i))?MTLColorWriteMaskAll:MTLColorWriteMaskNone;}
    desc.depthAttachmentPixelFormat=Pixel(key.depth);desc.stencilAttachmentPixelFormat=Pixel(key.stencil);desc.rasterSampleCount=key.samples;
    auto pipeline=[device newRenderPipelineStateWithDescriptor:desc error:&failure];if(!pipeline){error=DescribeError(failure);return nullptr;}
    auto depth=[MTLDepthStencilDescriptor new];depth.depthCompareFunction=MTLCompareFunctionAlways;depth.depthWriteEnabled=key.clear_depth;
    if(key.stencil!=Format::Invalid){auto s=[MTLStencilDescriptor new];s.stencilCompareFunction=MTLCompareFunctionAlways;s.stencilFailureOperation=MTLStencilOperationKeep;s.depthFailureOperation=MTLStencilOperationKeep;s.depthStencilPassOperation=key.clear_stencil?MTLStencilOperationReplace:MTLStencilOperationKeep;s.readMask=255;s.writeMask=key.clear_stencil?255:0;depth.frontFaceStencil=s;depth.backFaceStencil=s;}
    auto ds=[device newDepthStencilStateWithDescriptor:depth];if(!ds){Fail(error,"Rectangular clear depth state creation failed");return nullptr;}
    clears.Insert(key,ClearObject{pipeline,ds});clears.Trim(128);return clears.Find(key);
  }
  bool ClearCall(id<MTLRenderCommandEncoder> e,const RectClear& c,const Pass& pass,EncoderBindings& cache,std::shared_ptr<FrameLease> lease,std::string& error){
    if(!c.rectangle.width||!c.rectangle.height||(!c.colors&&!c.depth&&!c.stencil))return true;
    ClearKey key;key.samples=pass.samples;key.mask=c.colors;key.clear_depth=c.depth;key.clear_stencil=c.stencil;
    auto format=[&](const SurfaceView& v){return v.format==Format::Invalid?surfaces.at(v.surface).source.format:v.format;};
    for(size_t i=0;i<4;++i)if(pass.colors[i])key.colors[i]=format(pass.colors[i]->view);
    if(pass.depth)key.depth=format(pass.depth->view);if(pass.stencil)key.stencil=format(pass.stencil->view);
    auto* p=ClearPipeline(key,error);if(!p)return false;lease->gpu.push_back(p->pipeline);lease->gpu.push_back(p->depth);
    struct alignas(16) Constants {std::array<std::array<float,4>,4> color;float depth;};Constants constants{};
    for(size_t i=0;i<4;++i)for(size_t j=0;j<4;++j) {
      if(key.colors[i]==Format::RG16Uint||key.colors[i]==Format::RGBA16Uint||key.colors[i]==Format::R32Uint||key.colors[i]==Format::RG32Uint) {
        uint32_t bits=uint32_t(std::clamp(c.color[i][j],0.0,double(UINT32_MAX)));std::memcpy(&constants.color[i][j],&bits,4);
      } else constants.color[i][j]=float(c.color[i][j]);
    }
    constants.depth=float(c.depth_value);
    [e setRenderPipelineState:p->pipeline];[e setDepthStencilState:p->depth];
    [e setViewport:MTLViewport{0,0,double(pass.width),double(pass.height),0,1}];
    [e setScissorRect:MTLScissorRect{c.rectangle.x,c.rectangle.y,c.rectangle.width,c.rectangle.height}];
    [e setCullMode:MTLCullModeNone];[e setDepthClipMode:MTLDepthClipModeClip];[e setDepthBias:0 slopeScale:0 clamp:0];
    [e setStencilReferenceValue:c.stencil_value];[e setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];cache={};return true;
  }
  bool InlineUpload(id<MTLRenderCommandEncoder> encoder,const BufferUpload& upload,const Pass& pass,
      EncoderBindings& bindings,std::shared_ptr<FrameLease> lease,std::string& error){
    if(!upload_vertex) {
      auto options=[MTLCompileOptions new];NSError* failure=nil;
      auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:kUploadVertex] options:options error:&failure];
      if(!library){error=DescribeError(failure);return false;}upload_vertex=[library newFunctionWithName:@"upload_vertex"];
      if(!upload_vertex)return Fail(error,"Native in-pass upload vertex function is missing");
    }
    ClearKey key;key.samples=pass.samples;
    auto format=[&](const SurfaceView& view){return view.format==Format::Invalid?surfaces.at(view.surface).source.format:view.format;};
    for(size_t i=0;i<4;++i)if(pass.colors[i])key.colors[i]=format(pass.colors[i]->view);
    if(pass.depth)key.depth=format(pass.depth->view);if(pass.stencil)key.stencil=format(pass.stencil->view);
    auto* pipeline=upload_pipelines.Find(key);
    if(!pipeline) {
      auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=upload_vertex;desc.rasterizationEnabled=NO;desc.rasterSampleCount=pass.samples;
      for(size_t i=0;i<4;++i)desc.colorAttachments[i].pixelFormat=Pixel(key.colors[i]);
      desc.depthAttachmentPixelFormat=Pixel(key.depth);desc.stencilAttachmentPixelFormat=Pixel(key.stencil);
      NSError* failure=nil;auto created=[device newRenderPipelineStateWithDescriptor:desc error:&failure];
      if(!created){error=DescribeError(failure);return false;}
      pipeline=&upload_pipelines.Insert(key,created);upload_pipelines.Trim(128);
    }
    id<MTLBuffer> source=[device newBufferWithBytes:upload.bytes->data() length:upload.bytes->size() options:MTLResourceStorageModeShared];
    if(!source)return Fail(error,"Native in-pass upload staging allocation failed");
    // The group planner rejects any preceding fragment access to this buffer.
    // Only vertex-origin barriers are used, including on Apple3-Apple8 GPUs.
    id<MTLResource> destination=BufferAt(upload.destination);
    [encoder memoryBarrierWithResources:&destination count:1 afterStages:MTLRenderStageVertex beforeStages:MTLRenderStageVertex];
    [encoder setRenderPipelineState:*pipeline];[encoder setVertexBuffer:source offset:0 atIndex:0];
    [encoder setVertexBuffer:BufferAt(upload.destination) offset:upload.destination.offset atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:upload.bytes->size()/16];
    [encoder memoryBarrierWithResources:&destination count:1 afterStages:MTLRenderStageVertex beforeStages:MTLRenderStageVertex|MTLRenderStageFragment];
    lease->gpu.push_back(source);lease->gpu.push_back(*pipeline);bindings={};
    ++counters.inline_uploads;counters.inline_upload_bytes+=upload.bytes->size();return true;
  }
  bool Render(id<MTLCommandBuffer> cb,const Frame& frame,size_t begin,size_t end,std::shared_ptr<FrameLease> lease,std::string& error){
    const auto& pass=std::get<Pass>(frame.commands[begin]);const auto& last=std::get<Pass>(frame.commands[end-1]);
    Rect area=options.tight_render_area?RenderGroupArea(frame,begin,end):Rect{0,0,pass.width,pass.height};
    auto desc=[MTLRenderPassDescriptor new];desc.renderTargetWidth=area.width;desc.renderTargetHeight=area.height;
    desc.defaultRasterSampleCount=pass.samples;
    for(size_t i=0;i<4;++i)if(pass.colors[i]){
      auto attachment=*pass.colors[i];attachment.store=last.colors[i]->store;
      auto a=desc.colorAttachments[i];Attachment(a,attachment,error);if(!a.texture)return false;
      const auto& c=pass.colors[i]->color;a.clearColor=MTLClearColorMake(c[0],c[1],c[2],c[3]);
    }
    if(pass.depth){auto a=*pass.depth;a.store=last.depth->store;Attachment(desc.depthAttachment,a,error);if(!desc.depthAttachment.texture)return false;desc.depthAttachment.clearDepth=pass.depth->depth;}
    if(pass.stencil){auto a=*pass.stencil;a.store=last.stencil->store;Attachment(desc.stencilAttachment,a,error);if(!desc.stencilAttachment.texture)return false;desc.stencilAttachment.clearStencil=pass.stencil->stencil;}
    id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:desc];if(!e)return Fail(error,"Native render encoder creation failed");
    EncoderBindings bindings;bool success=true;
    for(size_t i=begin;i<end&&success;++i) {
      if(const auto* upload=std::get_if<BufferUpload>(&frame.commands[i]))success=InlineUpload(e,*upload,pass,bindings,lease,error);
      else for(const auto& command:std::get<Pass>(frame.commands[i]).commands){
        if(const auto* draw=std::get_if<Draw>(&command))success=DrawCall(e,*draw,bindings,lease,error);
        else success=ClearCall(e,std::get<RectClear>(command),pass,bindings,lease,error);
        if(!success)break;
      }
    }
    [e endEncoding];++counters.passes;counters.render_area_pixels+=uint64_t(pass.width)*pass.height;
    counters.render_area_used_pixels+=uint64_t(area.width)*area.height;
    if(area.width!=pass.width||area.height!=pass.height)++counters.reduced_passes;return success;
  }
  bool Compute(id<MTLCommandBuffer> cb,const Dispatch& d,const Frame& frame,std::shared_ptr<FrameLease> lease,std::string& error){
    const Shader& shader=ShaderAt(frame,d.shader);auto function=Function(shader,lease,error);if(!function)return false;
    auto* cached=computes.Find(shader.id);
    if(cached&&cached->source!=shader)return Fail(error,"Compute shader identity changed without a generation change");
    if(!cached){
      NSError* failure=nil;MTLComputePipelineReflection* reflection=nil;
      auto p=[device newComputePipelineStateWithFunction:function options:MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&failure];
      if(!p){error=DescribeError(failure);return false;}
      ComputeObject object;object.source=shader;object.pipeline=p;
      if(!Reflect(shader,reflection.arguments,object.arguments,error))return false;
      computes.Insert(shader.id,std::move(object));computes.Trim(options.maximum_pipelines);cached=computes.Find(shader.id);
    }
    if(uint64_t(d.threads[0])*d.threads[1]*d.threads[2]>cached->pipeline.maxTotalThreadsPerThreadgroup)return Fail(error,"Compute threadgroup exceeds the compiled pipeline limit");
    if(!ActualBuffers(d.bindings,cached->arguments,error))return false;
    lease->gpu.push_back(cached->pipeline);id<MTLComputeCommandEncoder> e=[cb computeCommandEncoder];if(!e)return Fail(error,"Native compute encoder creation failed");
    [e setComputePipelineState:cached->pipeline];
    for(const auto& b:d.bindings.buffers)[e setBuffer:BufferAt(b.view) offset:b.view.offset atIndex:b.index];
    for(const auto& t:d.bindings.textures){auto texture=Texture(t.view,true,error);if(!texture){[e endEncoding];return false;}lease->gpu.push_back(texture);[e setTexture:texture atIndex:t.index];}
    for(const auto& s:d.bindings.samplers)[e setSamplerState:active_samplers.at(s.sampler).sampler atIndex:s.index];
    [e dispatchThreadgroups:MTLSizeMake(d.groups[0],d.groups[1],d.groups[2]) threadsPerThreadgroup:MTLSizeMake(d.threads[0],d.threads[1],d.threads[2])];[e endEncoding];return true;
  }
  bool Transfer(id<MTLCommandBuffer> cb,const Command& command,std::shared_ptr<FrameLease> lease,std::string& error){
    id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];if(!blit)return Fail(error,"Native blit encoder creation failed");
    bool success=true;
    if(const auto* upload=std::get_if<BufferUpload>(&command)){
      id<MTLBuffer> source=[device newBufferWithBytes:upload->bytes->data() length:upload->bytes->size() options:MTLResourceStorageModeShared];
      if(!source)success=Fail(error,"GPU buffer upload staging failed");
      else {lease->gpu.push_back(source);[blit copyFromBuffer:source sourceOffset:0 toBuffer:BufferAt(upload->destination) destinationOffset:upload->destination.offset size:upload->bytes->size()];}
    }else if(const auto* copy=std::get_if<BufferCopy>(&command)){
      [blit copyFromBuffer:BufferAt(copy->source) sourceOffset:copy->source.offset toBuffer:BufferAt(copy->destination)
          destinationOffset:copy->destination.offset size:copy->size];++counters.copies;
    }else if(const auto* copy=std::get_if<BufferToTexture>(&command)){
      auto source=BufferAt(copy->source);auto destination=Texture(copy->destination,false,error);
      if(!source||!destination)success=Fail(error,"Native GPU texture copy resources are absent");
      else {
        [blit copyFromBuffer:source sourceOffset:copy->source.offset sourceBytesPerRow:copy->bytes_per_row
            sourceBytesPerImage:copy->depth>1?copy->bytes_per_image:0 sourceSize:MTLSizeMake(copy->width,copy->height,copy->depth)
            toTexture:destination destinationSlice:copy->destination.slice destinationLevel:copy->destination.level
            destinationOrigin:MTLOriginMake(copy->destination_x,copy->destination_y,copy->destination.plane)];
        ++counters.copies;
      }
    }else if(const auto* upload=std::get_if<TextureUpload>(&command)){
      const Surface& s=surfaces.at(upload->destination.surface).source;const uint64_t width=std::max(1u,s.width>>upload->destination.level),height=std::max(1u,s.height>>upload->destination.level);
      const auto f=Describe(s.format);const uint64_t rows=(height+f.block_height-1)/f.block_height;
      const uint64_t stride=(upload->bytes_per_row+255)&~uint64_t(255);
      id<MTLBuffer> source=[device newBufferWithLength:stride*rows options:MTLResourceStorageModeShared];
      auto texture=Texture(upload->destination,false,error);
      if(!source||!texture)success=Fail(error,"Native texture upload staging failed");
      else {
        for(uint64_t row=0;row<rows;++row)std::memcpy(static_cast<std::byte*>(source.contents)+stride*row,upload->bytes->data()+upload->bytes_per_row*row,upload->bytes_per_row);
        lease->gpu.push_back(source);[blit copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:stride sourceBytesPerImage:stride*rows sourceSize:MTLSizeMake(width,height,1) toTexture:texture destinationSlice:upload->destination.slice destinationLevel:upload->destination.level destinationOrigin:MTLOriginMake(0,0,upload->destination.plane)];
      }
    } else {
      const auto& copy=std::get<ImageCopy>(command);auto source=Texture(copy.source,false,error),destination=Texture(copy.destination,false,error);
      if(!source||!destination)success=false;
      else {[blit copyFromTexture:source sourceSlice:copy.source.slice sourceLevel:copy.source.level sourceOrigin:MTLOriginMake(copy.source_x,copy.source_y,copy.source.plane) sourceSize:MTLSizeMake(copy.width,copy.height,1) toTexture:destination destinationSlice:copy.destination.slice destinationLevel:copy.destination.level destinationOrigin:MTLOriginMake(copy.destination_x,copy.destination_y,copy.destination.plane)];++counters.copies;}
    }
    [blit endEncoding];return success;
  }
  bool ResolveImage(id<MTLCommandBuffer> cb,const Resolve& r,std::string& error){
    auto source=Texture(r.source,false,error),destination=Texture(r.destination,false,error);if(!source||!destination)return false;
    const auto format=Describe(surfaces.at(r.source.surface).source.format);auto desc=[MTLRenderPassDescriptor new];
    MTLRenderPassAttachmentDescriptor* attachment=format.color?desc.colorAttachments[0]:desc.depthAttachment;
    attachment.texture=source;attachment.level=r.source.level;attachment.slice=r.source.slice;attachment.depthPlane=r.source.plane;
    attachment.resolveTexture=destination;attachment.resolveLevel=r.destination.level;attachment.resolveSlice=r.destination.slice;attachment.resolveDepthPlane=r.destination.plane;
    attachment.loadAction=MTLLoadActionLoad;attachment.storeAction=MTLStoreActionStoreAndMultisampleResolve;
    if(format.depth)desc.depthAttachment.depthResolveFilter=r.filter==ResolveFilter::Min?MTLMultisampleDepthResolveFilterMin:r.filter==ResolveFilter::Max?MTLMultisampleDepthResolveFilterMax:MTLMultisampleDepthResolveFilterSample0;
    if(format.stencil){
      auto stencil=desc.stencilAttachment;stencil.texture=source;stencil.level=r.source.level;stencil.slice=r.source.slice;stencil.depthPlane=r.source.plane;
      stencil.resolveTexture=destination;stencil.resolveLevel=r.destination.level;stencil.resolveSlice=r.destination.slice;stencil.resolveDepthPlane=r.destination.plane;
      stencil.loadAction=MTLLoadActionLoad;stencil.storeAction=MTLStoreActionStoreAndMultisampleResolve;
      stencil.stencilResolveFilter=r.filter==ResolveFilter::SampleZero?MTLMultisampleStencilResolveFilterSample0:MTLMultisampleStencilResolveFilterDepthResolvedSample;
    }
    id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:desc];if(!e)return Fail(error,"Native resolve encoder creation failed");[e endEncoding];++counters.resolves;return true;
  }
  bool Present(id<MTLCommandBuffer> cb,const Frame& frame,std::shared_ptr<FrameLease> lease,std::string& error){
    if(!layer||!frame.output)return true;
    id<CAMetalDrawable> drawable=[layer nextDrawable];
    // UIKit may stop supplying drawables while inactive. Do not substitute a
    // CPU readback or destroy the offscreen frame contents in that case.
    if(!drawable)return true;
    if(!present_pipeline||present_format!=drawable.texture.pixelFormat){
      NSString* source=@"#include <metal_stdlib>\nusing namespace metal;struct V{float4 p[[position]];float2 uv;};vertex V present_vs(uint i[[vertex_id]]){float2 p=float2((i<<1)&2,i&2);return V{float4(p*2-1,0,1),float2(p.x,1-p.y)};}fragment float4 present_ps(V v[[stage_in]],texture2d<float> image[[texture(0)]]){constexpr sampler s(filter::linear,address::clamp_to_edge);return image.sample(s,v.uv);}";
      auto options=[MTLCompileOptions new];options.fastMathEnabled=NO;NSError* failure=nil;
      auto library=[device newLibraryWithSource:source options:options error:&failure];if(!library){error=DescribeError(failure);return false;}
      auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=[library newFunctionWithName:@"present_vs"];desc.fragmentFunction=[library newFunctionWithName:@"present_ps"];desc.colorAttachments[0].pixelFormat=drawable.texture.pixelFormat;
      present_pipeline=[device newRenderPipelineStateWithDescriptor:desc error:&failure];if(!present_pipeline){error=DescribeError(failure);return false;}present_format=drawable.texture.pixelFormat;
    }
    auto texture=Texture(*frame.output,true,error);if(!texture)return false;
    auto desc=[MTLRenderPassDescriptor new];desc.colorAttachments[0].texture=drawable.texture;desc.colorAttachments[0].loadAction=MTLLoadActionClear;desc.colorAttachments[0].storeAction=MTLStoreActionStore;desc.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
    auto encoder=[cb renderCommandEncoderWithDescriptor:desc];if(!encoder)return Fail(error,"Native presentation encoder creation failed");
    double w=drawable.texture.width,h=drawable.texture.height,x=0,y=0;
    if(options.letterbox){double scale=std::min(w/texture.width,h/texture.height);w=texture.width*scale;h=texture.height*scale;x=(drawable.texture.width-w)*.5;y=(drawable.texture.height-h)*.5;}
    [encoder setRenderPipelineState:present_pipeline];[encoder setViewport:MTLViewport{x,y,w,h,0,1}];[encoder setFragmentTexture:texture atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];
    [cb presentDrawable:drawable];lease->gpu.push_back(drawable);lease->gpu.push_back(texture);lease->gpu.push_back(present_pipeline);return true;
  }
};

MetalBackend::MetalBackend(const BackendOptions& options):impl_(std::make_unique<Impl>(options)){}
MetalBackend::~MetalBackend(){std::string ignored;Drain(std::chrono::seconds(30),ignored);}
bool MetalBackend::Ready(std::string& error) const {
  error.clear();if(!impl_->startup_error.empty()){error=impl_->startup_error;return false;}
  std::lock_guard lock(impl_->state->mutex);if(impl_->state->lost){error=impl_->state->loss;return false;}return true;
}
bool MetalBackend::Submit(std::shared_ptr<const Frame> frame,std::function<void(Completion)> completion,std::string& error){@autoreleasepool {
  if(!Ready(error))return false;
  if(!frame||!Validate(*frame,error))return false;
  if(frame->sequence<=impl_->last_sequence)return Fail(error,"Native frame sequence did not advance");
  auto state=impl_->state;
  {
    std::unique_lock lock(state->mutex);
    if(!state->changed.wait_for(lock,std::chrono::seconds(5),[&]{return state->pending<impl_->options.maximum_frames||state->lost;}))return Fail(error,"Native frame slots did not retire within five seconds");
    if(state->lost){error=state->loss;return false;}
    ++state->pending;
  }
  auto lease=std::make_shared<FrameLease>();lease->frame=frame;lease->state=state;
  if(!impl_->Prepare(*frame,lease,error))return false;
  id<MTLCommandBuffer> command=[impl_->queue commandBuffer];if(!command)return Fail(error,"Native command buffer creation failed");
  for(size_t index=0;index<frame->commands.size();++index){
    const auto& operation=frame->commands[index];
    bool success;
    if(std::holds_alternative<Pass>(operation)) {
      const size_t end=impl_->options.inline_buffer_uploads&&([impl_->device supportsFamily:MTLGPUFamilyApple3]||[impl_->device supportsFamily:MTLGPUFamilyMac2])?RenderGroupEnd(*frame,index):index+1;
      success=impl_->Render(command,*frame,index,end,lease,error);index=end-1;
    }
    else if(const auto* d=std::get_if<Dispatch>(&operation))success=impl_->Compute(command,*d,*frame,lease,error);
    else if(const auto* r=std::get_if<Resolve>(&operation))success=impl_->ResolveImage(command,*r,error);
    else success=impl_->Transfer(command,operation,lease,error);
    if(!success)return false;
  }
  if(!impl_->Present(command,*frame,lease,error))return false;
  const uint64_t sequence=frame->sequence;const bool timing=impl_->options.timing;
  [command addCompletedHandler:^(id<MTLCommandBuffer> buffer){
    Completion result;result.sequence=sequence;result.success=buffer.status==MTLCommandBufferStatusCompleted;
    if(!result.success)result.error=DescribeError(buffer.error);
    if(timing&&result.success){const auto a=buffer.GPUStartTime,b=buffer.GPUEndTime;
      if(a>0&&b>=a){result.timing_available=true;result.native_window_ms=(b-a)*1000;}}
    {
      std::lock_guard lock(state->mutex);++state->stats.completed;
      if(!result.success){state->lost=true;state->loss=result.error.empty()?"Native Metal command buffer failed":result.error;}
    }
    lease->Retire();
    if(completion)try{completion(std::move(result));}catch(...){NSLog(@"Native completion consumer threw an exception");}
  }];
  impl_->last_sequence=sequence;++impl_->counters.submitted;impl_->Snapshot();[command commit];return true;
}}
bool MetalBackend::Drain(std::chrono::milliseconds timeout,std::string& error){
  auto state=impl_->state;std::unique_lock lock(state->mutex);
  if(!state->changed.wait_for(lock,timeout,[&]{return state->pending==0;}))return Fail(error,"Native GPU work did not drain before the timeout");
  if(state->lost){error=state->loss;return false;}error.clear();return true;
}
BackendStats MetalBackend::Stats() const {std::lock_guard lock(impl_->state->mutex);return impl_->state->stats;}
bool MetalBackend::AttachLayer(void* opaque,std::string& error){
  if(!Ready(error))return false;
  {std::lock_guard lock(impl_->state->mutex);if(impl_->state->pending)return Fail(error,"Drain before changing the presentation layer");}
  CAMetalLayer* layer=(__bridge CAMetalLayer*)opaque;
  if(layer&&layer.device&&layer.device!=impl_->device)return Fail(error,"Presentation layer and native renderer use different Metal devices");
  if(layer){layer.device=impl_->device;layer.framebufferOnly=YES;}
  impl_->layer=layer;return true;
}
bool MetalBackend::ConfigureBinaryArchive(const std::string& path,std::string& error){
  if(!Ready(error))return false;
  {std::lock_guard lock(impl_->state->mutex);if(impl_->state->pending)return Fail(error,"Drain before configuring the native pipeline archive");}
  NSURL* url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  auto desc=[MTLBinaryArchiveDescriptor new];if([NSFileManager.defaultManager fileExistsAtPath:url.path])desc.url=url;
  NSError* failure=nil;auto archive=[impl_->device newBinaryArchiveWithDescriptor:desc error:&failure];
  if(!archive&&desc.url){desc.url=nil;failure=nil;archive=[impl_->device newBinaryArchiveWithDescriptor:desc error:&failure];}
  if(!archive){error=DescribeError(failure);return false;}
  impl_->archive=archive;impl_->archive_url=url;impl_->archive_dirty=false;return true;
}
bool MetalBackend::SaveBinaryArchive(std::string& error){
  if(!Drain(std::chrono::seconds(30),error))return false;
  if(!impl_->archive||!impl_->archive_dirty)return true;
  NSError* failure=nil;
  if(![NSFileManager.defaultManager createDirectoryAtURL:[impl_->archive_url URLByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:&failure]||
     ![impl_->archive serializeToURL:impl_->archive_url error:&failure]){error=DescribeError(failure);return false;}
  impl_->archive_dirty=false;return true;
}
void MetalBackend::SetTimingEnabled(bool enabled) {impl_->options.timing=enabled;}
bool MetalBackend::QualifyUploadBatching(std::string& detail) {@autoreleasepool {
  impl_->options.inline_buffer_uploads=false;
  // Hosted paravirtual GPUs advertise families but reject render barriers.
  // Keep them, and older/unqualified hardware, on the conventional path.
  if(![impl_->device supportsFamily:MTLGPUFamilyApple9]||
     [impl_->device.name rangeOfString:@"paravirtual" options:NSCaseInsensitiveSearch].location!=NSNotFound) {
    detail="unavailable on this GPU; conventional uploads retained";return false;
  }
  @try {
    for(auto test:std::array<std::pair<bool,uint32_t>,3>{{{false,1},{false,4},{true,1}}}) {
      BackendOptions options;options.inline_buffer_uploads=true;options.surface_budget=1ull<<20;options.maximum_frames=1;
      MetalBackend probe(options);auto frame=std::make_shared<const Frame>(UploadProbe(test.first,test.second));
      if(!probe.Submit(frame,{},detail)||!probe.Drain(std::chrono::seconds(5),detail))return false;
      auto pixels=probe.ReadRGBA8(*frame->output,detail);
      if(!CheckUploadProbe(pixels,test.first)||probe.Stats().inline_uploads!=1) {
        detail="pixel ordering check failed; conventional uploads retained";return false;
      }
    }
  } @catch(NSException* exception) {detail=exception.reason.UTF8String;return false;}
  impl_->options.inline_buffer_uploads=true;detail="passed vertex, index and 4x MSAA pixel ordering checks";return true;
}}
Capabilities MetalBackend::GetDeviceCapabilities() const {
  Capabilities c;c.max_buffer_length=impl_->device.maxBufferLength;
  c.float32_filtering=impl_->device.supports32BitFloatFiltering;
  c.bc_texture_compression=impl_->device.supportsBCTextureCompression;
#if TARGET_OS_IPHONE
  c.packed_16bit_textures=true;
#endif
  return c;
}
std::vector<std::byte> MetalBackend::ReadBuffer(const BufferView& view,std::string& error){@autoreleasepool {
  if(!Ready(error))return {};
  {std::lock_guard lock(impl_->state->mutex);if(impl_->state->pending) {Fail(error,"Buffer readback requires a drained native queue");return {};}}
  auto i=impl_->storage.find(view.buffer);
  if(i==impl_->storage.end() || !view.length || view.offset>i->second.size || view.length>i->second.size-view.offset || view.length>64ull*1024*1024) {
    Fail(error,"Invalid GPU buffer readback range");return {};
  }
  auto staging=[impl_->device newBufferWithLength:view.length options:MTLResourceStorageModeShared];
  auto command=[impl_->queue commandBuffer];if(!staging||!command) {Fail(error,"Buffer readback staging failed");return {};}
  auto blit=[command blitCommandEncoder];if(!blit) {Fail(error,"Buffer readback encoder failed");return {};}
  [blit copyFromBuffer:i->second.buffer sourceOffset:view.offset toBuffer:staging destinationOffset:0 size:view.length];[blit endEncoding];
  dispatch_semaphore_t done=dispatch_semaphore_create(0);[command addCompletedHandler:^(id<MTLCommandBuffer>){dispatch_semaphore_signal(done);}];[command commit];
  if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC))||command.status!=MTLCommandBufferStatusCompleted) {
    error=command.error?DescribeError(command.error):"Buffer readback did not complete";return {};
  }
  std::vector<std::byte> result(view.length);std::memcpy(result.data(),staging.contents,view.length);return result;
}}
std::vector<std::byte> MetalBackend::ReadRGBA8(const SurfaceView& view,std::string& error){@autoreleasepool {
  if(!Ready(error))return {};
  {std::lock_guard lock(impl_->state->mutex);if(impl_->state->pending){Fail(error,"Readback requires a drained native queue");return {};}}
  auto found=impl_->surfaces.find(view.surface);if(found==impl_->surfaces.end()){Fail(error,"Readback surface is absent");return {};}
  const Surface& s=found->second.source;Format format=view.format==Format::Invalid?s.format:view.format;
  if((format!=Format::RGBA8&&format!=Format::RGBA8Srgb&&format!=Format::BGRA8&&format!=Format::BGRA8Srgb)||s.samples!=1||view.level>=s.levels||view.slice>=s.layers||view.plane>=(s.kind==TextureKind::ThreeD?std::max(1u,s.depth>>view.level):1u)){
    Fail(error,"Readback requires a valid single-sample RGBA8/BGRA8 subresource");return {};}
  auto texture=impl_->Texture(view,false,error);if(!texture)return {};
  const uint64_t width=std::max(1u,s.width>>view.level),height=std::max(1u,s.height>>view.level),stride=(width*4+255)&~uint64_t(255);
  if(stride*height>64ull*1024*1024){Fail(error,"Validation readback exceeds 64 MB");return {};}
  auto buffer=[impl_->device newBufferWithLength:stride*height options:MTLResourceStorageModeShared];
  auto command=[impl_->queue commandBuffer];if(!buffer||!command){Fail(error,"Readback staging creation failed");return {};}
  auto blit=[command blitCommandEncoder];if(!blit){Fail(error,"Readback encoder creation failed");return {};}
  [blit copyFromTexture:texture sourceSlice:view.slice sourceLevel:view.level sourceOrigin:MTLOriginMake(0,0,view.plane) sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride*height];[blit endEncoding];
  dispatch_semaphore_t done=dispatch_semaphore_create(0);[command addCompletedHandler:^(id<MTLCommandBuffer>){dispatch_semaphore_signal(done);}];[command commit];
  if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC))||command.status!=MTLCommandBufferStatusCompleted){error=command.error?DescribeError(command.error):"Readback did not complete within thirty seconds";return {};}
  std::vector<std::byte> result(width*height*4);
  for(uint64_t y=0;y<height;++y)std::memcpy(result.data()+y*width*4,static_cast<const std::byte*>(buffer.contents)+y*stride,width*4);
  if(format==Format::BGRA8||format==Format::BGRA8Srgb)for(size_t i=0;i<result.size();i+=4)std::swap(result[i],result[i+2]);
  return result;
}}
} // namespace rex::native
