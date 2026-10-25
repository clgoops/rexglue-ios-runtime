// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "frame.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace rex::native {
struct BackendOptions {
  uint32_t maximum_frames=2;
  uint64_t surface_budget=512ull*1024*1024;
  uint64_t geometry_budget=256ull*1024*1024;
  uint64_t mutable_buffer_budget=512ull*1024*1024;
  uint64_t constant_budget=16ull*1024*1024;
  uint32_t maximum_pipelines=2048,maximum_shaders=1024,maximum_samplers=256;
  // Timing is opt-in. Normal rendering does not create counters or read clocks.
  bool timing=false;
  bool letterbox=true;
};
struct Completion {
  uint64_t sequence=0;
  bool success=false,timing_available=false;
  double native_window_ms=0;
  std::string error;
};
struct BackendStats {
  uint64_t submitted=0,completed=0,draws=0,passes=0,resolves=0,copies=0;
  uint64_t buffer_uploads=0,buffer_reuses=0,constant_pool_reuses=0;
  uint64_t pipeline_compiles=0,pipeline_hits=0,avoided_bindings=0;
  uint64_t surface_bytes=0,geometry_bytes=0,constant_bytes=0;
};
struct Capabilities {uint64_t max_buffer_length=0;bool float32_filtering=false,packed_16bit_textures=false;};
class MetalBackend {
 public:
  explicit MetalBackend(const BackendOptions& options={});
  ~MetalBackend();
  MetalBackend(const MetalBackend&)=delete;
  MetalBackend& operator=(const MetalBackend&)=delete;
  bool Ready(std::string& error) const;
  // Submit and cache maintenance belong to one frontend/worker thread. The
  // immutable packet and all GPU references survive through completion.
  bool Submit(std::shared_ptr<const Frame>,std::function<void(Completion)>,std::string& error);
  bool Drain(std::chrono::milliseconds timeout,std::string& error);
  // Call only after Drain. Physical readback is for validation/explicit guest
  // locks; presentation must not pass through this CPU path.
  std::vector<std::byte> ReadRGBA8(const SurfaceView&,std::string& error);
  BackendStats Stats() const;
  Capabilities GetDeviceCapabilities() const;
  void SetTimingEnabled(bool enabled);
  std::vector<std::byte> ReadBuffer(const BufferView&,std::string& error);
  // Opaque CAMetalLayer owned by the UIKit host, passed without API types into
  // the frame contract. Null disables presentation. The layer and backend use
  // the same device. Submission presents frame.output through a native pass.
  bool AttachLayer(void* layer,std::string& error);
  bool ConfigureBinaryArchive(const std::string& path,std::string& error);
  bool SaveBinaryArchive(std::string& error);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace rex::native
