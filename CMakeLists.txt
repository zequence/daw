cmake_minimum_required(VERSION 3.22)

project(OrchestralDAW VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(external/JUCE)

# zstd reference decoder (BSD licence, decompression only): Synchron Player
# state embeds zstd-compressed JSON (key ranges etc.).
add_library(zstd_decoder STATIC
    external/zstd/lib/common/debug.c
    external/zstd/lib/common/entropy_common.c
    external/zstd/lib/common/error_private.c
    external/zstd/lib/common/fse_decompress.c
    external/zstd/lib/common/xxhash.c
    external/zstd/lib/common/zstd_common.c
    external/zstd/lib/decompress/huf_decompress.c
    external/zstd/lib/decompress/zstd_ddict.c
    external/zstd/lib/decompress/zstd_decompress.c
    external/zstd/lib/decompress/zstd_decompress_block.c)
target_include_directories(zstd_decoder PUBLIC external/zstd/lib)
target_compile_definitions(zstd_decoder PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0)

juce_add_gui_app(OrchestralDAW
    PRODUCT_NAME "Daw+"
    COMPANY_NAME "Loitsut")

target_sources(OrchestralDAW PRIVATE
    src/Main.cpp
    src/AudioEngine.cpp
    src/MainComponent.cpp
    src/ui/TrackList.cpp
    src/ui/PianoRollView.cpp
    src/ui/ArrangementView.cpp
    src/ui/TimelineBar.cpp
    src/ui/NativeBusyWindow.cpp
    src/ui/SystemTitleBar.cpp
    src/api/CommandDispatcher.cpp
    src/model/TempoMap.cpp
    src/engine/Transport.cpp
    src/diagnostics/PerformanceMonitor.cpp
    src/diagnostics/PerformancePanel.cpp)

target_compile_definitions(OrchestralDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OrchestralDAW,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OrchestralDAW,JUCE_VERSION>")

# Bravura, the SMuFL reference music font (SIL Open Font License 1.1, resources/fonts/Bravura-OFL.txt):
# articulation symbols in the MIDI editor and the expression map editor
juce_add_binary_data(OrchestralDAWData
    HEADER_NAME AppBinaryData.h
    NAMESPACE AppBinaryData
    SOURCES resources/fonts/Bravura.otf)

target_link_libraries(OrchestralDAW
    PRIVATE
        OrchestralDAWData
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#==============================================================================
# Out-of-process plugin scanner. Writes the shared plugin cache in the user data dir.
juce_add_console_app(OrchestralDAWScanner
    PRODUCT_NAME "OrchestralDAWScanner")

target_sources(OrchestralDAWScanner PRIVATE
    src/scanner/ScannerMain.cpp
    src/diagnostics/PerformanceMonitor.cpp)

target_compile_definitions(OrchestralDAWScanner PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0)

target_link_libraries(OrchestralDAWScanner
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

#==============================================================================
# Unit tests (console app; returns non-zero on failure).
juce_add_console_app(OrchestralDAWTests
    PRODUCT_NAME "OrchestralDAWTests")

target_sources(OrchestralDAWTests PRIVATE
    tests/TempoMapTests.cpp
    tests/TransportTests.cpp
    tests/GraphIntegrationTests.cpp
    tests/RecordingTests.cpp
    tests/RouteTests.cpp
    tests/ProjectTests.cpp
    tests/EndToEndTests.cpp
    tests/ApiTests.cpp
    tests/ClipCommandTests.cpp
    tests/ArrangementTests.cpp
    tests/EventTests.cpp
    tests/ReplaceModeTests.cpp
    tests/HistoryTests.cpp
    tests/FolderTests.cpp
    tests/ThemeTests.cpp
    tests/ExpressionMapTests.cpp
    tests/ExpressionMapSlotTests.cpp
    tests/NoteNamesTests.cpp
    tests/SmuflTests.cpp
    tests/MidiMonitorTests.cpp
    tests/ExpressionMapEngineTests.cpp
    tests/ExpressionMapCommandTests.cpp
    tests/ExpressionMapLibraryTests.cpp
    tests/ExpressionMapHistoryTests.cpp
    tests/ArticulationMenuTests.cpp
    tests/WindowPlacementTests.cpp
    tests/PreRollTests.cpp
    tests/PlaybackSequenceTests.cpp
    tests/PlaybackWiringTests.cpp
    tests/VeproStateTests.cpp
    tests/KeyCommandTests.cpp
    tests/ControllerLaneTests.cpp
    src/AudioEngine.cpp
    src/api/CommandDispatcher.cpp
    src/model/TempoMap.cpp
    src/engine/Transport.cpp)

target_compile_definitions(OrchestralDAWTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_MODAL_LOOPS_PERMITTED=1)   # tests pump the message loop to let the graph rebuild

target_link_libraries(OrchestralDAWTests
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        OrchestralDAWData
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

#==============================================================================
# App version header, regenerated every build from git (commit count + hash,
# -dirty when the tree has uncommitted changes). app.status reports it.
add_custom_target(OrchestralDAWVersion
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/AppVersion.h
            -P ${CMAKE_SOURCE_DIR}/cmake/GenVersion.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/AppVersion.h)

foreach(target OrchestralDAW OrchestralDAWTests)
    target_link_libraries(${target} PRIVATE zstd_decoder)
    add_dependencies(${target} OrchestralDAWVersion)
    target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endforeach()

#==============================================================================
# Ship the scanner next to the app so the app can launch it.
add_dependencies(OrchestralDAW OrchestralDAWScanner)
add_custom_command(TARGET OrchestralDAW POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OrchestralDAWScanner> $<TARGET_FILE_DIR:OrchestralDAW>)
