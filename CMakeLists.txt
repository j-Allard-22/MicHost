cmake_minimum_required(VERSION 3.22)
project(MicHost VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(external/JUCE)

# VST2 hosting is enabled only when the (legally EOL, personal-use-only) VST2 SDK
# headers are present locally. They must never be committed or redistributed —
# see README.md. Without them the app still builds with VST3-only hosting.
set(MICHOST_VST2_SDK "${CMAKE_CURRENT_SOURCE_DIR}/sdks/vst2")
if(EXISTS "${MICHOST_VST2_SDK}/pluginterfaces/vst2.x/aeffect.h")
    juce_set_vst2_sdk_path("${MICHOST_VST2_SDK}")
    set(MICHOST_HAS_VST2 TRUE)
    message(STATUS "MicHost: VST2 hosting ENABLED")
else()
    set(MICHOST_HAS_VST2 FALSE)
    message(STATUS "MicHost: VST2 hosting disabled (no headers under sdks/vst2)")
endif()

juce_add_gui_app(MicHost
    PRODUCT_NAME "MicHost"
    COMPANY_NAME "Personal"
    MICROPHONE_PERMISSION_ENABLED TRUE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

juce_generate_juce_header(MicHost)

target_sources(MicHost PRIVATE
    Source/Main.cpp
    Source/AudioEngine.cpp
    Source/MainComponent.cpp
    Source/RateMatchedBridge.cpp)

target_compile_definitions(MicHost PRIVATE
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_ASIO=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=$<IF:$<BOOL:${MICHOST_HAS_VST2}>,1,0>
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_VST3_HOST_CROSS_PLATFORM_UID=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=0)

target_link_libraries(MicHost PRIVATE
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_recommended_config_flags)
