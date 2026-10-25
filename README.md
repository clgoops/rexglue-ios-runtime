# ReXGlue iOS runtime build

Intended repository: **clgoops/rexglue-ios-runtime**. This source package supports
public review and reproducible builds of the open-source runtime components.

Build the open-source ReXGlue runtime and Xenos graphics plugin for iOS 18+
ARM64. The repository contains SDK patches, focused renderer test sources, build
and artifact-audit scripts, an input checksum manifest and upstream licensing.
It contains no recompiled game source, original game executable, assets, IPA,
device logs, credentials or signing material. It cannot build a complete game.

The workflow fetches SDK c94f5ebdcb3c9d1a460ca48e04f9758448f8d518 and its
pinned submodules, applies the reviewed patches, runs the focused renderer
tests, compiles those tests for iOS ARM64, and builds rexgpu-xenos plus its
runtime dependency. Artifacts contain the two runtime libraries, the diagnostic MoltenVK driver, license
notices and checksums. They are retained for one day. Device signing and game
packaging happen separately on the owner's Windows machine.

The current diagnostic additionally builds the pinned MoltenVK driver with
completion-window reporting for command buffers labeled `rex.native.*`. It reads
GPUStartTime/GPUEndTime and CPU scheduling properties in the existing completion
handler, before submission destruction. No extra GPU counter commands or waits
are introduced. Native mode disables both Vulkan submission timestamps and sparse
pass queries while keeping FPS/CPU metrics. GPU span fields in the old overlay
are unavailable in this mode; native windows appear in the exported log.

The log identifies each guest frame/submission, execution window, pre-GPU interval,
driver scheduling interval and completion-callback lag. An execution window may
include intra-buffer stalls; it is not a shader-busy-cycle counter. Unknown/failed
timestamps are marked invalid instead of reporting a false zero. CPU-only tests
run the actual reporting function with captured Metal properties; they do not
prove GPU behavior or a speedup. Test targets also compile for iOS ARM64.

MoltenVK source modifications retain Apache-2.0 licensing and upstream notices.
Existing performanceTracking settings and renderer algorithms are unchanged.

The current runtime adds local SPIR-V register canonicalization before shader
module creation: scalar replacement, local access conversion, load/store
elimination, SSA rewriting and dead local work removal. It validates input and
output and checks preservation of globals, bindings, entry points, execution
modes and specialization constants. Failed validation or driver module creation
retains/retries the original module. It does not enable float relaxation or
half precision. The pinned SPIRV-Tools implementation is statically linked;
upstream license notices are collected with the runtime artifacts. The Mac job
tests the optimizer and actual module-creation fallback and compiles them for iOS.
Validation uses the uniform-buffer-standard and scalar-block-layout features
actually enabled on the Vulkan device, for both optimizer input and output.
Default validator layout rules reject the translator's packed system constants
before optimization; patch 022 fixes that fallback bug without skipping validation
or changing the uniform-buffer ABI. A separate test target compiles the actual
translator and covers empty, ALU and dynamically addressed register shaders in
SPIR-V 1.0, 1.3 and 1.5, including disabled-feature rejection. It is run on Mac
and compiled for iOS ARM64. Constructed-kernel tests also exercise feature-aware
module creation and still reject misaligned data even with scalar layout enabled.
These checks do not execute a GPU or use game shaders.
Device rendering and performance remain unverified. Sparse pass sampling now
defaults Off; ordinary performance metrics remain available.

The transfer-bounds build compiled successfully and was reported visually
correct, with no useful performance gain. The current diagnostic adds sparse
same-queue completion-boundary samples for rendering and outside-pass work.
Eight segments rotate through one in four eligible submissions, with at least
16 dynamic rendering passes. It reads availability after the existing fence;
allocation failures disable the diagnostic. The existing performance toggle
gates capture. Results include pass dimensions, domain bits, work counts and
shader identifiers in the local application log; device logs stay private.

These are approximate boundary spans, including timestamp overhead, not shader
busy time or a complete/unbiased frame-cost breakdown. MoltenVK may coalesce
timestamps; zero/unavailable results are explicitly reported. No native driver
profiling settings or renderer backend are changed. The host-target path remains.

Use standard macos-15 runners. GitHub documents standard hosted runners as
[free for public repositories](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
This is a public build of open-source runtime components on the same account;
the game repository stays private. Runner admission and a successful build are
still required. No paid or larger runner is configured.

`python3 tools/audit-inputs.py` verifies the exact reviewed input allowlist and
checksums. Run it before publishing or building. Runtime output libraries must
be audited for ARM64/iOS metadata and matched imports before local packaging;
a successful build alone does not establish application compatibility.

LICENSE contains the upstream BSD 3-Clause notice for ReXGlue and the derived
Xenia portions. Existing upstream and third-party notices apply to their
respective code. New patch/test helper contributions use the same BSD 3-Clause
terms. Library artifacts collect upstream license/notice files as well.

## Latest-session diagnostics helper

`diagnostics/` contains a generic Apache-2.0 iOS log-export helper. It exports one
latest game-session snapshot and admits a performance CSV only when its filename
timestamp matches that session's Play request within two seconds. Without a game
session it exports the latest startup. Historical source files stay in place but
are excluded from the share sheet. No game-specific configuration is published.
The standalone diagnostics workflow checks file selection on Mac, compiles iOS
ARM64, and tests the action replacement and single-item UIKit share sheet in Simulator.
This helper changes diagnostics export only, with no renderer changes.
