# Native Metal guest graphics backend

The SDK GPU factory can now select `NativeGraphicsSystem`. Its live command
processor handles guest shader analysis, native draw/pipeline bindings, Xbox
texture tiling and endian conversion, primitive/index processing, EDRAM ownership
transfers, resolve-to-guest-memory, gamma presentation and the SDK touch/UI drawer.
The native game-runtime workflow builds this GPU plugin and the generic runtime
for iOS ARM64, minimum iOS 18.0. Private packaging supplies the owner's existing
AOT game application; this public module contains no game code or assets.

The native factory does not fall back to Vulkan. Historical launcher requests
using the `vulkan` name are explicitly routed to Metal and identified in the log.
Legacy SDK sources remain available for ABI compatibility, with their notices.
GPU queues, caches and frame owners are bounded. Guest submissions are preserved;
presentation does not use CPU readback. Explicit guest memory readbacks use a
bounded synchronized path. Pipeline archives are stored separately from Vulkan
shader caches. Existing Vulkan cache files cannot be preloaded as Metal shaders.

The implementation uses architecture patterns reviewed in Theft4 revision
`88115fefad59cfcb62d17c0c9548a7a900c01a23`: ordered API-neutral commands, explicit
resource ownership, reflected shader interfaces and completion-owned uploads.
No Theft4 implementation, game shader, asset or title-specific ABI was copied.
ReXGlue and Xenia retain their upstream BSD notices. SPIRV-Cross is pinned in
[DEPENDENCIES.md](DEPENDENCIES.md); its readonly storage-buffer mapping is corrected
for this explicit binding model before compilation.

Apple workflow [40233547889](https://github.com/clgoops/rexglue-ios-runtime/actions/runs/40233547889)
passed 75 CPU frame contracts, 32 binding checks, 1,836 EDRAM shader conversions,
27 actual SDK translator fixtures and 4,185 frontend checks. Actual Metal GPU
cases passed native draws, blending, depth/stencil, 1x/2x/4x ownership transfers,
GPU buffer/texture copies, and target-to-EDRAM-to-texture conversion. The native
backend and live frontend adapters compiled for iOS ARM64. Later checks also
compile the production gamma and touch-overlay shaders and test attachmentless
fragment memory export. See VALIDATION.json for the latest verified stage.

These are synthetic fixtures, not captured Blur frames. A successful runtime
build does not establish device compatibility, visual fidelity or an FPS gain.
The live path currently uses original guest resolution. Native tessellation
requires additional explicit lowering; unsupported operations stop the session
with an error rather than silently substituting a placeholder rendering path.
First launch can compile newly encountered pipelines. Only actual iPhone testing
can establish playability and performance of the packaged game.

Build the isolated module with CMake 3.25+ and C++23. Set
`REX_NATIVE_SPIRV_CROSS_DIR` and `REX_NATIVE_REXSDK_DIR` to the reviewed dependencies.
The live frontend also requires the runtime patches that define the app's frame
metrics/performance ABI. `tools/connect-native-sdk.py` in the runtime package
links this module to the SDK's actual `rexgpu-xenos` target.

The UI upload fix is validated in Apple run 40253231180, and the complete iOS
runtime built in run 40253234741 (source 798e458). The actual ImmediateDrawer,
SDK base methods and packed vertex/index data now run in a headless composition
test: first atlas upload, cached second frame and white fallback pass 33 checks
with real Metal pixels. Restoring a zero image pitch reproduces frame rejection.
Optional UI rejection no longer immediately stops guest presentation. Device
execution of the updated game still requires confirmation.

Native resource admission now coordinates the sampled texture cache with the
surface budget: 128/256 MiB sampled-texture limits with a two-second soft
lifetime, and 768 MiB total native surfaces. At guest swaps or a surface high-water mark,
the common SDK prunes render targets which no longer own EDRAM data, retaining
both host-depth precision owners. Accumulated bindings are invalidated before
pruning; immutable packets retain image owners until GPU completion. A full
expired-owner sweep and bounded wait for older GPU packets precede a budget
rejection. Live images are never discarded to satisfy the cap. GPU regression
cases exercise pressure beyond the incremental scan and preservation of live
pixels. Their results for this revision must be checked in VALIDATION.json.

The surface-retirement revision (source 70c416a) passed Apple renderer run
40292417303: 21 native GPU cases / 446 checks, including full pressure scanning
and live-image preservation, plus all 33 production UI checks. Full native iOS
runtime run 40292423829 succeeded. These results do not establish a device FPS
gain; the replacement game package requires an iPhone test.

The next revision keeps a bounded 128 MiB exact-key surface reuse pool to avoid
reallocating targets that briefly lose EDRAM ownership. Compatible passes can
keep their attachments in one encoder across aligned CPU buffer uploads. The
upload uses a rasterization-disabled vertex copy with vertex-origin memory
barriers; preceding fragment-buffer access, memexport, texture/compute work,
unaligned copies and incompatible attachment states retain encoder boundaries.
Render extents follow actual scissors and partial clears, preserving full
attachment clears. Tests cover old/new vertex values, fixed-function index
fetch, fragment-hazard fallback, 1x/2x/4x MSAA and surrounding pixel preservation.
A 128-pass synthetic workload reports GPU windows with batching off/on; it is
not a captured game workload or an iPhone FPS prediction. Validation of this
revision is pending until its Apple workflow results are recorded.

Upload batching is disabled by default. Activation requires a physical Apple9+
GPU and three isolated on-device pixel checks: old/new vertex data, hardware
index fetch and 4x MSAA. A failure leaves conventional uploads active. The
hosted Apple Paravirtual GPU rejects both render-barrier APIs on macOS 15/26;
its tests must report batching unavailable, not claim an optimized-path result.
The render-area benchmark uses conventional uploads in both modes, isolating
attachment extent reduction without requiring unsupported barriers.
