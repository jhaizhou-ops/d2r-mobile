# 标量 WASM 库，仅供显式实验选项的可选闭包；不代表浏览器或设备验证。
# Scalar WASM library for an explicit experimental optional closure, not browser/device validation.
# 基于 Arm 5.0.0 的静态库目标与 Emscripten 有界内存设置。
# Based on the Arm 5.0.0 static library target and bounded Emscripten memory settings.
cmake_minimum_required(VERSION 3.20)
project(bounded_astc LANGUAGES CXX)
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Use emcmake with an installed Emscripten SDK")
endif()
set(ASTCENC_ISA_NONE ON CACHE BOOL "" FORCE)
set(ASTCENC_ISA_NATIVE OFF CACHE BOOL "" FORCE)
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
set(ASTCENC_INVARIANCE ON CACHE BOOL "" FORCE)
add_subdirectory("${ASTC_SOURCE}" upstream)
add_executable(astc-encoder bridge.cpp)
target_link_libraries(astc-encoder PRIVATE astcenc-none-static)
target_compile_features(astc-encoder PRIVATE cxx_std_14)
set_target_properties(astc-encoder PROPERTIES SUFFIX ".mjs")
target_link_options(astc-encoder PRIVATE
  "--no-entry" "-sMODULARIZE=1" "-sEXPORT_ES6=1" "-sENVIRONMENT=worker"
  "-sFILESYSTEM=0" "-sALLOW_MEMORY_GROWTH=1" "-sINITIAL_MEMORY=33554432"
  "-sMAXIMUM_MEMORY=134217728" "-sSTACK_SIZE=1048576" "-sABORTING_MALLOC=0"
  "-sEXPORTED_FUNCTIONS=['_malloc','_free','_astc_output_size','_astc_encode_rgba','_astc_encoder_abi_version']"
  "-sEXPORTED_RUNTIME_METHODS=['HEAPU8']")
