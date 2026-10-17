cmake_minimum_required(VERSION 3.20)
project(xbl_harmony_runtime LANGUAGES C ASM)
if(NOT OHOS OR NOT OHOS_ARCH STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Use the official OHOS toolchain with OHOS_ARCH=arm64-v8a")
endif()

if(DEFINED XBLR_GENERATED_EXECUTION)
  file(GLOB XBLR_EXECUTION_ASSEMBLY "${XBLR_GENERATED_EXECUTION}/harmony_*.S")
  file(GLOB XBLR_SERVICE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/services/hle_*_ohos.c")
  add_library(xblr_execution SHARED src/loader_ohos.c src/hle_ohos_bridge.c
    src/execution.c src/services_ohos.c src/graphics_platform.c src/memory_layout.c src/instruction_gate.S
    "${XBLR_GENERATED_EXECUTION}/harmony_images.c" ${XBLR_EXECUTION_ASSEMBLY}
    ${XBLR_SERVICE_SOURCES})
  target_include_directories(xblr_execution PUBLIC include PRIVATE src)
  target_compile_definitions(xblr_execution PRIVATE XBLR_EXECUTION_ENABLED=1 XBLR_FULL_SERVICES=1 _GNU_SOURCE=1)
  if(NOT EXISTS "${XBLR_OPUS_LIBRARY}" OR NOT EXISTS "${XBLR_OPUS_INCLUDE}/opus.h")
    message(FATAL_ERROR "Set XBLR_OPUS_LIBRARY and XBLR_OPUS_INCLUDE to your ARM64 OHOS Opus 1.5.2 build")
  endif()
  target_link_libraries(xblr_execution PRIVATE ohaudio "${XBLR_OPUS_LIBRARY}")
  target_include_directories(xblr_execution PRIVATE "${XBLR_OPUS_INCLUDE}")
  set_source_files_properties(${XBLR_SERVICE_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-format;-Wno-unused-function")
  set_target_properties(xblr_execution PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_VISIBILITY_PRESET hidden)
  target_compile_options(xblr_execution PRIVATE -Wall -Wextra -Werror
    -Wno-unused-command-line-argument -ffixed-x18 -mno-outline-atomics)
  target_link_options(xblr_execution PRIVATE
    "-Wl,-T,${XBLR_GENERATED_EXECUTION}/harmony_layout.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/xblr_execution.map"
    -Wl,-z,max-page-size=4096 -Wl,-z,separate-code -Wl,-z,text -Wl,-z,relro
    -Wl,-z,now -Wl,--no-undefined)
  set_property(TARGET xblr_execution APPEND PROPERTY LINK_DEPENDS "${XBLR_GENERATED_EXECUTION}/harmony_layout.ld")
endif()
add_library(xblr_runtime SHARED src/runtime.c src/input_gate.c src/guest.S src/abi_entry.S)
target_include_directories(xblr_runtime PUBLIC include PRIVATE src)
set_target_properties(xblr_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_VISIBILITY_PRESET hidden)
target_compile_options(xblr_runtime PRIVATE -Wall -Wextra -Werror -Wno-unused-command-line-argument
  -ffixed-x18 -mno-outline-atomics)
target_link_options(xblr_runtime PRIVATE
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/src/guest_layout.ld"
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/xblr_runtime.map"
  -Wl,-z,max-page-size=16384 -Wl,-z,separate-code -Wl,-z,text -Wl,-z,relro -Wl,-z,now
  -Wl,--no-undefined)
set_property(TARGET xblr_runtime APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/guest_layout.ld" "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
# Compile the host integration example too; it is not an additional runtime export.
add_library(xblr_integration_example OBJECT examples/integration.c)
target_include_directories(xblr_integration_example PRIVATE include)
target_compile_options(xblr_integration_example PRIVATE -Wall -Wextra -Werror
  -Wno-unused-command-line-argument -ffixed-x18 -mno-outline-atomics)

# Real NSOs remain external artifacts. This optional target embeds their VERIFIED
# decompressed images as READ-ONLY DATA, never AX code, while the real loader runs.
if(DEFINED XBLR_GENERATED_ORIGINALS)
  add_library(xblr_originals SHARED src/loader_ohos.c src/originals_bridge.c src/hle_ohos_bridge.c
    "${XBLR_GENERATED_ORIGINALS}/original_images.c" "${XBLR_GENERATED_ORIGINALS}/original_images.S"
    "${XBLR_GENERATED_ORIGINALS}/hle_movie_ohos.c")
  target_include_directories(xblr_originals PUBLIC include PRIVATE src)
  set_target_properties(xblr_originals PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_VISIBILITY_PRESET hidden)
  target_compile_options(xblr_originals PRIVATE -Wall -Wextra -Werror
    -Wno-unused-command-line-argument -ffixed-x18 -mno-outline-atomics)
  target_link_options(xblr_originals PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,separate-code
    -Wl,-z,text -Wl,-z,relro -Wl,-z,now -Wl,--no-undefined
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/xblr_originals.map")
endif()
