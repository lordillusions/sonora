# Command-line tools. They are the fastest way for a human or an LLM to verify work
# without clicking around the GUI.

add_executable(als_dump als_dump/main.cpp)
target_link_libraries(als_dump PRIVATE sonora_engine) # devices: the Sonora VST3 state encoder (ALS-15)

if(SONORA_WITH_UI)
add_executable(ui_snapshot ui_snapshot/main.cpp)
target_link_libraries(ui_snapshot PRIVATE sonora_ui)
# LIB-1 --browser-packs reads installed packs (no network: not sonora_packs, whose JUCE link order clashes with Skia's HarfBuzz).
target_sources(ui_snapshot PRIVATE ../src/packs/PackManifest.cpp ../src/packs/PackLibrary.cpp)
target_compile_definitions(ui_snapshot PRIVATE
    SONORA_DEFAULT_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/minimal_set.xml")
endif()

add_executable(plugin_probe plugin_probe/main.cpp)
target_link_libraries(plugin_probe PRIVATE sonora_engine)

add_executable(native_wavetable_probe native_wavetable_probe/main.cpp)
target_link_libraries(native_wavetable_probe PRIVATE sonora_engine)

add_executable(native_effects_probe native_effects_probe/main.cpp)
target_link_libraries(native_effects_probe PRIVATE sonora_engine)

add_executable(native_fx_probe native_fx_probe/main.cpp)
target_link_libraries(native_fx_probe PRIVATE sonora_engine)

add_executable(parity_probe parity_probe/main.cpp)
target_link_libraries(parity_probe PRIVATE sonora_engine)

add_executable(engine_probe engine_probe/main.cpp)
target_link_libraries(engine_probe PRIVATE sonora_engine)

add_executable(resynth_probe resynth_probe/main.cpp)
target_link_libraries(resynth_probe PRIVATE sonora_engine)

add_executable(recording_probe recording_probe/main.cpp)
target_link_libraries(recording_probe PRIVATE sonora_engine)

add_executable(midi_input_probe midi_input_probe/main.cpp)
target_link_libraries(midi_input_probe PRIVATE sonora_juce)

# control_probe pumps the CoreFoundation run loop and drives the AppKit dock: macOS only (TODO(COM-7)).
if(APPLE)
    add_executable(control_probe control_probe/main.cpp)
    target_link_libraries(control_probe PRIVATE sonora_agent)
    if(SONORA_WITH_UI)
        target_sources(control_probe PRIVATE ../src/app/AssistantDock.mm)
        target_link_libraries(control_probe PRIVATE sonora_ui)
        target_compile_options(control_probe PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
        target_compile_definitions(control_probe PRIVATE SONORA_PROBE_UI=1)
    else()
        target_compile_definitions(control_probe PRIVATE SONORA_PROBE_UI=0)
    endif()
endif()

if(SONORA_WITH_UI)
add_executable(brand_render brand_render/main.cpp)
target_link_libraries(brand_render PRIVATE sonora_ui)
endif()

# COL-3: a headless collab session over the real network (local relay and LAN).
add_executable(collab_probe collab_probe/main.cpp)
target_link_libraries(collab_probe PRIVATE sonora_collab)
target_compile_definitions(collab_probe PRIVATE SONORA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# --chat-dock captures the native Session Chat dock (COL-9): macOS with the UI only.
if(APPLE AND SONORA_WITH_UI)
    target_sources(collab_probe PRIVATE ../src/app/CollabChatDock.mm)
    target_link_libraries(collab_probe PRIVATE sonora_ui)
    target_compile_options(collab_probe PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(collab_probe PRIVATE SONORA_PROBE_CHAT_DOCK=1)
endif()

# LIB-1: build/sign sample packs and drive the app's PackStore against a feed (docs/SAMPLE_PACKS.md).
add_executable(pack_tool pack_tool/main.cpp)
target_link_libraries(pack_tool PRIVATE sonora_packs) # Monocypher (signing) via sonora_core

# PLG-14: render any effect plug-in over a WAV with named parameter settings (black-box measurements, A/B renders).
add_executable(fx_render fx_render/main.cpp)
target_link_libraries(fx_render PRIVATE sonora_juce)

# --bench reads usrsctp's counters (retransmissions, timeouts) when the collab network is built in.
if (TARGET usrsctp)
    target_include_directories(collab_probe SYSTEM PRIVATE $<TARGET_PROPERTY:usrsctp,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(collab_probe PRIVATE SONORA_PROBE_SCTP_STATS=1)
endif()
