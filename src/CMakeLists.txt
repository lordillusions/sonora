# Library layering (a lower layer must never include a higher one):
#
#   sonora_core   core/ io/ util/                      model + file formats (no audio, no UI)
#   sonora_engine engine/ devices/ plugins/ midi/      realtime audio, devices, plugin hosting, MIDI
#   sonora_ui     gfx/ ui/                             Skia rendering + widgets + views
#   Sonora        app/                                 the .app bundle (wires everything together)
#
#   platform/     small per-OS seams (COM-7, docs/PLATFORMS.md): header-only helpers used by every
#                 layer, plus sonora_platform (HTTP, secret storage) for the agent. *Mac.mm files are
#                 the macOS implementations; *Juce/*File/*Portable/*None/*Unsupported.cpp the others.
#
# See docs/ARCHITECTURE.md.

# ------------------------------------------------------------------ platform
if(APPLE)
    add_library(sonora_platform STATIC platform/HttpClientMac.mm platform/SecretStoreMac.mm)
    target_compile_options(sonora_platform PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(sonora_platform PUBLIC "-framework Security" "-framework Foundation" "-framework LocalAuthentication")
else()
    add_library(sonora_platform STATIC platform/HttpClientJuce.cpp platform/SecretStoreFile.cpp)
endif()
target_include_directories(sonora_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sonora_platform PUBLIC sonora::juce sonora_core) # Paths.h -> core/AppDataFolder (COM-4c folder name)

# ------------------------------------------------------------------ core
add_library(sonora_core STATIC
    core/Model.cpp
    core/ResynthModel.cpp
    core/Edit.cpp
    core/ClipOperations.cpp
    core/Op.cpp
    core/OpJson.cpp
    core/ModelCodec.cpp
    core/Sha256.cpp
    core/InputRouting.cpp
    core/DeviceCatalog.cpp
    core/LiveColors.cpp
    core/AppDataFolder.cpp
    io/als/AlsReader.cpp
    io/als/AlsWriter.cpp
    io/als/AlsTemplates.cpp
    io/als/AlsNewSet.cpp
    io/als/SampleDrop.cpp
    io/midi/MidiFileExport.cpp
)
target_include_directories(sonora_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sonora_core PUBLIC sonora::juce)
if(APPLE)
    target_link_libraries(sonora_core PUBLIC "-framework Security") # core/Sha256 randomBytes
elseif(WIN32)
    target_link_libraries(sonora_core PUBLIC bcrypt)
endif()

# ------------------------------------------------------------------ engine
add_library(sonora_engine STATIC
    dsp/Loudness.cpp
    dsp/Analyzers.cpp
    engine/MasterMonitor.cpp
    engine/Exporter.cpp
    engine/ClipRender.cpp
    engine/StripSilence.cpp
    engine/AudioCapture.cpp
    engine/AudioToMidi.cpp
    engine/TempoAnalysis.cpp
    engine/ResynthAnalysis.cpp
    engine/ResynthEdits.cpp
    engine/StemSeparator.cpp
    engine/AudioEngine.cpp
    engine/AudioClipPlayer.cpp
    engine/SampleExport.cpp
    engine/Automation.cpp
    engine/AutomationCapture.cpp
    engine/ClipLauncher.cpp
    engine/ControlRouter.cpp
    engine/EngineGraph.cpp
    engine/PeakCache.cpp
    engine/Recorder.cpp
    engine/RecordingManager.cpp
    engine/OverdubPreview.cpp
    engine/ClipPlayers.cpp
    engine/SampleCache.cpp
    engine/AudioFileFormats.cpp
    engine/stretch/Transients.cpp
    devices/DeviceFactory.cpp
    devices/RackProcessor.cpp
    devices/native/Utility.cpp
    devices/native/EqEight.cpp
    devices/native/Simpler.cpp
    devices/native/FmSynth.cpp
    devices/native/VectorSynth.cpp
    devices/native/Resynth.cpp
    devices/native/Impulse.cpp
    devices/native/Echo.cpp
    devices/native/WavetableSynth.cpp
    devices/native/WavetableModRouting.cpp
    devices/native/WavetableVoiceMode.cpp
    devices/native/WavetableData.cpp
    devices/native/WavetableDownsampler.cpp
    devices/native/NoiseSampleVoice.cpp
    dsp/PreparedSampleKernel.cpp
    devices/native/WavetableContent.cpp
    devices/native/OscillatorMedia.cpp
    devices/native/WavetableEffects.cpp
    devices/native/LfoShaper.cpp
    devices/native/CurveLfo.cpp
    devices/native/Compressor.cpp
    devices/native/Limiter.cpp
    devices/native/Bands.cpp
    devices/native/Delay.cpp
    devices/native/MidiNoteEffects.cpp
    devices/native/Arpeggiator.cpp
    devices/native/NoteLength.cpp
    devices/native/ClassicEffects.cpp
    devices/native/Redux.cpp
    devices/native/SwissArmyMeter.cpp
    devices/native/Tuner.cpp
    devices/native/Reverb.cpp
    devices/native/AutoFilter.cpp
    plugins/PluginHost.cpp
    plugins/PluginDevice.cpp
    plugins/Vst3PresetBuilder.cpp
    plugins/SerumState.cpp
    plugins/SerumPatch.cpp
    plugins/SerumLibrary.cpp
    plugins/PrismPreset.cpp
    plugins/SerumEffectsImport.cpp
    midi/MidiManager.cpp
    midi/MidiCapture.cpp
    midi/MappingStore.cpp
    midi/MidiClockOut.cpp
)
target_link_libraries(sonora_engine PUBLIC sonora_core sonora::zstd)

# DEV-18c / ENG-14d: everything that runs per sample on the audio thread is optimized in the default Debug app too.
# At -O0 the native devices alone (12 Reverbs, piano racks) overran a 512-sample callback on a real 47-track set
# that Release plays at 25 % load. Symbols, asserts and normal floating-point semantics are retained.
# Configure with -DSONORA_DEBUG_OPTIMIZE_AUDIO=OFF to single-step DSP code.
option(SONORA_DEBUG_OPTIMIZE_AUDIO "Compile audio-thread DSP with -O2 in Debug builds" ON)
if (SONORA_DEBUG_OPTIMIZE_AUDIO)
    get_target_property(_sonoraAudioSources sonora_engine SOURCES)
    list(FILTER _sonoraAudioSources INCLUDE REGEX
         "^(dsp/|devices/native/|devices/RackProcessor\\.cpp|engine/stretch/|engine/(AudioEngine|AudioClipPlayer|ClipPlayers|Automation|ResynthAnalysis)\\.cpp)")
    set_source_files_properties(${_sonoraAudioSources} PROPERTIES
        COMPILE_OPTIONS "$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:Clang,AppleClang,GNU>>:-O2>")
endif()

# Out-of-process plugin scanner (PLG-3). PluginHost looks for it next to the running
# executable, then in ../bin. The Sonora.app target copies it into Contents/MacOS.
add_executable(sonora_plugin_scanner plugins/scanner/main.cpp)
target_link_libraries(sonora_plugin_scanner PRIVATE sonora::juce)
set_target_properties(sonora_plugin_scanner PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_dependencies(sonora_engine sonora_plugin_scanner)

# ------------------------------------------------------------------ ui
add_library(sonora_control STATIC control/ControlService.cpp control/DeviceControl.cpp control/SerumLead.cpp control/HostedParameters.cpp)
if(WIN32)
    target_sources(sonora_control PRIVATE control/LocalEndpointUnsupported.cpp)
else()
    target_sources(sonora_control PRIVATE control/LocalEndpoint.cpp)
endif()
target_link_libraries(sonora_control PUBLIC sonora_engine)

# ------------------------------------------------------------------ collab (COL-2/3, ADR 0009/0010)
add_library(sonora_collab STATIC
    collab/Objects.cpp
    collab/Session.cpp
    collab/Protocol.cpp
    collab/Invite.cpp
    collab/Assets.cpp
    collab/Transport.cpp
    collab/CollabSession.cpp
    collab/Net.cpp
)
target_include_directories(sonora_collab PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sonora_collab PUBLIC sonora_engine PRIVATE sonora_collab_net)

# AGT-6a: provider framing, Keychain and cancellable HTTP stay outside the audio engine/UI.
file(READ "${CMAKE_SOURCE_DIR}/schemas/control/v1/request.schema.json" SONORA_CONTROL_SCHEMA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/schemas/control/v1/request.schema.json")
configure_file(agent/ControlSchema.h.in generated/ControlSchema.h @ONLY)
add_library(sonora_agent STATIC agent/AgentConfig.cpp agent/AgentSettings.cpp agent/AgentProtocol.cpp agent/AgentRunner.cpp agent/AgentHttp.cpp)
target_include_directories(sonora_agent PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(sonora_agent PUBLIC sonora_control sonora_platform)

if(NOT SONORA_WITH_UI)
    return()
endif()

add_library(sonora_ui STATIC
    gfx/SkiaCanvasComponent.cpp
    gfx/Fonts.cpp
    gfx/Brand.cpp
    ui/Theme.cpp
    ui/BrowserTags.cpp
    ui/Widget.cpp
    ui/RootWidget.cpp
    ui/Painter.cpp
    ui/AudioClipEdits.cpp
    ui/Controls.cpp
    ui/Waveform.cpp
    ui/widgets/Buttons.cpp
    ui/widgets/Knob.cpp
    ui/widgets/Fader.cpp
    ui/views/TopBar.cpp
    ui/views/BrowserPanel.cpp
    ui/views/SessionView.cpp
    ui/views/InputControls.cpp
    ui/views/AutomationEditor.cpp
    ui/views/ArrangementView.cpp
    ui/views/EffectPanels.cpp
    ui/views/PluginPanel.cpp
    ui/views/DetailView.cpp
    ui/views/MonitorPanel.cpp
    ui/views/NativeEditors.cpp
    ui/views/DeviceFace.cpp
    ui/views/ReferenceFace.cpp
    ui/views/EffectFaces.cpp
    ui/views/ResynthEditor.cpp
    ui/views/CurveLfoEditor.cpp
    ui/views/WavetableEditor.cpp
    ui/views/WavetableWindowView.cpp
    ui/views/WavetablePresetBrowser.cpp
    ui/views/WavetableModulationEditor.cpp
    ui/views/WavetableRoutingEditor.cpp
    ui/MidiClipEdits.cpp
    ui/GridModel.cpp
    ui/GridMenu.cpp
    ui/TrackMenu.cpp
    ui/Shortcuts.cpp
    ui/views/ClipControls.cpp
    ui/views/ClipEnvelopeEditor.cpp
    ui/views/MpeEditor.cpp
    ui/views/PianoRoll.cpp
    ui/views/SampleView.cpp
    ui/views/PreferencesPanel.cpp
    ui/views/ExportPanel.cpp
    ui/views/CollabPanel.cpp
    ui/CollabOverlay.cpp
    ui/views/StatusBar.cpp
    ui/views/MainLayout.cpp
)
if(APPLE)
    target_sources(sonora_ui PRIVATE gfx/MetalLayerView.mm)
else()
    target_sources(sonora_ui PRIVATE gfx/MetalRendererNone.cpp) # raster rendering, see SkiaCanvasComponent.h
endif()
target_link_libraries(sonora_ui PUBLIC sonora_engine sonora::skia)
target_compile_options(sonora_ui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

# ------------------------------------------------------------------ Sonora Prism as a VST3 (PLG-11)
# A plug-in needs JUCE compiled with its JucePlugin_* configuration, and sonora_juce is the app's. So the plug-in's
# shared-code library compiles JUCE again and the core/engine/ui sources with it (the linker keeps only what the
# instrument and its editor use). Build: cmake --build build --target SonoraPrism_VST3.
option(SONORA_BUILD_PRISM_VST3 "Build Sonora Prism as a VST3 instrument" ${APPLE})
if (SONORA_BUILD_PRISM_VST3)
    # Same version as the app (SONORA_APP_VERSION below; release builds pass it with -D). Installers read it back.
    set(_sonoraPrismVersion "${PROJECT_VERSION}")
    if (SONORA_APP_VERSION)
        set(_sonoraPrismVersion "${SONORA_APP_VERSION}")
    endif()
    juce_add_plugin(SonoraPrism
        PRODUCT_NAME "Sonora Prism"
        COMPANY_NAME "Sonora"
        BUNDLE_ID "org.sonora.prism"
        VERSION ${_sonoraPrismVersion}
        PLUGIN_MANUFACTURER_CODE Snra
        PLUGIN_CODE SnPr
        FORMATS VST3
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        VST3_CATEGORIES Instrument Synth
        COPY_PLUGIN_AFTER_BUILD FALSE)
    get_target_property(_sonoraCoreSources sonora_core SOURCES)
    get_target_property(_sonoraEngineSources sonora_engine SOURCES)
    get_target_property(_sonoraUiSources sonora_ui SOURCES)
    target_sources(SonoraPrism PRIVATE
        plugin/WavetablePlugin.cpp
        plugin/WavetablePluginEditor.cpp
        ${_sonoraCoreSources} ${_sonoraEngineSources} ${_sonoraUiSources})
    target_include_directories(SonoraPrism PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    get_target_property(_sonoraJuceDefinitions sonora_juce COMPILE_DEFINITIONS)
    target_compile_definitions(SonoraPrism PUBLIC
        ${_sonoraJuceDefinitions}
        JUCE_VST3_CAN_REPLACE_VST2=0
        SONORA_METAL_VIEW_CLASS=SonoraPrismMetalView) # Sonora.app may load this bundle: one ObjC class name each
    target_link_libraries(SonoraPrism
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_gui_extra
            sonora::skia
            sonora::zstd
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    if (APPLE)
        target_link_libraries(SonoraPrism PRIVATE "-framework Security")
    elseif (WIN32)
        target_link_libraries(SonoraPrism PRIVATE bcrypt)
    endif()
    if (APPLE)
        set_property(SOURCE gfx/MetalLayerView.mm APPEND PROPERTY COMPILE_OPTIONS -fobjc-arc) # not JUCE's own .mm files
    endif()
    set_target_properties(SonoraPrism SonoraPrism_VST3 PROPERTIES
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN TRUE)
    if (APPLE) # only the VST3 entry points: the bundle's copies of JUCE, Skia, FLAC... stay private to it
        target_link_options(SonoraPrism_VST3 PRIVATE
            "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/plugin/vst3_exports.txt")
    endif()
endif()

# ------------------------------------------------------------------ app
# COM-4 release identity and updates (docs/UPDATES.md). scripts/release/make_release.sh sets these; a plain developer build
# keeps updates off (empty feed/key) so it never offers to replace itself.
set(SONORA_APP_VERSION "${PROJECT_VERSION}" CACHE STRING "CFBundleShortVersionString shown to users")
set(SONORA_APP_BUILD "" CACHE STRING "CFBundleVersion, the value Sparkle compares (empty = SONORA_APP_VERSION)")
set(SONORA_UPDATE_CHANNEL "stable" CACHE STRING "stable (stable + beta items in one feed) or nightly (separate app and feed)")
set_property(CACHE SONORA_UPDATE_CHANNEL PROPERTY STRINGS stable nightly)
set(SONORA_UPDATE_FEED_URL "" CACHE STRING "Sparkle SUFeedURL (appcast.xml) for this channel; empty = updates off")
set(SONORA_UPDATE_PUBLIC_KEY "" CACHE STRING "Sparkle SUPublicEDKey (base64 EdDSA public key from generate_keys); empty = updates off")
set(SONORA_UPDATE_ALLOW_LOCAL_HTTP OFF CACHE BOOL "Allow a plain-HTTP localhost feed (local update tests only, never for releases)")
set(SONORA_BUNDLE_ID "" CACHE STRING "Bundle id override (local update tests use their own so they never touch the real app's defaults)")
set(SONORA_SPARKLE_DIR "${CMAKE_SOURCE_DIR}/third_party/sparkle" CACHE PATH "Sparkle 2 distribution (scripts/bootstrap.sh)")
if (APPLE AND EXISTS "${SONORA_SPARKLE_DIR}/Sparkle.framework")
    set(_sonoraSparkleDefault ON)
else()
    set(_sonoraSparkleDefault OFF)
endif()
option(SONORA_WITH_SPARKLE "Embed Sparkle 2 for application updates (macOS)" ${_sonoraSparkleDefault})
if (NOT SONORA_APP_BUILD)
    set(SONORA_APP_BUILD "${SONORA_APP_VERSION}")
endif()
if (SONORA_UPDATE_CHANNEL STREQUAL "nightly")
    set(_sonoraProduct "Sonora Nightly")
    set(_sonoraBundleId "org.sonora.daw.nightly") # installs next to stable, own settings, never overwrites it
else()
    set(_sonoraProduct "Sonora")
    set(_sonoraBundleId "org.sonora.daw")
endif()
if (SONORA_BUNDLE_ID)
    set(_sonoraBundleId "${SONORA_BUNDLE_ID}")
endif()
set(_sonoraPlist "<plist><dict>
    <key>SUFeedURL</key><string>${SONORA_UPDATE_FEED_URL}</string>
    <key>SUPublicEDKey</key><string>${SONORA_UPDATE_PUBLIC_KEY}</string>
    <key>SonoraUpdateChannel</key><string>${SONORA_UPDATE_CHANNEL}</string>
    <key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>
    <key>SUEnableAutomaticChecks</key><true/>
    <key>SUScheduledCheckInterval</key><integer>86400</integer>
    <key>SUAutomaticallyUpdate</key><false/>
    <key>SUVerifyUpdateBeforeExtraction</key><true/>
    <key>NSLocalNetworkUsageDescription</key><string>Sonora connects to collaborators on your local network during a shared session.</string>
    <key>NSBonjourServices</key><array><string>_sonora-collab._tcp</string></array>")
# ^ COL-3 collab: macOS 15+ local network privacy prompt text, and the Bonjour type LAN sessions advertise/browse (Apple TN3179).
if (SONORA_UPDATE_ALLOW_LOCAL_HTTP)
    # Local update tests (docs/UPDATES.md): plain HTTP to 127.0.0.1 and the SONORA_UPDATE_AUTOTEST background install.
    string(APPEND _sonoraPlist "
    <key>SUAllowsAutomaticUpdates</key><true/>
    <key>NSAppTransportSecurity</key><dict><key>NSAllowsLocalNetworking</key><true/></dict>")
else()
    # Releases never install silently: the user picks the moment (the restart waits for the session, see Updater.h).
    string(APPEND _sonoraPlist "
    <key>SUAllowsAutomaticUpdates</key><false/>")
endif()
string(APPEND _sonoraPlist "
</dict></plist>")

juce_add_gui_app(Sonora
    PRODUCT_NAME "${_sonoraProduct}"
    COMPANY_NAME "Sonora"
    BUNDLE_ID "${_sonoraBundleId}"
    VERSION ${SONORA_APP_VERSION}
    BUILD_VERSION ${SONORA_APP_BUILD}
    PLIST_TO_MERGE "${_sonoraPlist}"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Sonora records audio from your inputs."
    DOCUMENT_EXTENSIONS als
    ICON_BIG "${CMAKE_SOURCE_DIR}/resources/brand/icon-1024.png" # UI-42: tools/brand_render writes it
    ICON_SMALL "${CMAKE_SOURCE_DIR}/resources/brand/icon-256.png")

target_sources(Sonora PRIVATE
    app/Main.cpp
    app/MainWindow.cpp
    app/PromptWindow.cpp
    app/SplashWindow.cpp
    app/AppControl.cpp
    app/AppController.cpp
    app/AppCollab.cpp
    app/AppExport.cpp
    app/AppMediaEdits.cpp
    app/AppWarp.cpp
    app/AppResynth.cpp
    app/AppStems.cpp
)
if (SONORA_UPDATE_ALLOW_LOCAL_HTTP)
    set(_sonoraLocalTest 1)
else()
    set(_sonoraLocalTest 0)
endif()
set_source_files_properties(app/Main.cpp PROPERTIES COMPILE_DEFINITIONS
    "SONORA_APP_VERSION_STRING=\"${SONORA_APP_VERSION}\";SONORA_APP_NAME_STRING=\"${_sonoraProduct}\";SONORA_APP_DATA_FOLDER_STRING=\"${_sonoraProduct}\"")
target_compile_definitions(Sonora PRIVATE SONORA_UPDATE_LOCAL_TEST=${_sonoraLocalTest})
if(APPLE)
    target_sources(Sonora PRIVATE app/AssistantDock.mm app/WindowChrome.mm)
else()
    target_sources(Sonora PRIVATE app/AssistantDockPortable.cpp app/WindowChromePortable.cpp)
endif()
target_link_libraries(Sonora PRIVATE sonora_collab sonora_agent sonora_ui)
if (SONORA_WITH_SPARKLE)
    # Sparkle.framework is linked from third_party and copied into Contents/Frameworks (ditto keeps its symlinks and signature).
    target_sources(Sonora PRIVATE app/UpdaterSparkle.mm)
    target_compile_options(Sonora PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-F${SONORA_SPARKLE_DIR}>")
    target_link_options(Sonora PRIVATE "-F${SONORA_SPARKLE_DIR}" "-Wl,-rpath,@executable_path/../Frameworks")
    target_link_libraries(Sonora PRIVATE "-framework Sparkle")
    add_custom_command(TARGET Sonora POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:Sonora>/Frameworks
        COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_BUNDLE_CONTENT_DIR:Sonora>/Frameworks/Sparkle.framework
        COMMAND ditto ${SONORA_SPARKLE_DIR}/Sparkle.framework $<TARGET_BUNDLE_CONTENT_DIR:Sonora>/Frameworks/Sparkle.framework
        VERBATIM)
else()
    target_sources(Sonora PRIVATE app/UpdaterStub.cpp)
endif()
target_compile_options(Sonora PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
add_dependencies(Sonora sonora_plugin_scanner)
set_property(TARGET Sonora APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_SOURCE_DIR}/docs/UI_GUIDE.md" "${CMAKE_SOURCE_DIR}/docs/MENU_SETTINGS_GAPS.md" "${CMAKE_SOURCE_DIR}/docs/SHORTCUT_AUDIT.md"
    "${CMAKE_SOURCE_DIR}/docs/NATIVE_DEVICE_FACES.md")
set_property(TARGET Sonora APPEND PROPERTY LINK_DEPENDS "${CMAKE_SOURCE_DIR}/docs/RESYNTH.md")
# The scanner lives next to the executable (PluginHost::scannerExecutable); docs go to Resources/.
if(APPLE)
    set(_sonora_exe_dir "$<TARGET_BUNDLE_CONTENT_DIR:Sonora>/MacOS/")
    set(_sonora_resource_dir "$<TARGET_BUNDLE_CONTENT_DIR:Sonora>/Resources")
else()
    set(_sonora_exe_dir "$<TARGET_FILE_DIR:Sonora>/")
    set(_sonora_resource_dir "$<TARGET_FILE_DIR:Sonora>/Resources")
endif()
add_custom_command(TARGET Sonora POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sonora_plugin_scanner> ${_sonora_exe_dir}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_sonora_resource_dir}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/docs/UI_GUIDE.md ${_sonora_resource_dir}/UI_GUIDE.md
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/docs/NATIVE_DEVICE_FACES.md ${_sonora_resource_dir}/NATIVE_DEVICE_FACES.md
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/docs/RESYNTH.md ${_sonora_resource_dir}/RESYNTH.md
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/docs/MENU_SETTINGS_GAPS.md ${_sonora_resource_dir}/MENU_SETTINGS_GAPS.md
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/docs/SHORTCUT_AUDIT.md ${_sonora_resource_dir}/SHORTCUT_AUDIT.md
    VERBATIM)
