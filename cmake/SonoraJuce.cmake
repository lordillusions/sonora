# Builds every JUCE module we use ONCE into a static library `sonora::juce`.
# All Sonora libraries/executables link this instead of individual juce:: modules,
# which avoids compiling JUCE several times and duplicate-symbol errors.
#
# Feature switches for JUCE live here (plugin hosting formats, etc.).

add_library(sonora_juce STATIC)
add_library(sonora::juce ALIAS sonora_juce)

target_link_libraries(sonora_juce
    PRIVATE
        juce::juce_core
        juce::juce_events
        juce::juce_data_structures
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Per-platform switches (COM-7, docs/PLATFORMS.md): Audio Units exist only on macOS; Linux needs libcurl
# for HTTPS in juce::URL (macOS uses NSURLSession, Windows WinINet).
# MP3: macOS decodes it through CoreAudio; elsewhere JUCE's own decoder (the MP3 patents have expired).
if(APPLE)
    set(_sonora_juce_au 1)
    set(_sonora_juce_mp3 0)
else()
    set(_sonora_juce_au 0)
    set(_sonora_juce_mp3 1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_sonora_juce_curl 1)
    find_package(CURL REQUIRED)
    target_link_libraries(sonora_juce PUBLIC CURL::libcurl)
else()
    set(_sonora_juce_curl 0)
endif()

# PLG-15: VST2 hosting through FST's clean-room headers (scripts/bootstrap.sh fetches them; docs/adr/0003-plugin-format.md).
# JUCE includes <pluginterfaces/vst2.x/aeffect.h>; src/plugins/vst2/ maps that path onto FST.
set(_sonora_fst_dir "${SONORA_THIRD_PARTY_DIR}/fst")
set(_sonora_fst_patched FALSE)
if(EXISTS "${_sonora_fst_dir}/fst/fst.h")
    file(STRINGS "${_sonora_fst_dir}/fst/fst.h" _sonora_fst_marker REGEX "Sonora PLG-15")
    if(_sonora_fst_marker) # src/plugins/vst2/fst-host-layout.patch applied (scripts/bootstrap.sh)
        set(_sonora_fst_patched TRUE)
    endif()
endif()
if(_sonora_fst_patched)
    set(_sonora_juce_vst2 1)
    target_include_directories(sonora_juce PRIVATE "${CMAKE_SOURCE_DIR}/src/plugins/vst2" "${_sonora_fst_dir}")
else()
    set(_sonora_juce_vst2 0)
    message(STATUS "Patched FST headers not found in ${_sonora_fst_dir}: VST2 hosting is off (run scripts/bootstrap.sh)")
endif()

target_compile_definitions(sonora_juce
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=${_sonora_juce_curl}
        JUCE_PLUGINHOST_VST3=1      # chosen format, see docs/adr/0003-plugin-format.md
        JUCE_PLUGINHOST_AU=${_sonora_juce_au}  # cheap to keep on macOS; UI exposes it later
        JUCE_USE_MP3AUDIOFORMAT=${_sonora_juce_mp3}
        JUCE_PLUGINHOST_VST=${_sonora_juce_vst2}  # VST2 via FST headers (PLG-15)
        JUCE_PLUGINHOST_LV2=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_SILENCE_XCODE_15_LINKER_WARNING=1
    INTERFACE
        $<TARGET_PROPERTY:sonora_juce,COMPILE_DEFINITIONS>)

if(MSVC)
    # M_PI and friends (used throughout the DSP code) need this before <cmath> on MSVC.
    target_compile_definitions(sonora_juce PUBLIC _USE_MATH_DEFINES NOMINMAX)
endif()

target_include_directories(sonora_juce
    INTERFACE
        $<TARGET_PROPERTY:sonora_juce,INCLUDE_DIRECTORIES>)

set_target_properties(sonora_juce PROPERTIES
    POSITION_INDEPENDENT_CODE TRUE
    VISIBILITY_INLINES_HIDDEN TRUE
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)
