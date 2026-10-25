# Isolated translator service stubs are used only by this test executable.
set(SDK "${REX_NATIVE_REXSDK_DIR}")
add_executable(native-rex-shader-tests tests/rex-shaders.cpp
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
