cmake_minimum_required(VERSION 3.20)
project(VulkanWorld CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_property(MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin REQUIRED)

# Compile GLSL -> SPIR-V into <build>/shaders
# (the executable looks in <exe dir>/shaders and <exe dir>/../shaders, which covers
#  single-config builds and Visual Studio's <build>/Debug, <build>/Release folders)
set(SHADER_SOURCES shaders/scene.vert shaders/scene.frag shaders/outline.vert shaders/outline.frag)
set(SPIRV_OUTPUTS)
foreach(src ${SHADER_SOURCES})
  get_filename_component(name ${src} NAME)
  set(out ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/${src} -o ${out}
    DEPENDS ${CMAKE_SOURCE_DIR}/${src}
    COMMENT "Compiling ${name}")
  list(APPEND SPIRV_OUTPUTS ${out})
endforeach()
add_custom_target(shaders ALL DEPENDS ${SPIRV_OUTPUTS})

# Vendored third-party UI libraries (Dear ImGui v1.92.9b, ImGuizmo)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUIZMO_DIR ${CMAKE_SOURCE_DIR}/third_party/ImGuizmo/src)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
  ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
  ${IMGUIZMO_DIR}/ImGuizmo.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUIZMO_DIR})
target_link_libraries(imgui PUBLIC Vulkan::Vulkan)

add_executable(vkworld
  src/main.cpp
  src/geometry.cpp
  src/scene.cpp
  src/renderer.cpp
  src/editor.cpp)
add_dependencies(vkworld shaders)
target_link_libraries(vkworld PRIVATE imgui)

# Platform layer
if(WIN32)
  target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
  if(NOT MSVC)
    target_link_libraries(imgui PUBLIC gdi32 dwmapi)   # MSVC gets these via #pragma comment
  endif()
  target_sources(vkworld PRIVATE src/platform_win32.cpp)
  target_compile_definitions(vkworld PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(APPLE)
  enable_language(OBJCXX)
  target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_osx.mm)
  target_compile_options(imgui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(imgui PUBLIC "-framework Cocoa" "-framework GameController")
  target_sources(vkworld PRIVATE src/platform_macos.mm)
  target_compile_options(vkworld PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(vkworld PRIVATE "-framework Cocoa" "-framework QuartzCore")
else()
  find_library(XCB_LIBRARY xcb REQUIRED)
  target_sources(vkworld PRIVATE src/platform_xcb.cpp src/imgui_impl_xcb.cpp)
  target_link_libraries(vkworld PRIVATE ${XCB_LIBRARY})
endif()

# Warnings
if(MSVC)
  target_compile_options(imgui PRIVATE /utf-8)
  target_compile_options(vkworld PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(vkworld PRIVATE _CRT_SECURE_NO_WARNINGS)
  set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT vkworld)
else()
  target_compile_options(vkworld PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
