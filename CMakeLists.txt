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

# ─── Dave application ───────────────────────────────────────────────────────
file(GLOB_RECURSE DAVE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

add_executable(Dave ${DAVE_SOURCES})

target_include_directories(Dave PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${THIRD_PARTY}/miniaudio")  # miniaudio.h + dr_wav.h live here

target_link_libraries(Dave PRIVATE
    imgui
    glfw
    glad
    nfd)

if(APPLE)
    # miniaudio uses CoreAudio/AudioToolbox; OpenGL is the system framework
    # (deprecated by Apple but functional for our ImGui backend); NFD uses
    # AppKit for the native open/save panels.
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    find_library(OPENGL_LIB OpenGL REQUIRED)
    find_library(APPKIT_LIB AppKit REQUIRED)
    target_link_libraries(Dave PRIVATE
        ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB} ${COREFOUNDATION_LIB}
        ${OPENGL_LIB} ${APPKIT_LIB})
endif()

if(WIN32)
    target_link_libraries(Dave PRIVATE ole32 winmm)
endif()

if(WIN32)
    target_link_libraries(Dave PRIVATE ole32 winmm)
endif()

target_compile_definitions(Dave PRIVATE
    DAVE_VERSION="0.1.0")

# ─── Permissive-license enforcement ─────────────────────────────────────────
# Dave is dual-licensed (GPL-3.0+ now, commercial option preserved). All deps
# above are permissive (MIT/Zlib/Unlicense/public domain). Do NOT add GPL/AGPL
# deps (JUCE, Tracktion, etc.) — see docs/architecture.md.
