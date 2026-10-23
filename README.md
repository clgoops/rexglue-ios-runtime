# ReXGlue iOS runtime build

Intended repository: **clgoops/rexglue-ios-runtime**. This source package supports
public review and reproducible builds of the open-source runtime components.

Build the open-source ReXGlue runtime and Xenos graphics plugin for iOS 18+
ARM64. The repository contains SDK patches, two renderer test sources, build
and artifact-audit scripts, an input checksum manifest and upstream licensing.
It contains no recompiled game source, original game executable, assets, IPA,
device logs, credentials or signing material. It cannot build a complete game.

The workflow fetches SDK c94f5ebdcb3c9d1a460ca48e04f9758448f8d518 and its
pinned submodules, applies the reviewed patches, runs the focused renderer
tests, compiles those tests for iOS ARM64, and builds rexgpu-xenos plus its
runtime dependency. Artifacts contain the two runtime libraries, license
notices and checksums. They are retained for one day. Device signing and game
packaging happen separately on the owner's Windows machine.

The current renderer change bounds ownership-transfer scissors to the copied
rectangles so attachment render-area optimization can take effect. Host/source
checks passed; Apple GPU correctness and FPS improvement remain unverified.
The normal game configuration continues to select the host-target path.

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
