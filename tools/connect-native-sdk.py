"""Wire the reviewed native backend into the pinned SDK's GPU plugin.

The legacy sources remain linked to preserve dynamic exports for existing AOT
apps. The iOS factory returns only NativeGraphicsSystem; there is no fallback
to a Vulkan command processor when native initialization or a draw fails.
"""
from pathlib import Path
import sys

sdk = Path(sys.argv[1]).resolve()
factory = sdk / 'src/graphics/plugin_main.cpp'
source = factory.read_text()
assert 'REX_NATIVE_GAME_BACKEND' not in source
source = source.replace('#include <string_view>', '#include <string_view>\n#include <rex/native/rex_live.h>')
needle = '  std::string_view backend = info->backend ? info->backend : "any";'
assert source.count(needle) == 1
source = source.replace(needle, needle + '''
  // REX_NATIVE_GAME_BACKEND: existing iOS launchers may request the historical
  // "vulkan" name. This package explicitly replaces that route with Metal.
  if (backend == "any" || backend == "metal" || backend == "native" || backend == "vulkan") {
    REXLOG_INFO("rexgpu-xenos: selecting native Metal backend (requested '{}')", backend);
    return new rex::native::NativeGraphicsSystem();
  }
  REXLOG_ERROR("Native Metal package does not support backend '{}'", backend);
  return nullptr;
''')
factory.write_text(source)
with (sdk / 'CMakeLists.txt').open('a') as out:
    out.write('''
# Reviewed native Metal guest backend. No title code is linked by this build.
set(REX_NATIVE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REX_NATIVE_METAL_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
set(REX_NATIVE_REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
set(REX_NATIVE_SPIRV_CROSS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../spirv-cross-src" CACHE PATH "" FORCE)
add_subdirectory("../native-renderer" "native-renderer")
target_link_libraries(rex-native-live PUBLIC rexruntime rex-native-metal)
target_link_libraries(rexgpu-xenos PRIVATE rex-native-live)
foreach(native_target rex-native-frame rex-native-spirv rex-native-metal rex-native-rex-adapters rex-native-ui rex-native-live)
  set_target_properties(${native_target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endforeach()
''')
print('Native Metal graphics system and command processor selected by the GPU plugin factory')
