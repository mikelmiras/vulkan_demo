cmake_minimum_required(VERSION 3.20)
project(VulkanWorld VERSION 0.5.0 LANGUAGES C CXX)   # (the engine and editor: core/version.h)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_property(MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Releases: optimised and with debug info, which tools/symbols.py then moves aside (crash reports are read back
# with it: tools/symbolicate.py). Every Linux binary gets a GNU build id, the key its symbols are kept by.
option(VKWORLD_RELEASE_SYMBOLS "Release builds with debug info (for crash reports; strip it with tools/symbols.py)" OFF)
if(VKWORLD_RELEASE_SYMBOLS)
  if(MSVC)
    add_compile_options($<$<CONFIG:Release>:/Zi>)
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
  else()
    add_compile_options($<$<CONFIG:Release>:-g>)
  endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_link_options(-Wl,--build-id)
endif()

# Executables find the engine library next to them ($ORIGIN / @loader_path), so a build folder can be copied as is
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin REQUIRED)

# Compile GLSL -> SPIR-V into <build>/shaders
# (the executable looks in <exe dir>/shaders and <exe dir>/../shaders, which covers
#  single-config builds and Visual Studio's <build>/Debug, <build>/Release folders)
set(SHADER_SOURCES shaders/scene.vert shaders/scene.frag shaders/outline.vert shaders/outline.frag shaders/shadow.vert shaders/shadow_cutout.frag
                   shaders/post.vert shaders/bloom_down.frag shaders/bloom_up.frag shaders/composite.frag shaders/ssao.frag shaders/ssao_ms.frag
                   shaders/particle.vert shaders/particle.frag shaders/sky.vert shaders/sky.frag shaders/taa.frag shaders/cull.comp
                   shaders/hiz_depth.comp shaders/hiz_depth_ms.comp shaders/hiz_reduce.comp shaders/foliage.vert
                   shaders/gi_project.comp shaders/ssr_capture.frag shaders/ssr_capture_ms.frag shaders/depth_prepass.frag
                   shaders/fog_inject.comp shaders/fog_integrate.comp shaders/fog_surface.comp shaders/luminance.frag
                   shaders/adapt.frag shaders/sky_transmittance.frag shaders/sky_view.frag
                   shaders/ssr_trace.frag shaders/gi_trace.frag)
set(SPIRV_OUTPUTS)
foreach(src ${SHADER_SOURCES})
  get_filename_component(name ${src} NAME)
  set(out ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/${src} -o ${out}
    DEPENDS ${CMAKE_SOURCE_DIR}/${src} ${CMAKE_SOURCE_DIR}/shaders/common.glsl ${CMAKE_SOURCE_DIR}/shaders/ssao.glsl ${CMAKE_SOURCE_DIR}/shaders/ssr_capture.glsl
            ${CMAKE_SOURCE_DIR}/shaders/hiz.glsl ${CMAKE_SOURCE_DIR}/shaders/fog.glsl
            ${CMAKE_SOURCE_DIR}/shaders/water.glsl ${CMAKE_SOURCE_DIR}/shaders/atmosphere.glsl
            ${CMAKE_SOURCE_DIR}/shaders/screen_trace.glsl ${CMAKE_SOURCE_DIR}/shaders/trace_common.glsl
    COMMENT "Compiling ${name}")
  list(APPEND SPIRV_OUTPUTS ${out})
endforeach()
# The fog's surface pass over a multisampled depth
set(out ${CMAKE_BINARY_DIR}/shaders/fog_surface_ms.comp.spv)
add_custom_command(
  OUTPUT ${out}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
  COMMAND ${GLSLC} -DMS ${CMAKE_SOURCE_DIR}/shaders/fog_surface.comp -o ${out}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/fog_surface.comp
  COMMENT "Compiling fog_surface_ms.comp")
list(APPEND SPIRV_OUTPUTS ${out})
# The lit scene shader again with early depth tests, for after the depth prepass
set(out ${CMAKE_BINARY_DIR}/shaders/scene_early.frag.spv)
add_custom_command(
  OUTPUT ${out}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
  COMMAND ${GLSLC} -DEARLY_DEPTH ${CMAKE_SOURCE_DIR}/shaders/scene.frag -o ${out}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/scene.frag ${CMAKE_SOURCE_DIR}/shaders/common.glsl ${CMAKE_SOURCE_DIR}/shaders/fog.glsl
          ${CMAKE_SOURCE_DIR}/shaders/screen_trace.glsl ${CMAKE_SOURCE_DIR}/shaders/water.glsl
  COMMENT "Compiling scene_early.frag")
