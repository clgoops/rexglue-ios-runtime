# The pinned SPIRV-Cross exposes all SSBOs as `device`, including blocks whose
# complete SPIR-V member decoration is NonWritable. Apple then reflects those
# inputs as read/write. This backend has discrete, explicit resource bindings;
# readonly bindings never alias writes within a shader. Preserve that contract
# in MSL by applying const to the same complete block flags used by reflection.
set(_native_msl_source "${REX_NATIVE_SPIRV_CROSS_DIR}/spirv_msl.cpp")
file(READ "${_native_msl_source}" _native_msl)
set(_native_old "if (entry_point_requires_const_device_buffers() && type.basetype != SPIRType::AtomicCounter)")
set(_native_new "if ((flags.get(DecorationNonWritable) || entry_point_requires_const_device_buffers()) && type.basetype != SPIRType::AtomicCounter)")
string(FIND "${_native_msl}" "${_native_new}" _native_patched)
if(_native_patched EQUAL -1)
  string(REGEX MATCHALL "entry_point_requires_const_device_buffers\\(\\) && type.basetype" _native_matches "${_native_msl}")
  list(LENGTH _native_matches _native_count)
  if(NOT _native_count EQUAL 2)
    message(FATAL_ERROR "Pinned SPIRV-Cross readonly fix does not match the reviewed source")
  endif()
  string(REPLACE "${_native_old}" "${_native_new}" _native_msl "${_native_msl}")
  file(WRITE "${_native_msl_source}" "${_native_msl}")
endif()
