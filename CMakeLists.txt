cmake_minimum_required(VERSION 3.22)
project(SliderRhythmMidi VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED JUCE_DIR)
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/juce")
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.2 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

option(SLIDER_BUILD_STANDALONE_PREVIEW "Build a UI preview without host transport" OFF)
set(plugin_formats VST3)
if(APPLE)
    list(APPEND plugin_formats AU)
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(SLIDER_BUILD_STANDALONE_PREVIEW)
    list(APPEND plugin_formats Standalone)
endif()

juce_add_plugin(SliderRhythmMidi
    COMPANY_NAME "Slider Rhythm Lab"
    BUNDLE_ID "com.sliderrhythmlab.midiseq"
    PLUGIN_MANUFACTURER_CODE Slrh
    PLUGIN_CODE SlrM
    PRODUCT_NAME "Slider Rhythm MIDI"
    FORMATS ${plugin_formats}
    IS_SYNTH FALSE
    IS_MIDI_EFFECT TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    VST3_CATEGORIES Instrument
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SliderRhythmMidi PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(SliderRhythmMidi PRIVATE Source)
target_compile_definitions(SliderRhythmMidi PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SliderRhythmMidi PRIVATE juce::juce_audio_utils juce::juce_gui_extra)

option(SLIDER_BUILD_MIDI_LEARN_TESTS "Build MIDI Learn integration checks" OFF)
if(SLIDER_BUILD_MIDI_LEARN_TESTS)
    enable_testing()
    add_executable(SliderRhythmSequencerTests tests/SequencerEngineTests.cpp)
    target_compile_options(SliderRhythmSequencerTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
    add_test(NAME sequencer COMMAND SliderRhythmSequencerTests)
    add_executable(SliderRhythmMidiLearnTests tests/MidiLearnTests.cpp)
    target_include_directories(SliderRhythmMidiLearnTests PRIVATE Source)
    target_link_libraries(SliderRhythmMidiLearnTests PRIVATE SliderRhythmMidi juce::juce_audio_utils juce::juce_gui_extra)
    add_test(NAME midi-learn-and-presets COMMAND SliderRhythmMidiLearnTests)
endif()
