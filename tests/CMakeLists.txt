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
    test_clipmove.cpp
    test_smf.cpp
    test_mididoc.cpp
    test_instrument.cpp
    test_midigraph.cpp
    test_timelinedrag.cpp
    test_timelinelayout.cpp
    test_maineditorlayout.cpp
    test_trackcolor.cpp
    test_automation.cpp
    test_trackmeter.cpp
    test_transport.cpp
    test_spscqueue.cpp
    test_ringbuffer.cpp
    test_wavwriter.cpp
    test_diskwriter.cpp
    test_audioimport.cpp
    test_inputmeter.cpp
    test_audiodeviceconfig.cpp
    test_audiopreferences.cpp
    test_iopanel.cpp
    test_mixerstrip.cpp
    test_recordcontroller.cpp
    test_transportrecording.cpp
    test_committake.cpp
    test_recordingbundle.cpp
    test_routing.cpp
    dr_wav_impl.cpp
    # Sources under test.
    "${CMAKE_SOURCE_DIR}/src/document/Sha256.cpp"
    "${CMAKE_SOURCE_DIR}/src/application/AudioPreferences.cpp"
    "${CMAKE_SOURCE_DIR}/src/document/Edit.cpp"
    "${CMAKE_SOURCE_DIR}/src/document/MarkerCsv.cpp"
    "${CMAKE_SOURCE_DIR}/src/document/ProjectFile.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/midi/SmfReader.cpp"
    # InstrumentNode::process references PluginInstance, so the VST3 hosting
    # side comes with it (linked below). No plugin is ever loaded in a test:
    # the note-scheduling seams are exercised without an instance.
    "${CMAKE_SOURCE_DIR}/src/engine/nodes/InstrumentNode.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/GraphBuilder.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/plugins/PluginHost.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/plugins/PluginInstance.cpp"
    "${THIRD_PARTY}/vst3sdk/public.sdk/source/common/memorystream.cpp"
    "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/eventlist.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/graph/Graph.cpp"
    # PeakCache lives in Timeline.cpp alongside the drawing code. The tests
    # only call PeakCache, but the translation unit references ImGui symbols,
    # so imgui is linked to satisfy them — no ImGui context is ever created.
    "${CMAKE_SOURCE_DIR}/src/gui/Timeline.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/Mixer.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/Theme.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/IoPanel.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/RoutingViewModel.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/TrackColorPicker.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/record/WavWriter.cpp"
    "${CMAKE_SOURCE_DIR}/src/engine/record/DiskWriter.cpp")
target_sources(dave_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src/engine/record/RecordController.cpp")

target_include_directories(dave_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${THIRD_PARTY}/miniaudio"
    "${THIRD_PARTY}/nlohmann_json/single_include")

target_link_libraries(dave_tests PRIVATE
    Catch2::Catch2WithMain
    imgui
    base pluginterfaces sdk sdk_common sdk_hosting)

# The VST3 platform module loader isn't part of sdk_hosting — each host
# compiles it. PluginInstance references it even though no test loads a plugin.
if(APPLE)
    target_sources(dave_tests PRIVATE
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm")
    set_source_files_properties(
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm"
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    find_library(TEST_CF_LIB CoreFoundation REQUIRED)
    find_library(TEST_FOUNDATION_LIB Foundation REQUIRED)
    target_link_libraries(dave_tests PRIVATE ${TEST_CF_LIB} ${TEST_FOUNDATION_LIB})
elseif(WIN32)
    target_sources(dave_tests PRIVATE
        "${THIRD_PARTY}/vst3sdk/public.sdk/source/vst/hosting/module_win32.cpp")
endif()

include(Catch)
catch_discover_tests(dave_tests)
