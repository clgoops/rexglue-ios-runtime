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
- ReXGlue constant-bank preparation using the actual SDK system struct and
  sparse constant maps. Unchanged banks reuse immutable buffers; pending frames
  retain old data when later draws change the constants.
- A shared-memory adapter retains the SDK's physical-page watches and emits
  immutable dirty-page snapshots as ordered buffer uploads. It supports the
  translator's one/two/four buffer layout, splits uploads at native chunk
  boundaries and faults rejected frame scopes rather than continuing with
  incorrect coherence metadata. This adapter has compile validation only so far;
  guest concurrency and full-game integration remain unverified.

First backend validation (`c9564f9`, workflow run `40189292454`) passed 57 CPU
contracts, their ASan/UBSan run, and 16 actual Metal GPU cases with 134 checks.
The backend compiled for iOS ARM64 with deployment target 18.0. The downloaded
report explicitly confirmed a GPU was available. That validation predates the
shader-conversion additions; their new workflow must validate them separately.

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
