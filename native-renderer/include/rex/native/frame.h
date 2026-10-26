// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace rex::native {
// These are host rendering contracts. Guest addresses, formats and shader-bank
// offsets must be lowered by the title frontend; they are not resource IDs.
struct Identity {
  uint64_t object=0,generation=0;
  auto operator<=>(const Identity&) const=default;
  explicit operator bool() const {return object!=0 && generation!=0;}
};
using Bytes=std::shared_ptr<const std::vector<std::byte>>;
enum class Format:uint8_t {
  Invalid,R8,RG8,RGBA8,RGBA8Srgb,BGRA8,BGRA8Srgb,RGB10A2,RG16Float,
  RGBA16Float,R32Float,RG32Float,RGBA32Float,Depth32Float,Depth32Stencil8,
  BC1,BC1Srgb,BC2,BC2Srgb,BC3,BC3Srgb,BC4,BC4Signed,BC5,BC5Signed,
  BC6Float,BC6SignedFloat,BC7,BC7Srgb,ASTC4x4,ASTC4x4Srgb,RG16Snorm,RGBA16Snorm,
  RG16Uint,RGBA16Uint,R32Uint,RG32Uint,Stencil8View,
  R8Snorm,RG8Snorm,RGBA8Snorm,R16Float,R16Unorm,R16Snorm,RG16Unorm,RGBA16Unorm,
  BGR565,BGR5A1,ABGR4,Count
};
struct FormatInfo {uint8_t block_width=1,block_height=1,bytes=0;bool color=false,depth=false,stencil=false;};
FormatInfo Describe(Format);
bool CompatibleView(Format storage,Format view);
enum class TextureKind:uint8_t {TwoD,TwoDArray,Cube,CubeArray,ThreeD,Count};
struct Surface {
  Identity id;
  Format format=Format::Invalid;
  TextureKind kind=TextureKind::TwoD;
  uint32_t width=0,height=0,depth=1,layers=1,levels=1,samples=1;
  // Renderable and sampled are explicit. Compressed textures cannot be targets.
  bool renderable=false,sampled=true,storage=false;
  // Retain this owner in the frontend registry while the GPU contents can be
  // read by later frames. Frames retain it through GPU completion. The backend
  // keeps only a weak owner, so retirement never needs an unbounded global scan.
  std::shared_ptr<const void> owner;
  auto operator<=>(const Surface&) const=default;
};
struct SurfaceView {
  Identity surface;
  uint32_t level=0,slice=0,plane=0;
  Format format=Format::Invalid; // Invalid means the storage format.
  auto operator<=>(const SurfaceView&) const=default;
};
struct Buffer {
  Identity id;
  Bytes bytes;
  bool constants=false;
  // The immutable owner protects pending frames from mutation/eviction. A new
  // allocation or new bytes require a new generation, even at the same address.
};
struct BufferView {
  Identity buffer;
  uint64_t offset=0,length=0;
  auto operator<=>(const BufferView&) const=default;
};
struct BufferStorage {
  Identity id;
  uint64_t size=0;
  bool writable=false;
  std::shared_ptr<const void> owner;
};
enum class Stage:uint8_t {Vertex,Fragment,Compute,Count};
enum class ArgumentKind:uint8_t {Buffer,Texture,Sampler,Count};
struct Argument {
  ArgumentKind kind=ArgumentKind::Buffer;
  uint32_t index=0,count=1;
  uint64_t minimum_bytes=0,alignment=1;
  TextureKind texture_kind=TextureKind::TwoD;
  bool multisampled=false;
  bool writable=false;
  auto operator<=>(const Argument&) const=default;
};
struct Shader {
  Identity id;
  Stage stage=Stage::Vertex;
  std::string entry;
  // Exactly one representation. Device-library bytes are produced by the
  // offline exporter; MSL supports on-device preparation without a CPU JIT.
  Bytes library;
  std::string msl;
  std::vector<Argument> arguments;
  enum class ConstantType:uint8_t {Bool,UInt,Int,Float,Count};
  struct Constant {
    uint32_t index=0,bits=0;ConstantType type=ConstantType::UInt;
    auto operator<=>(const Constant&) const=default;
  };
  std::vector<Constant> constants;
  auto operator<=>(const Shader&) const=default;
};
enum class Compare:uint8_t {Never,Less,Equal,LessEqual,Greater,NotEqual,GreaterEqual,Always,Count};
enum class BlendFactor:uint8_t {Zero,One,SourceColor,OneMinusSourceColor,SourceAlpha,OneMinusSourceAlpha,DestinationColor,OneMinusDestinationColor,DestinationAlpha,OneMinusDestinationAlpha,ConstantColor,OneMinusConstantColor,ConstantAlpha,OneMinusConstantAlpha,SourceAlphaSaturated,Count};
enum class BlendOperation:uint8_t {Add,Subtract,ReverseSubtract,Min,Max,Count};
struct Blend {
  bool enabled=false;
  uint8_t write_mask=15;
  BlendFactor source_rgb=BlendFactor::One,destination_rgb=BlendFactor::Zero;
  BlendFactor source_alpha=BlendFactor::One,destination_alpha=BlendFactor::Zero;
  BlendOperation rgb=BlendOperation::Add,alpha=BlendOperation::Add;
  auto operator<=>(const Blend&) const=default;
};
enum class StencilOperation:uint8_t {Keep,Zero,Replace,IncrementClamp,DecrementClamp,Invert,IncrementWrap,DecrementWrap,Count};
struct Stencil {
  Compare compare=Compare::Always;
  StencilOperation fail=StencilOperation::Keep,depth_fail=StencilOperation::Keep,pass=StencilOperation::Keep;
  uint32_t read_mask=255,write_mask=255;
  auto operator<=>(const Stencil&) const=default;
};
struct Pipeline {
  Identity id,vertex,fragment;
  std::array<Format,4> colors{};
  Format depth=Format::Invalid,stencil=Format::Invalid;
  uint32_t samples=1;
  std::array<Blend,4> blend{};
  Compare depth_compare=Compare::Always;
  bool depth_write=false,stencil_enabled=false,alpha_to_coverage=false;
  Stencil front{},back{};
  auto operator<=>(const Pipeline&) const=default;
};
enum class Filter:uint8_t {Nearest,Linear,Count};
enum class Address:uint8_t {Clamp,Repeat,Mirror,ClampZero,MirrorClamp,ClampBorder,Count};
enum class Border:uint8_t {TransparentBlack,OpaqueBlack,OpaqueWhite,Count};
struct Sampler {
  Identity id;
  Filter min=Filter::Nearest,mag=Filter::Nearest,mip=Filter::Nearest;
  Address u=Address::Clamp,v=Address::Clamp,w=Address::Clamp;
  float min_lod=0,max_lod=1000;
  uint32_t anisotropy=1;
  Border border=Border::TransparentBlack;
  auto operator<=>(const Sampler&) const=default;
};
struct BufferBinding {uint32_t index=0;BufferView view;};
struct TextureBinding {uint32_t index=0;SurfaceView view;};
struct SamplerBinding {uint32_t index=0;Identity sampler;};
struct Bindings {
  std::vector<BufferBinding> buffers;
  std::vector<TextureBinding> textures;
  std::vector<SamplerBinding> samplers;
};
struct Rect {
  uint32_t x=0,y=0,width=0,height=0;
  auto operator<=>(const Rect&) const=default;
};
struct Viewport {double x=0,y=0,width=0,height=0,near_z=0,far_z=1;};
enum class Primitive:uint8_t {Points,Lines,LineStrip,Triangles,TriangleStrip,Count};
enum class Cull:uint8_t {None,Front,Back,Count};
struct Draw {
  Identity pipeline;
  std::array<Bindings,2> bindings;
  Primitive primitive=Primitive::Triangles;
  uint32_t first_vertex=0,vertices=0,instances=1;
  std::optional<BufferView> indices;
  uint32_t index_count=0,index_bytes=2;
  int32_t base_vertex=0;
  Viewport viewport;
  Rect scissor;
  Cull cull=Cull::None;
  bool clockwise=false,depth_clamp=false;
  uint32_t stencil_front=0,stencil_back=0;
  std::array<float,4> blend_color{};
  float depth_bias=0,slope_bias=0;
};
struct RectClear {
  Rect rectangle;
  uint8_t colors=0;
  bool depth=false,stencil=false;
  std::array<std::array<double,4>,4> color{};
  double depth_value=1;
  uint32_t stencil_value=0;
};
enum class Load:uint8_t {Discard,Load,Clear,Count};
enum class Store:uint8_t {Discard,Store,Count};
struct Attachment {
  SurfaceView view;
  Load load=Load::Load;
  Store store=Store::Store;
  std::array<double,4> color{};
  double depth=1;
  uint32_t stencil=0;
  auto operator<=>(const Attachment&) const=default;
};
using PassCommand=std::variant<Draw,RectClear>;
struct Pass {
  uint32_t width=0,height=0,samples=1;
  std::array<std::optional<Attachment>,4> colors;
  std::optional<Attachment> depth,stencil;
  std::vector<PassCommand> commands;
  bool mergeable=true; // False when shader writes require an encoder boundary.
};
struct ImageCopy {
  SurfaceView source,destination;
  uint32_t source_x=0,source_y=0,destination_x=0,destination_y=0,width=0,height=0;
};
enum class ResolveFilter:uint8_t {Average,SampleZero,Min,Max,Count};
struct Resolve {SurfaceView source,destination;ResolveFilter filter=ResolveFilter::Average;};
struct TextureUpload {
  SurfaceView destination;
  Bytes bytes;
  uint64_t bytes_per_row=0,bytes_per_image=0;
};
struct BufferUpload {BufferView destination;Bytes bytes;};
struct BufferCopy {BufferView source,destination;uint64_t size=0;};
// A GPU-produced linear image becomes a texture without a CPU readback.
// Pitches include packed-mip offsets. Depth/stencil requires explicit lowering.
struct BufferToTexture {
  BufferView source;
  SurfaceView destination;
  uint64_t bytes_per_row=0,bytes_per_image=0;
  uint32_t destination_x=0,destination_y=0,width=0,height=0,depth=1;
};
struct Dispatch {
  Identity shader;
  Bindings bindings;
  std::array<uint32_t,3> groups{1,1,1},threads{1,1,1};
};
// Image/compute hazards cross explicit encoder boundaries. An aligned CPU
// buffer upload may use an in-pass vertex copy if the planner proves that no
// preceding fragment stage accesses its destination. Other writes stay split.
using Command=std::variant<TextureUpload,BufferUpload,BufferCopy,BufferToTexture,Dispatch,Pass,ImageCopy,Resolve>;
struct Frame {
  uint64_t sequence=0;
  std::vector<Surface> surfaces;
  std::vector<Buffer> buffers;
  std::vector<BufferStorage> buffer_storage;
  std::vector<Shader> shaders;
  std::vector<Pipeline> pipelines;
  std::vector<Sampler> samplers;
  std::vector<Command> commands;
  // Final display-encoded values. Guest gamma/tonemapping belong to explicit
  // frontend passes. An sRGB allocation can provide an unorm output view to
  // avoid applying a second transfer function during presentation.
  std::optional<SurfaceView> output;
};
struct Limits {
  uint32_t max_dimension=16384,max_texture_slots=128,max_sampler_slots=16,max_buffer_slots=31;
  uint32_t max_passes=4096,max_draws=32768;
  uint64_t max_buffer_bytes=512ull*1024*1024,max_upload_bytes=512ull*1024*1024;
};
// Admission is separate from API allocation. It validates the complete ordered
// packet; it does not claim that a title frontend supplied the correct contents.
bool Validate(const Frame&,std::string& error,const Limits& limits={});
// Joins adjacent conventional passes only when removing the store/load boundary
// preserves every attachment. Copies, resolves and partial clears stay ordered.
void AppendPass(Frame&,Pass);
// Extends a render group across aligned CPU uploads only when no preceding
// fragment stage accesses their destination. This needs vertex-to-vertex and
// vertex-to-fragment barriers, which Apple GPUs support. Other hazards split.
size_t RenderGroupEnd(const Frame&,size_t begin);
// An origin-anchored render area contains all scissors/partial clears. Full
// attachment clears and discarded stores preserve the original full extent.
Rect RenderGroupArea(const Frame&,size_t begin,size_t end);
} // namespace rex::native
