# Native renderer dependencies

Shader conversion uses KhronosGroup/SPIRV-Cross commit
`adec7acbf41a988713cdb85f93f26c8ca5ea863e`, the revision pinned by this
project's MoltenVK dependency. Pass its checkout as `REX_NATIVE_SPIRV_CROSS_DIR`.
Conversion calls SPIRV-Cross directly and does not use MoltenVK or create Vulkan
objects. SPIRV-Cross retains its Apache-2.0-or-MIT licensing in its own checkout.

`native-shader-export` writes MSL and an explicit resource manifest. Descriptor
set numbers and bindings retain their ReXGlue meaning; Metal slots are compact
and stage-local. Storage arrays, push constants, specialization defaults and
the auxiliary buffer for `OpArrayLength` are reflected rather than inferred
from Theft4's title-specific layout. Shader compilation preserves full precision.

SPIRV-Cross lowers 1D images to 2D rows and linear texel images to row-major 2D
images. Their resource mappings identify this requirement explicitly; coordinate
conversion uses the actual image width. No fixed-width GTA texture layout applies.

The frontend must lower unsupported vertex attributes, subpass images, runtime
descriptor arrays and specialized workgroup sizes explicitly.
Conversion fails on these contracts. No dummy resources or guessed bindings are
substituted. Successful MSL conversion still requires Apple compilation, actual
Metal reflection, pixel validation and correct guest texture/target lowering.
