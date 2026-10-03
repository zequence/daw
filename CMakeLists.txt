cmake_minimum_required(VERSION 3.22)

project(OrchestralDAW VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(external/JUCE)

juce_add_gui_app(OrchestralDAW
    PRODUCT_NAME "Orchestral DAW"
    COMPANY_NAME "Loitsut")

target_sources(OrchestralDAW PRIVATE
    src/Main.cpp
    src/AudioEngine.cpp
    src/MainComponent.cpp
    src/TrackRow.cpp
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

target_link_libraries(OrchestralDAW
    PRIVATE
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
    tests/EndToEndTests.cpp
    src/AudioEngine.cpp
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
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

#==============================================================================
# Ship the scanner next to the app so the app can launch it.
add_dependencies(OrchestralDAW OrchestralDAWScanner)
add_custom_command(TARGET OrchestralDAW POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OrchestralDAWScanner> $<TARGET_FILE_DIR:OrchestralDAW>)