list(APPEND SPIRV_OUTPUTS ${out})
# The depth prepass writing motion vectors too (temporal AA)
set(out ${CMAKE_BINARY_DIR}/shaders/depth_prepass_velocity.frag.spv)
add_custom_command(
  OUTPUT ${out}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
  COMMAND ${GLSLC} -DVELOCITY ${CMAKE_SOURCE_DIR}/shaders/depth_prepass.frag -o ${out}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/depth_prepass.frag
  COMMENT "Compiling depth_prepass_velocity.frag")
list(APPEND SPIRV_OUTPUTS ${out})
# The temporal pass without the history (upscaling alone: no depth read, so MSAA's multisampled depth can be bound)
set(out ${CMAKE_BINARY_DIR}/shaders/taa_upscale.frag.spv)
add_custom_command(
  OUTPUT ${out}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
  COMMAND ${GLSLC} -DUPSCALE_ONLY ${CMAKE_SOURCE_DIR}/shaders/taa.frag -o ${out}
  DEPENDS ${CMAKE_SOURCE_DIR}/shaders/taa.frag
  COMMENT "Compiling taa_upscale.frag")
list(APPEND SPIRV_OUTPUTS ${out})
add_custom_target(shaders ALL DEPENDS ${SPIRV_OUTPUTS})

