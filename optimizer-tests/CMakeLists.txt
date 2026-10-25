cmake_minimum_required(VERSION 3.25)
project(RegisterOptimizerTests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT REXSDK_DIR)
  message(FATAL_ERROR "REXSDK_DIR is required")
endif()
set(SPIRV-Headers_SOURCE_DIR "${REXSDK_DIR}/thirdparty/spirv-headers")
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
if(NOT TARGET SPIRV-Tools-opt)
  add_subdirectory("${REXSDK_DIR}/thirdparty/spirv-tools" spirv-tools EXCLUDE_FROM_ALL)
endif()
add_executable(register-opt-tests tests.cpp "${REXSDK_DIR}/src/graphics/vulkan/spirv_register_optimizer.cpp")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract-module.py"
  "${REXSDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/shader-module.inc" COMMAND_ERROR_IS_FATAL ANY)
target_include_directories(register-opt-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${REXSDK_DIR}/include"
  "${REXSDK_DIR}/thirdparty/spirv-headers/include" "${REXSDK_DIR}/thirdparty/vulkan-headers/include")
target_link_libraries(register-opt-tests PRIVATE SPIRV-Tools-opt)

# Exercise the actual translator, not just hand-written SPIR-V kernels.
set(SDK "${REXSDK_DIR}")
add_executable(register-translator-tests translator-tests.cpp
  "${SDK}/src/core/string_buffer.cpp"
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
  "${SDK}/src/graphics/vulkan/spirv_register_optimizer.cpp"
  "${SDK}/thirdparty/glslang/SPIRV/SpvBuilder.cpp"
  "${SDK}/thirdparty/glslang/SPIRV/SpvPostProcess.cpp"
  "${SDK}/thirdparty/glslang/SPIRV/InReadableOrder.cpp"
  "${SDK}/thirdparty/glslang/SPIRV/Logger.cpp")
target_include_directories(register-translator-tests PRIVATE stubs "${SDK}/include"
  "${SDK}/thirdparty/fmt/include" "${SDK}/thirdparty/glslang"
  "${SDK}/thirdparty/spirv-tools/include" "${SDK}/thirdparty/spirv-headers/include"
  "${SDK}/thirdparty/vulkan-headers/include" "${SDK}/thirdparty/xxhash"
  "${SDK}/thirdparty/renderdoc")
target_compile_definitions(register-translator-tests PRIVATE NDEBUG FMT_HEADER_ONLY _CRT_SECURE_NO_WARNINGS)
target_compile_options(register-translator-tests PRIVATE -O2 -Wno-unused-variable)
target_link_libraries(register-translator-tests PRIVATE SPIRV-Tools-opt)
