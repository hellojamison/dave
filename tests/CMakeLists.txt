# Unit tests. Deliberately built from selected sources rather than by linking
# the Dave target: Dave is a macOS .app bundle that opens a window and a
# CoreAudio device, neither of which belongs in a headless CI run.

add_subdirectory("${THIRD_PARTY}/Catch2" "${CMAKE_BINARY_DIR}/Catch2" EXCLUDE_FROM_ALL)

add_executable(dave_tests
    test_sha256.cpp
    test_graph.cpp
    test_peaks.cpp
    test_markers.cpp
    test_mixer.cpp
    dr_wav_impl.cpp
    # Sources under test.
    "${CMAKE_SOURCE_DIR}/src/document/Sha256.cpp"
    "${CMAKE_SOURCE_DIR}/src/document/Edit.cpp"
    "${CMAKE_SOURCE_DIR}/src/document/MarkerCsv.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/graph/Graph.cpp"
    # PeakCache lives in Timeline.cpp alongside the drawing code. The tests
    # only call PeakCache, but the translation unit references ImGui symbols,
    # so imgui is linked to satisfy them — no ImGui context is ever created.
    "${CMAKE_SOURCE_DIR}/src/gui/Timeline.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/Theme.cpp")

target_include_directories(dave_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${THIRD_PARTY}/miniaudio"
    "${THIRD_PARTY}/nlohmann_json/single_include")

target_link_libraries(dave_tests PRIVATE
    Catch2::Catch2WithMain
    imgui)

include(Catch)
catch_discover_tests(dave_tests)
