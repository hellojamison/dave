cmake_minimum_required(VERSION 3.25...4.0)

project(Dave
    VERSION 0.1.0
    DESCRIPTION "D.A.V.E. — Digital Audio & Video Environment"
    LANGUAGES C CXX OBJC)  # OBJC needed for nativefiledialog's macOS panels

# ─── Build config ───────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(DAVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(THIRD_PARTY "${DAVE_ROOT}/third_party")

# ─── GLFW (windowing + GL context) ──────────────────────────────────────────
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${THIRD_PARTY}/glfw" "${CMAKE_BINARY_DIR}/glfw" EXCLUDE_FROM_ALL)

# ─── glad (OpenGL function loader, generated sources) ───────────────────────
# glad sources are generated once (GL 3.3 core) and committed so the build
# doesn't need Python. See third_party/glad-generated/README.md.
add_library(glad STATIC "${THIRD_PARTY}/glad-generated/glad.c")
target_include_directories(glad PUBLIC "${THIRD_PARTY}/glad-generated")
# glad needs a GL loader backend; we use glfwGetProcAddress at runtime, so glad
# is built with its own loader enabled (the generated glad.c provides
# gladLoadGLLoader).

# ─── ImGui (compile sources directly; docking branch) ───────────────────────
add_library(imgui STATIC
    "${THIRD_PARTY}/imgui/imgui.cpp"
    "${THIRD_PARTY}/imgui/imgui_draw.cpp"
    "${THIRD_PARTY}/imgui/imgui_tables.cpp"
    "${THIRD_PARTY}/imgui/imgui_widgets.cpp"
    "${THIRD_PARTY}/imgui/imgui_demo.cpp"
    # GL3 + GLFW backends
    "${THIRD_PARTY}/imgui/backends/imgui_impl_glfw.cpp"
    "${THIRD_PARTY}/imgui/backends/imgui_impl_opengl3.cpp")
target_include_directories(imgui PUBLIC
    "${THIRD_PARTY}/imgui"
    "${THIRD_PARTY}/imgui/backends")
target_link_libraries(imgui PUBLIC glfw)

# ─── nativefiledialog-extended (file open/save dialogs) ─────────────────────
set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${THIRD_PARTY}/nfd" "${CMAKE_BINARY_DIR}/nfd" EXCLUDE_FROM_ALL)

# ─── VST3 SDK (plugin HOSTING; MIT since Oct 2025) ──────────────────────────
# We only need the hosting side (we don't build plugins), so disable plugin
# examples + VSTGUI (which needs the vstgui4 submodule we didn't init).
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_AUV2_BUILDS OFF CACHE BOOL "" FORCE)
add_subdirectory("${THIRD_PARTY}/vst3sdk" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
# The SDK creates targets: base, pluginterfaces, sdk, sdk_hosting (and
# sdk_common). We link the hosting-side ones into Dave.
# On macOS the hosting module loader (module_mac.mm) needs ARC + CoreFoundation.

# ─── Dave application ───────────────────────────────────────────────────────
file(GLOB_RECURSE DAVE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

# On macOS, build as a .app bundle with Info.plist + icon.
add_executable(Dave ${DAVE_SOURCES})
if(APPLE)
    set_target_properties(Dave PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in")
    # The Info.plist template uses @DAVE_VERSION@.
    set_target_properties(Dave PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER "com.takeoneaudio.dave")
endif()

target_include_directories(Dave PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${THIRD_PARTY}/miniaudio"                      # miniaudio.h + dr_wav.h
    "${THIRD_PARTY}/nlohmann_json/single_include")  # nlohmann/json.hpp

# The VST3 platform module loader (module_mac.mm / module_linux.cpp /
# module_win32.cpp) is NOT part of the sdk_hosting library — each host app
# compiles it directly (see the SDK's audiohost/validator samples). It loads
# .vst3 bundles via dlopen and needs ARC on macOS.
if(APPLE)
    target_sources(Dave PRIVATE
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm"
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/common/memorystream.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/plugins/PluginEditor_mac.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/MacMenuBar.mm")
    set_source_files_properties(
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/plugins/PluginEditor_mac.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/MacMenuBar.mm" PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
elseif(WIN32)
    target_sources(Dave PRIVATE
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/module_win32.cpp")
endif()

target_link_libraries(Dave PRIVATE
    imgui
    glfw
    glad
    nfd
    # VST3 hosting (plugin host). These targets come from the vst3sdk
    # add_subdirectory above.
    base
    pluginterfaces
    sdk
    sdk_common
    sdk_hosting)

if(APPLE)
    # miniaudio uses CoreAudio/AudioToolbox; OpenGL is the system framework
    # (deprecated by Apple but functional for our ImGui backend); NFD uses
    # AppKit for the native open/save panels.
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    find_library(OPENGL_LIB OpenGL REQUIRED)
    find_library(APPKIT_LIB AppKit REQUIRED)
    find_library(FOUNDATION_LIB Foundation REQUIRED)
    target_link_libraries(Dave PRIVATE
        ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB} ${COREFOUNDATION_LIB}
        ${OPENGL_LIB} ${APPKIT_LIB} ${FOUNDATION_LIB})
endif()

if(WIN32)
    target_link_libraries(Dave PRIVATE ole32 winmm)
endif()

target_compile_definitions(Dave PRIVATE
    DAVE_VERSION="0.1.0")

# ─── macOS .app post-build: copy bundled ffmpeg into Helpers/ ───────────────
# The LGPL ffmpeg binary lives in third_party/ffmpeg-lgpl/bin. In a packaged
# .app it goes in Contents/Helpers/ so the VideoDecoder can find it at runtime
# (resolveVideoTools checks Contents/Helpers/ffmpeg).
if(APPLE)
    set(FFMPEG_BIN "${THIRD_PARTY}/ffmpeg-lgpl/bin/ffmpeg")
    set(FFPROBE_BIN "${THIRD_PARTY}/ffmpeg-lgpl/bin/ffprobe")
    add_custom_command(TARGET Dave POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:Dave>/../Helpers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_BIN}" "$<TARGET_FILE_DIR:Dave>/../Helpers/ffmpeg"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFPROBE_BIN}" "$<TARGET_FILE_DIR:Dave>/../Helpers/ffprobe"
        COMMENT "Copying LGPL ffmpeg/ffprobe into .app bundle"
        VERBATIM)
endif()

# ─── Permissive-license enforcement ─────────────────────────────────────────
# Dave is dual-licensed (GPL-3.0+ now, commercial option preserved). All deps
# above are permissive (MIT/Zlib/Unlicense/public domain). Do NOT add GPL/AGPL
# deps (JUCE, Tracktion, etc.) — see docs/architecture.md.

# ─── Test: plugin host end-to-end (sine through a VST3, measure output) ─────
add_executable(plugin_test tools/plugin_test.cpp
    src/engine/plugins/PluginHost.cpp
    src/engine/plugins/PluginInstance.cpp
    "${THIRD_PARTY}/vst3sdk/public.sdk/source/common/memorystream.cpp")
target_include_directories(plugin_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(plugin_test PRIVATE base pluginterfaces sdk sdk_common sdk_hosting)
if(APPLE)
    target_sources(plugin_test PRIVATE
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm")
    set_source_files_properties(
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm" PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
    find_library(CF_LIB CoreFoundation REQUIRED)
    find_library(FOUNDATION_LIB Foundation REQUIRED)
    target_link_libraries(plugin_test PRIVATE ${CF_LIB} ${FOUNDATION_LIB})
endif()