# Vendored third-party UI libraries (Dear ImGui v1.92.9b-docking, ImGuizmo), compiled into the engine library: there must be
# one ImGui context for the engine, the programs and game modules
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUIZMO_DIR ${CMAKE_SOURCE_DIR}/third_party/ImGuizmo/src)
add_library(imgui OBJECT
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
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Jolt Physics v5.6.0 (vendored, MIT): rigid bodies. Its own CMake file, with every optional feature off (no debug
# renderer, profiler or GPU compute; baseline SIMD). Linked into the engine only: game code never sees it.
set(PHYSICS_REPO_ROOT ${CMAKE_SOURCE_DIR}/third_party/JoltPhysics)
include(${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake)
set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(Jolt PRIVATE /w)
else()
  target_compile_options(Jolt PRIVATE -w)
endif()

# Engine: everything a game needs at runtime (platform, rendering, assets, scene, game loop). A shared library, so
# game modules (project code loaded at run time, see game/module.h) use the same engine as the editor and vkgame.
# With VKWORLD_STANDALONE_GAMES it's also built as a static library, for games compiled into one executable.
# The game UI's default font (Roboto Medium, Apache 2.0, from Dear ImGui's misc/fonts), compiled in
set(DEFAULT_FONT_SOURCE ${CMAKE_BINARY_DIR}/generated/default_font.cpp)
add_custom_command(
  OUTPUT ${DEFAULT_FONT_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/third_party/imgui/misc/fonts/Roboto-Medium.ttf
          -DOUTPUT=${DEFAULT_FONT_SOURCE} -DSYMBOL=kDefaultGameFont -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/third_party/imgui/misc/fonts/Roboto-Medium.ttf ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  COMMENT "Embedding the default game font")

# The version and git commit, refreshed every build (core/version.h)
set(BUILD_INFO_SOURCE ${CMAKE_BINARY_DIR}/generated/build_info.cpp)
add_custom_target(build_info
  COMMAND ${CMAKE_COMMAND} -DVERSION=${PROJECT_VERSION} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${BUILD_INFO_SOURCE}
          -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${BUILD_INFO_SOURCE}
  COMMENT "Version and commit")

# HarfBuzz 14.6.0 (vendored, MIT-style "Old MIT"): text shaping (game/text_shaping.h), its one-file build, no
# optional backends (FreeType, ICU, CoreText, DirectWrite: off without their HAVE_* defines)
add_library(harfbuzz STATIC ${CMAKE_SOURCE_DIR}/third_party/harfbuzz/src/harfbuzz.cc)
target_include_directories(harfbuzz PUBLIC ${CMAKE_SOURCE_DIR}/third_party/harfbuzz/src)
set_target_properties(harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
if(MSVC)
  target_compile_options(harfbuzz PRIVATE /w /bigobj)
else()
  target_compile_options(harfbuzz PRIVATE -w)
endif()

# Lua 5.4.7, compiled as C++ so its errors unwind as exceptions through our code (no warnings from it)
# (without the libraries scripts don't get: os, io, debug, package loading, and linit's luaL_openlibs)
file(GLOB LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/src/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(loslib|liolib|loadlib|ldblib|linit)\\.c$")
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
set(ENGINE_SOURCES
  ${DEFAULT_FONT_SOURCE}
  ${BUILD_INFO_SOURCE}
  src/core/version.cpp
  src/game/fonts.cpp
  src/game/strings.cpp
  src/core/json.cpp
  src/core/png.cpp
  src/core/hdr_image.cpp
  src/core/project.cpp
  src/core/vfs.cpp
  src/core/sha256.cpp
  src/core/profiler.cpp
  src/core/crash.cpp
  src/core/log.cpp
  src/core/compress.cpp
  src/app/profiler_view.cpp
  src/assets/meshes.cpp
  src/assets/animation.cpp
  src/assets/textures.cpp
  src/assets/bc_codec.cpp
  src/assets/cooking.cpp
  src/assets/asset_db.cpp
  src/assets/gltf.cpp
  src/scene/behaviour.cpp
  src/scene/input_bindings.cpp
  src/scene/scene.cpp
  src/scene/project_templates.cpp
  src/scene/terrain.cpp
  src/scene/water.cpp
  src/scene/serialization.cpp
  src/scene/prefab_sync.cpp
  src/scene/scene_diff.cpp
  src/scene/stress_scene.cpp
  src/physics/physics.cpp
  src/physics/soft_body.cpp
  src/physics/fracture.cpp
  src/audio/audio.cpp
  src/audio/miniaudio_impl.c
  src/game/input.cpp
  src/game/game.cpp
  src/game/builtin_behaviours.cpp
  src/game/behaviour_tree.cpp
  src/game/module.cpp
  src/game/ui.cpp
  src/game/prompts.cpp
  src/game/text_shaping.cpp
  src/game/glyph_font.cpp
  src/game/color_glyphs.cpp
  src/game/line_break.cpp
  src/game/ui_layout.cpp
  src/game/animation.cpp
  src/game/camera_shake.cpp
  src/game/particles.cpp
  src/game/navigation.cpp
  src/game/scene_loader.cpp
  src/game/ragdoll.cpp
  src/render/renderer.cpp
  src/render/renderer_textures.cpp
  src/render/renderer_instances.cpp
  src/render/renderer_post.cpp
  src/render/renderer_shadows.cpp
  src/render/scene_draw.cpp
  src/render/environment.cpp
  src/render/light_clusters.cpp
  src/render/atmosphere.cpp
  src/render/decals.cpp
  src/render/renderer_sky.cpp
  src/render/renderer_decals.cpp
  src/render/renderer_trace.cpp
  src/assets/mesh_lod.cpp
  src/assets/retarget.cpp
  src/render/renderer_lights.cpp
  src/render/renderer_environment.cpp
  src/render/renderer_probes.cpp
  src/render/renderer_taa.cpp
  src/render/renderer_cull.cpp
  src/render/renderer_views.cpp
  src/render/renderer_gi.cpp
  src/render/texture_streaming.cpp
  src/render/shadow_cascades.cpp
  src/render/renderer_fog.cpp
  src/render/volumetric_fog.cpp
  src/render/display.cpp
  src/app/application.cpp
  src/app/export.cpp
  src/app/updates.cpp
  src/app/crash_reports.cpp
  src/app/exe_metadata.cpp
  src/script/lua_script.cpp
  src/script/lua_debugger.cpp
  src/net/udp.cpp
  src/net/tcp.cpp
  src/net/net_session.cpp
  src/net/relay.cpp
  src/net/crypto.cpp
  src/platform/hid_pads.cpp
  ${LUA_SOURCES})
if(WIN32)
  list(APPEND ENGINE_SOURCES src/platform/platform_win32.cpp)
elseif(APPLE)
  list(APPEND ENGINE_SOURCES src/platform/platform_macos.mm)
else()
  list(APPEND ENGINE_SOURCES src/platform/platform_linux.cpp src/platform/platform_xcb.cpp src/platform/imgui_impl_xcb.cpp)
  # Native Wayland too, when its headers are there (libwayland-dev, libxkbcommon-dev); the protocols' code is
  # generated already (third_party/wayland-protocols) and the libraries are loaded at run time (wayland_api.h), so
  # programs still start without them. Without the headers: X11 / XWayland only.
  option(VKWORLD_WAYLAND "Linux: a native Wayland window on Wayland sessions" ON)
  if(VKWORLD_WAYLAND)
    find_path(WAYLAND_CLIENT_INCLUDE_DIR wayland-client.h)
    find_path(WAYLAND_CURSOR_INCLUDE_DIR wayland-cursor.h)
    find_path(XKBCOMMON_INCLUDE_DIR xkbcommon/xkbcommon.h)
    if(WAYLAND_CLIENT_INCLUDE_DIR AND WAYLAND_CURSOR_INCLUDE_DIR AND XKBCOMMON_INCLUDE_DIR)
      file(GLOB WAYLAND_PROTOCOL_SOURCES ${CMAKE_SOURCE_DIR}/third_party/wayland-protocols/*.c)
      list(APPEND ENGINE_SOURCES src/platform/platform_wayland.cpp src/platform/wayland_api.cpp ${WAYLAND_PROTOCOL_SOURCES})
      set(VKWORLD_HAVE_WAYLAND ON)
      message(STATUS "Wayland: native windows (headers in ${WAYLAND_CLIENT_INCLUDE_DIR})")
    else()
      message(STATUS "Wayland: headers not found (install libwayland-dev libxkbcommon-dev): X11 only")
    endif()
  endif()
endif()
# miniaudio v0.11.21 (third-party code: no warnings from it)
set_source_files_properties(src/audio/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
find_package(Threads REQUIRED)
option(VKWORLD_STANDALONE_GAMES "Also build each game module into a standalone executable (static engine)" OFF)

function(vkworld_engine_target name type)
  add_library(${name} ${type} ${ENGINE_SOURCES})
  target_include_directories(${name} PUBLIC ${CMAKE_SOURCE_DIR}/src)
  # Vendored single-header libraries (implementations compiled in assets/): stb_image v2.30, cgltf v1.14
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb ${CMAKE_SOURCE_DIR}/third_party/cgltf
                                             ${CMAKE_SOURCE_DIR}/third_party/miniaudio ${CMAKE_SOURCE_DIR}/third_party/lua/src)
  target_link_libraries(${name} PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  # The ImGui objects go into the engine (PRIVATE: users get them through it, not a second copy); its headers are public
  target_link_libraries(${name} PRIVATE imgui Jolt harfbuzz)
  target_include_directories(${name} PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUIZMO_DIR})
  target_link_libraries(${name} PUBLIC Vulkan::Vulkan)
  if(WIN32)
    if(NOT MSVC)
      target_link_libraries(${name} PRIVATE gdi32 dwmapi)   # MSVC gets these via #pragma comment
    endif()
    target_link_libraries(${name} PUBLIC comdlg32 shell32 ole32 ws2_32 winhttp dxgi bcrypt hid setupapi)
    target_compile_definitions(${name} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  elseif(APPLE)
    target_link_libraries(${name} PRIVATE "-framework Cocoa" "-framework GameController" "-framework CoreHaptics")
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(${name} PUBLIC "-framework Cocoa" "-framework QuartzCore" "-framework UniformTypeIdentifiers"
                                         "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")
  else()
    target_link_libraries(${name} PUBLIC ${XCB_LIBRARY} m)
    if(VKWORLD_HAVE_WAYLAND)
      target_compile_definitions(${name} PRIVATE VKWORLD_WAYLAND=1)
      target_include_directories(${name} PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIR} ${WAYLAND_CURSOR_INCLUDE_DIR}
                                                 ${XKBCOMMON_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/third_party/wayland-protocols)
    endif()
  endif()
  add_dependencies(${name} shaders build_info)
endfunction()

if(NOT WIN32 AND NOT APPLE)
  find_library(XCB_LIBRARY xcb REQUIRED)
endif()
if(APPLE)
  enable_language(OBJCXX)
endif()
vkworld_engine_target(engine SHARED)
# Windows: export every function, like a Unix shared library does (data symbols aren't: the engine keeps none in headers)
set_target_properties(engine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(MSVC)   # load the Vulkan loader on first use: programs that never render (the tests) run without one installed
  target_link_options(engine PRIVATE /DELAYLOAD:vulkan-1.dll)
  target_link_libraries(engine PRIVATE delayimp)
endif()
if(VKWORLD_STANDALONE_GAMES)
  vkworld_engine_target(engine_static STATIC)
endif()

# Editor: the application built on top of the engine
add_executable(vkworld
  src/main.cpp
  src/editor/editor.cpp
  src/editor/editor_strings.cpp
  src/editor/editor_panels.cpp
  src/editor/editor_outliner.cpp
  src/editor/editor_animation.cpp
  src/editor/editor_inspector.cpp
  src/editor/editor_viewport.cpp
  src/editor/editor_assets.cpp
  src/editor/editor_asset_db.cpp
  src/editor/editor_terrain.cpp
  src/editor/editor_ai.cpp
  src/editor/editor_ui_layout.cpp
  src/editor/editor_project.cpp
  src/editor/editor_prefs.cpp)
target_link_libraries(vkworld PRIVATE engine)
# Help > Check for updates asks here (GitHub's latest release of the engine; only when asked)
set(VKWORLD_UPDATE_URL "https://api.github.com/repos/mikelmiras/vulkan_demo/releases/latest" CACHE STRING
    "Where the editor's Help > Check for updates looks")
target_compile_definitions(vkworld PRIVATE VKWORLD_UPDATE_URL="${VKWORLD_UPDATE_URL}")
# For game modules built along with the engine: "{engineBuild}" in a project's moduleBuild command
target_compile_definitions(vkworld PRIVATE VKWORLD_BUILD_DIR="${CMAKE_BINARY_DIR}")

# Standalone game: the engine without the editor, playing a scene
add_executable(vkgame src/runtime/main.cpp)
target_link_libraries(vkgame PRIVATE engine)

# The relay server for lobbies and NAT traversal (net/relay.h): standalone, without the engine (runs on any server)
add_executable(vkrelay src/relay/main.cpp src/net/relay.cpp src/net/udp.cpp src/net/crypto.cpp)
target_include_directories(vkrelay PRIVATE src)
if(WIN32)
  target_link_libraries(vkrelay PRIVATE ws2_32 bcrypt)
endif()

# Benchmark: times the CPU side of a frame on a large generated scene (engine_bench [statics] [bodies] [triggers] [ticks])
add_executable(engine_bench bench/bench_main.cpp)
target_link_libraries(engine_bench PRIVATE engine)

# Platform layer: the ImGui backends (the engine's own platform files are in ENGINE_SOURCES)
if(WIN32)
  target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
elseif(APPLE)
  target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_osx.mm)
  target_compile_options(imgui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

# Game modules: a project's own behaviours as a shared library that the editor and vkgame load at run time (see
# src/game/module.h). The library goes into <PROJECT_DIR>/bin, where the project's "module" setting points.
function(vkworld_add_game_module target)
  cmake_parse_arguments(ARG "" "PROJECT_DIR" "SOURCES" ${ARGN})
  add_library(${target} MODULE ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE engine)
  set_target_properties(${target} PROPERTIES PREFIX ""
                        LIBRARY_OUTPUT_DIRECTORY ${ARG_PROJECT_DIR}/bin RUNTIME_OUTPUT_DIRECTORY ${ARG_PROJECT_DIR}/bin)
  foreach(config ${CMAKE_CONFIGURATION_TYPES})   # multi-config generators: the same folder for every config
    string(TOUPPER ${config} CONFIG)
    set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${CONFIG} ${ARG_PROJECT_DIR}/bin
                          RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${ARG_PROJECT_DIR}/bin)
  endforeach()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  endif()
  # The game as one executable: vkgame with the module's code compiled in and the engine linked statically
  # (<PROJECT_DIR>/bin/<target>_game; File > Export game uses it when it's there)
  if(VKWORLD_STANDALONE_GAMES)
    add_executable(${target}_game ${CMAKE_SOURCE_DIR}/src/runtime/main.cpp ${ARG_SOURCES})
    target_link_libraries(${target}_game PRIVATE engine_static)
    target_compile_definitions(${target}_game PRIVATE VKWORLD_STANDALONE_MODULE)
    if(MSVC)   # (like the engine DLL: starts without a Vulkan runtime, so --check works anywhere)
      target_link_options(${target}_game PRIVATE /DELAYLOAD:vulkan-1.dll)
      target_link_libraries(${target}_game PRIVATE delayimp)
    endif()
    set_target_properties(${target}_game PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${ARG_PROJECT_DIR}/bin)
    foreach(config ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER ${config} CONFIG)
      set_target_properties(${target}_game PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CONFIG} ${ARG_PROJECT_DIR}/bin)
    endforeach()
  endif()
endfunction()

# The sample game (examples/sample_game) and any other game projects to build with the engine
# (-DVKWORLD_GAME_PROJECTS="/path/to/game;..."; each folder has a CMakeLists.txt calling vkworld_add_game_module)
option(VKWORLD_BUILD_EXAMPLES "Build the sample game's module" ON)
set(VKWORLD_GAME_PROJECTS "" CACHE STRING "Game project folders whose modules are built with the engine")
if(VKWORLD_BUILD_EXAMPLES)
  add_subdirectory(examples/sample_game)
endif()
foreach(dir ${VKWORLD_GAME_PROJECTS})
  get_filename_component(name ${dir} NAME)
  add_subdirectory(${dir} ${CMAKE_BINARY_DIR}/games/${name})
endforeach()

# Tests: `make engine_tests && ctest` (or run ./engine_tests [name filter])
option(VKWORLD_BUILD_TESTS "Build the engine unit tests" ON)
if(VKWORLD_BUILD_TESTS)
  enable_testing()
  add_executable(engine_tests
    tests/test_main.cpp
    tests/test_editor_prefs.cpp
    tests/test_scripting.cpp
    tests/test_asset_db.cpp
    tests/test_terrain.cpp
    tests/test_environment.cpp
    tests/test_animation_ik.cpp
    tests/test_ui_layout.cpp
    tests/test_network.cpp
    tests/test_relay.cpp
    tests/test_crypto.cpp
    tests/test_render_scale.cpp
    tests/test_atmosphere.cpp
    tests/test_surfaces.cpp
    tests/test_gamepads.cpp
    tests/test_text_shaping.cpp
    tests/test_shipping.cpp
    src/editor/editor_prefs.cpp
    tests/test_core.cpp
    tests/test_assets.cpp
    tests/test_scene.cpp
    tests/test_game.cpp
    tests/test_physics.cpp
    tests/test_prefabs.cpp
    tests/test_rendering_assets.cpp
    tests/test_audio.cpp
    tests/test_project.cpp
    tests/test_templates.cpp
    tests/test_streaming.cpp
    tests/test_cooking.cpp
    tests/test_shadow_cascades.cpp
    tests/test_hdr_image.cpp
    tests/test_color_glyphs.cpp
    tests/test_line_break.cpp
    tests/test_volumetric_fog.cpp
    tests/test_display.cpp
    tests/test_module.cpp
    tests/test_scene_flow.cpp
    tests/test_ui.cpp
    tests/test_animation.cpp
    tests/test_formats.cpp
    tests/test_scene_diff.cpp
    tests/test_particles.cpp
    tests/test_navigation.cpp
    tests/test_loading.cpp
    tests/test_gameplay.cpp
    tests/test_joints.cpp
    tests/test_vehicles.cpp
    tests/test_reliability.cpp
    tests/test_lights.cpp
    tests/test_lod.cpp
    tests/test_ai.cpp
    tests/test_soft_fracture.cpp
    tests/test_retarget.cpp
    tests/test_lua_debugger.cpp)
  target_link_libraries(engine_tests PRIVATE engine)
  target_compile_definitions(engine_tests PRIVATE VKWORLD_TEST_ASSETS="${CMAKE_SOURCE_DIR}/tests/assets")
  if(VKWORLD_BUILD_EXAMPLES)   # the module tests load the sample game's module
    add_dependencies(engine_tests sample_game vkgame)   # (the export test packs vkgame)
    target_compile_definitions(engine_tests PRIVATE VKWORLD_SAMPLE_GAME="${CMAKE_SOURCE_DIR}/examples/sample_game")
  endif()
  add_test(NAME engine_tests COMMAND engine_tests)
endif()

# Warnings
set(OWN_TARGETS engine vkworld vkgame engine_bench)
if(VKWORLD_BUILD_TESTS)
  list(APPEND OWN_TARGETS engine_tests)
endif()
if(MSVC)
  target_compile_options(imgui PRIVATE /utf-8)
  foreach(t ${OWN_TARGETS})
    target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endforeach()
  set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT vkworld)
else()
  foreach(t ${OWN_TARGETS})
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  endforeach()
endif()
