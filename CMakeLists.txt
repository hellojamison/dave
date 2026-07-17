cmake_minimum_required(VERSION 3.25...4.0)

project(Dave
    VERSION 0.1.0
    DESCRIPTION "D.A.V.E. — Digital Audio & Video Environment"
    LANGUAGES C CXX)

# ─── Build config ───────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match JUCE's default of hiding symbols and exporting only what's needed.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Where to find JUCE. We vendor JUCE directly at a stable tag rather than
# relying on tracktion_engine's nested develop-tracking submodule.
set(JUCE_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE")

# ─── JUCE ───────────────────────────────────────────────────────────────────
add_subdirectory(${JUCE_ROOT_DIR} ${CMAKE_BINARY_DIR}/juce_build EXCLUDE_FROM_ALL)

# ─── Tracktion Engine (as JUCE modules) ─────────────────────────────────────
# v3.2.0 ships tracktion_engine, tracktion_core, tracktion_graph as modules.
# Phase 0 does not consume Tracktion yet — we only prove our own RT graph with
# a SineNode. Tracktion is wired in Phase 1 when the engine drives real clips.
# Kept here so a single flag (DAVE_USE_TRACKTION) flips it on without surgery.
option(DAVE_USE_TRACKTION "Link the Tracktion Engine modules (Phase 1+)" OFF)
if(DAVE_USE_TRACKTION)
    juce_add_module("${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracktion_engine/modules/tracktion_engine")
    juce_add_module("${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracktion_engine/modules/tracktion_core")
    juce_add_module("${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracktion_engine/modules/tracktion_graph")
endif()

# ─── Dave application ───────────────────────────────────────────────────────
file(GLOB_RECURSE DAVE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm")

juce_add_gui_app(Dave
    # VERSION / COMPANY / etc. metadata
    PRODUCT_NAME "Dave"
    COMPANY_NAME "Jamison Rabbe"
    BUNDLE_ID "com.takeoneaudio.dave"
    ICON_BIG  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icon.png")

target_sources(Dave PRIVATE ${DAVE_SOURCES})

# Our headers include each other as "engine/graph/Node.h", "application/...", etc.
# So the include root is src/. JUCE headers are pulled in via the juce_* targets.
target_include_directories(Dave PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(Dave PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="Dave"
    JUCE_APPLICATION_VERSION_STRING="0.1.0")

target_link_libraries(Dave PRIVATE
    # JUCE modules used by Phase 0: audio device I/O + GUI.
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(DAVE_USE_TRACKTION)
    target_link_libraries(Dave PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_extra
        juce::juce_opengl
        juce::juce_osc
        tracktion_core
        tracktion_engine
        tracktion_graph)
endif()

# ─── Headless engine test target (Phase 1+) ─────────────────────────────────
# Placeholder; wiring the Catch2 harness is a later phase.
