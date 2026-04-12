cmake_minimum_required(VERSION 3.22)
project(TsimafeiPlugin VERSION 1.0.0)

# JUCE is expected at ../../JUCE relative to this file (matching the .jucer paths).
# Override by passing -DJUCE_PATH=/path/to/JUCE to cmake.
if(NOT DEFINED JUCE_PATH)
    set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../JUCE")
endif()
add_subdirectory(${JUCE_PATH} JUCE_build)

juce_add_plugin(TsimafeiPlugin
    PRODUCT_NAME        "Tsimafei 187719"
    COMPANY_NAME        "yourcompany"
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE         Pqui
    FORMATS             VST3
    IS_SYNTH            FALSE
    NEEDS_MIDI_INPUT    TRUE
    NEEDS_MIDI_OUTPUT   TRUE
    IS_MIDI_EFFECT      FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES     "Fx"
)

target_sources(TsimafeiPlugin PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

# PluginProcessor.h uses #include <JuceHeader.h> from the Projucer-generated
# JuceLibraryCode directory. Make it findable under the CMake build as well.
target_include_directories(TsimafeiPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/JuceLibraryCode
)

target_compile_definitions(TsimafeiPlugin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_DISPLAY_SPLASH_SCREEN=1
)

target_link_libraries(TsimafeiPlugin PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_plugin_client
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# shm_open / shm_unlink live in librt on Linux (not needed on macOS or Windows).
if (UNIX AND NOT APPLE)
    target_link_libraries(TsimafeiPlugin PRIVATE rt)
endif()
