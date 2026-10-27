# Isolated translator service stubs are used only by this test executable.
set(SDK "${REX_NATIVE_REXSDK_DIR}")
# Compile the pinned SDK's exact read-only loader metadata without linking its
# memory manager into the isolated tests. No replacement metadata is invented.
file(READ "${SDK}/src/graphics/pipeline/texture/cache.cpp" TEXTURE_CACHE_SOURCE)
string(REGEX MATCH "const TextureCache::LoadShaderInfo TextureCache::load_shader_info_\\[kLoadShaderCount\\] = \\{[^;]*\\};" TEXTURE_LOAD_INFO "${TEXTURE_CACHE_SOURCE}")
if(NOT TEXTURE_LOAD_INFO)
  message(FATAL_ERROR "Pinned texture load metadata was not found")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/native-test-texture-info.cpp" "#include <rex/graphics/pipeline/texture/cache.h>\nnamespace rex::graphics {\n${TEXTURE_LOAD_INFO}\n}\n")
add_executable(native-rex-shader-tests tests/rex-shaders.cpp tests/math-stubs.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/native-test-texture-info.cpp"
  "${SDK}/src/graphics/pipeline/texture/util.cpp"
  "${SDK}/src/graphics/pipeline/texture/info_formats.cpp"
  "${SDK}/src/core/string_buffer.cpp"
  "${SDK}/src/graphics/register_file.cpp"
  "${SDK}/src/graphics/format/ucode.cpp"
  "${SDK}/src/graphics/pipeline/shader/shader.cpp"
  "${SDK}/src/graphics/pipeline/shader/translator.cpp"
  "${SDK}/src/graphics/pipeline/shader/translator_disasm.cpp"
  "${SDK}/src/graphics/pipeline/shader/spirv.cpp"
  "${SDK}/src/graphics/pipeline/shader/spirv_builder.cpp"
  "${SDK}/src/graphics/pipeline/shader/spirv_translator.cpp"
  "${SDK}/src/graphics/pipeline/shader/spirv_translator_alu.cpp"
  "${SDK}/src/graphics/pipeline/shader/spirv_translator_fetch.cpp"
  "${SDK}/src/graphics/pipeline/shader/spirv_translator_memexport.cpp"
  "${SDK}/src/graphics/pipeline/shader/spirv_translator_rb.cpp"
  "${SDK}/thirdparty/glslang/SPIRV/SpvBuilder.cpp"
  "${SDK}/thirdparty/glslang/SPIRV/SpvPostProcess.cpp"
  "${SDK}/thirdparty/glslang/SPIRV/InReadableOrder.cpp"
  "${SDK}/thirdparty/glslang/SPIRV/Logger.cpp")
target_include_directories(native-rex-shader-tests BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../optimizer-tests/stubs"
  "${SDK}/thirdparty/spirv-tools/include" "${SDK}/thirdparty/spirv-headers/include"
  "${SDK}/thirdparty/renderdoc")
target_compile_definitions(native-rex-shader-tests PRIVATE NDEBUG FMT_HEADER_ONLY _CRT_SECURE_NO_WARNINGS)
target_compile_options(native-rex-shader-tests PRIVATE -Wno-unused-variable)
target_link_libraries(native-rex-shader-tests PRIVATE rex-native-rex-adapters)
add_test(NAME native-rex-shaders COMMAND native-rex-shader-tests)
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  target_compile_definitions(native-rex-shader-tests PRIVATE REX_NATIVE_TEST_METAL=1)
  target_link_libraries(native-rex-shader-tests PRIVATE rex-native-metal)
  add_executable(native-bc-tests tests/bc-textures.cpp tests/math-stubs.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/native-test-texture-info.cpp"
    "${SDK}/src/graphics/pipeline/texture/util.cpp"
    "${SDK}/src/graphics/pipeline/texture/info_formats.cpp")
  target_compile_definitions(native-bc-tests PRIVATE NDEBUG FMT_HEADER_ONLY)
  target_link_libraries(native-bc-tests PRIVATE rex-native-rex-adapters rex-native-metal)
  add_test(NAME native-bc COMMAND native-bc-tests)
elseif(NOT APPLE)
  add_library(native-bc-compile OBJECT tests/bc-textures.cpp)
  target_compile_definitions(native-bc-compile PRIVATE NDEBUG FMT_HEADER_ONLY)
  target_link_libraries(native-bc-compile PRIVATE rex-native-rex-adapters)
endif()
