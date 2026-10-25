# Native Metal renderer: integration in progress

This independently implemented BSD-3-Clause module follows architectural patterns
reviewed in Theft4 revision `88115fefad59cfcb62d17c0c9548a7a900c01a23`:
API-neutral ordered frames, explicit target/resolve ownership, reflected shader
interfaces, bounded caches and upload lifetimes ending at GPU completion. No
Theft4 implementation, game shader, asset, title hook or title-specific ABI is
copied here. ReXGlue and Xenia retain their own notices and contracts.

Implemented:

- Native Metal render/compute/blit/resolve commands and presentation.
- Ordered pass merging only where attachment/load/store meaning is preserved.
- Reflected pipeline bindings, separate constant/geometry storage, incremental
  surface reclamation, binary pipeline archives and completion-owned frames.
- Direct SPIRV-Cross conversion with explicit descriptor-to-Metal mappings.
- A native SDK shader subclass prepares and caches each actual microcode
  modification directly as MSL, preserving the SDK's texture/sampler records.
  Its translation objects are scoped to one stable backend feature profile.
- ReXGlue constant-bank preparation using the actual SDK system struct and
  sparse constant maps. Unchanged banks reuse immutable buffers; pending frames
  retain old data when later draws change the constants.
- A shared-memory adapter retains the SDK's physical-page watches and emits
  immutable dirty-page snapshots as ordered buffer uploads. It supports the
  translator's one/two/four buffer layout, splits uploads at native chunk
  boundaries and faults rejected frame scopes rather than continuing with
  incorrect coherence metadata. This adapter has compile validation only so far;
  guest concurrency and full-game integration remain unverified.
- Draw-state preparation now uses the SDK's register math, shader modification
  keys, texture signs, depth flags and color exponent biases with an explicit
  native capability profile.
- The target adapter retains the SDK's EDRAM ownership tracker. Its default
  transfer encoder uses the SDK's bit-preserving format conversion shaders,
  native 1x/2x/4x MSAA, integer format views and depth/stencil output. When retained
  host depth is also the destination, a preceding native depth snapshot avoids
  illegal attachment feedback. CPU checks cover 1,836 shader combinations and
  frame admission. Apple run `40215220850` passed the iOS shader compilation,
  actual Metal transfer pixel cases and iOS ARM64 adapter build. The pixel cases
  cover half-bit reinterpretation with integer views at 1x/2x/4x MSAA and a
  single-sample depth/stencil round trip with retained host-depth snapshot.
  Resolve-to-guest-memory and the live command processor are not connected yet.

Latest verified implementation: `264bc35`, Apple workflow run
[`40215220850`](https://github.com/clgoops/rexglue-ios-runtime/actions/runs/40215220850).
It passed 72 CPU frame contracts and their ASan/UBSan run, 32 binding checks,
1,836 ownership shader conversions, 4,019 combined frontend checks, and Apple's
iOS compilation of 96 SDK host modules plus selected transfer/translator fixtures.
Actual GPU hardware passed 18 native cases with 153 checks, the SDK gamma/EDRAM
clear cases, microcode-to-Metal draws with two in-flight constant snapshots, and
native target transfers at 1x/2x/4x MSAA. GPU conversion buffers also became
native textures with explicit pitches, offsets and 3D image strides.

The backend and SDK adapter libraries compiled for iOS ARM64, minimum iOS 18.0.
The downloaded backend's architecture, deployment target, lack of Vulkan linkage
and SHA-256 were checked locally. See `VALIDATION.json` for the artifact identity.
These fixtures do not establish Blur frame fidelity or performance. The game
factory, command submission, guest texture loading and resolve path still require
integration. There is no playable native-game IPA from this stage.

Local shader validation converts 96 of 97 pinned SDK host shaders. The remaining
`passthrough_position_xy_vs` uses vertex attributes; the backend currently uses
buffer-pulled vertices. The same local check converts 27 actual ReXGlue translator
fixtures, including dynamic register access, across SPIR-V 1.0/1.3/1.5. These are
synthetic microcode fixtures, not captured game shaders or rendered game frames.

Still required before a native game build:

1. Route the real command processor's shader/pipeline preparation into this frame
   contract, using the correct current registers and shader modifications.
2. Adapt shared-memory dirty pages, primitive/index lowering, texture uploads and
   target/resolve aliases while preserving endian, tiling, gamma and ownership.
3. Connect presentation/launcher lifecycle, validate complete game frames against
   the working renderer, then measure the same race under matched conditions.

The live game still uses its existing renderer. This module is not an FPS fix
or an installable game build yet. A successful native backend test does not
establish game compatibility or a 30/60/120 FPS result.

Build with CMake 3.25+ and C++23. See [DEPENDENCIES.md](DEPENDENCIES.md) for the
pinned shader converter. `REX_NATIVE_REXSDK_DIR` enables the actual SDK adapters
and isolated translator tests; it must point to the reviewed SDK revision.
