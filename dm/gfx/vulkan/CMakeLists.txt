cmake_minimum_required(VERSION 3.22)
project(dm_vulkan LANGUAGES C CXX)
set(E ${CMAKE_CURRENT_SOURCE_DIR}/engine)
add_library(dm_vulkan_engine STATIC
 ${E}/xbg_game.cpp ${E}/xbg_shader_key.c ${E}/xbg_command_clone.c
 ${E}/xbg_draw_plan.c ${E}/xbg_vertex.c ${E}/xbg_quad.c ${E}/xbg_stages.cpp
 ${E}/xbg_clear.cpp ${E}/xbg_blit.cpp ${E}/xbg_texture.c ${E}/xbg_depth.c
 ${E}/xbg_framebuffer.c ${E}/xbg_vulkan.cpp ${E}/xbg_nvn.cpp ${E}/xbg_sampler.cpp ${E}/xbg_spirv_quad.cpp ${E}/xbg_spirv_warp.cpp ${E}/xbg_spirv_ldc.cpp ${E}/xbg_spirv_temps.cpp ${E}/xbg_spirv_raster.cpp)
target_include_directories(dm_vulkan_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ../nvn/include)
target_compile_features(dm_vulkan_engine PUBLIC cxx_std_17)
target_compile_options(dm_vulkan_engine PRIVATE -O2 -fno-strict-aliasing -Wall -Wextra -Wno-missing-field-initializers -Wno-format)
set_source_files_properties(${E}/xbg_texture.c PROPERTIES COMPILE_OPTIONS -fsigned-char)
set_target_properties(dm_vulkan_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  if(NOT TARGET Vulkan::Vulkan)
    find_package(Vulkan REQUIRED)
  endif()
  target_link_libraries(dm_vulkan_engine PUBLIC Vulkan::Vulkan)
else()
  target_link_libraries(dm_vulkan_engine PUBLIC vulkan log)
endif()
# nvn_vk.c 在平台适配目标中使用宿主兼容选项编译；引擎复用 dmcore 的纹理函数，不重复定义。
# nvn_vk.c uses host compatibility flags in the platform adapter; the engine reuses dmcore's texture functions without duplicate definitions.
