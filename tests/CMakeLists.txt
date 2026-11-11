set(SONORA_TEST_SOURCES
    main.cpp
    test_core.cpp
    test_automation_edits.cpp
    test_app_data_folder.cpp
    test_als.cpp
    test_vst2_bank.cpp
    test_sample_drop.cpp
    test_sample_files.cpp
    test_engine.cpp
    test_parallel_engine.cpp
    test_devices.cpp
    test_simpler_parity.cpp
    test_sampler.cpp
    test_channel_eq.cpp
    test_eq_display.cpp
    test_delay_reverb.cpp
    test_filter_steps.cpp
    test_native_expansion.cpp
    test_swiss_army_meter.cpp
    test_native_plugins.cpp
    test_compact_devices.cpp
    test_wavetable_instrument.cpp
    test_tension.cpp
    test_serum_state.cpp
    test_native_replacement.cpp
    test_lfotool_state.cpp
    test_serum_effects_import.cpp
    test_wavetable.cpp
    test_wavetable_routing.cpp
    test_wavetable_drag_modulation.cpp test_wavetable_serum_parity.cpp test_wavetable_preset_navigation.cpp
    test_wavetable_voice_mode.cpp
    test_wavetable_content.cpp
    test_wavetable_media_reference.cpp
    test_wavetable_data_quality.cpp
    test_wavetable_quality_data.cpp
    test_wavetable_downsampler.cpp
    test_wavetable_oversampling.cpp
    test_wavetable_mip_transition.cpp
    test_noise_sample_voice.cpp
    test_sample_kernel.cpp
    test_wavetable_noise.cpp
    test_wavetable_sub.cpp
    test_oscillator_media.cpp
    test_wavetable_effects.cpp
    test_wavetable_effects_content.cpp
    test_wavetable_effects_integration.cpp
    test_prism_preset.cpp
    test_supporters.cpp
    test_curve_lfo.cpp
    test_sonora_stand_in.cpp
    test_hush.cpp
    test_kickstract.cpp
    test_stutter.cpp
    test_phase_align.cpp
    test_sweep.cpp
    test_drum_fill.cpp
    test_hush_builtin.cpp
    test_midi.cpp
    test_midi_release.cpp
    test_arpeggiator.cpp
    test_note_length.cpp
    test_midi_export.cpp
    test_monitor.cpp
    test_export.cpp
    test_media_edits.cpp
    test_track_state.cpp
    test_plugin_playhead.cpp
    test_resynth.cpp
    test_tempo_analysis.cpp
    test_ladder_filter.cpp
    test_stems.cpp
    test_clip_launch_mappings.cpp
    test_midi_envelopes.cpp
    test_overdub.cpp
    test_session_recording.cpp
    test_edit.cpp
    test_clip_geometry.cpp
    test_project.cpp
    test_recovery.cpp
    test_live_compat.cpp
    test_updates.cpp
    test_update_feed.cpp ../src/app/UpdateFeed.cpp
    test_control.cpp
    test_device_control.cpp
    test_op.cpp
    test_locators.cpp
    test_model_codec.cpp
    test_collab_session.cpp
    test_collab_protocol.cpp
    test_collab_voice.cpp
    test_collab_video.cpp
    test_agent.cpp
    test_platform.cpp
    test_sample_packs.cpp
    test_device_presets.cpp
)
if(SONORA_WITH_UI)
    list(APPEND SONORA_TEST_SOURCES test_resynth_ui.cpp test_transport_warp_ui.cpp test_sample_drop_ui.cpp)
    list(APPEND SONORA_TEST_SOURCES test_device_faces.cpp test_wavetable_face_ui.cpp test_tension_face_ui.cpp test_sampler_face_ui.cpp test_editor_workflow_ui.cpp test_browser_tags_ui.cpp test_device_presets_ui.cpp test_sample_packs_ui.cpp test_context_track_ui.cpp test_locators_ui.cpp test_inline_rename_ui.cpp test_sample_files_ui.cpp test_drag_guide_ui.cpp test_clip_editing_ui.cpp test_ruler_zoom_ui.cpp test_track_state_ui.cpp test_set_view_ui.cpp test_shortcuts_ui.cpp test_updates_ui.cpp test_device_chain_drop_ui.cpp)
    list(APPEND SONORA_TEST_SOURCES test_ui.cpp test_clip_envelope_ui.cpp test_audio_clip.cpp test_grid.cpp test_grid_ui.cpp test_tools_ui.cpp test_automation_ui.cpp test_automation_edit_ui.cpp test_clip_launch_ui.cpp test_native_plugin_ui.cpp test_rack_face_ui.cpp test_curve_lfo_ui.cpp test_wavetable_ui.cpp test_wavetable_fx_ui.cpp test_wavetable_modulation_ui.cpp test_wavetable_routing_ui.cpp test_wavetable_sub_ui.cpp test_wavetable_quality_ui.cpp test_wavetable_noise_ui.cpp test_wavetable_window_ui.cpp test_device_mini_ui.cpp test_wavetable_drag_modulation_ui.cpp test_wavetable_preset_browser_ui.cpp test_brand_ui.cpp)
    # PLG-11: the VST3's processor and editor, built here against the app's JUCE (no plug-in wrapper needed).
    list(APPEND SONORA_TEST_SOURCES test_wavetable_plugin.cpp ../src/plugin/WavetablePlugin.cpp ../src/plugin/WavetablePluginEditor.cpp)
    # PLG-12: the Curve LFO VST3 likewise; Prism's source already defines the plug-in entry point.
    list(APPEND SONORA_TEST_SOURCES test_curve_lfo_plugin.cpp ../src/plugin/CurveLfoPlugin.cpp ../src/plugin/CurveLfoPluginEditor.cpp)
    set_source_files_properties(../src/plugin/CurveLfoPlugin.cpp PROPERTIES COMPILE_DEFINITIONS SONORA_PLUGIN_NO_ENTRY=1)
    list(APPEND SONORA_TEST_SOURCES test_hush_plugin.cpp ../src/plugin/HushPlugin.cpp ../src/plugin/HushPluginEditor.cpp)
    list(APPEND SONORA_TEST_SOURCES test_device_menu_swap.cpp) # PLG-16f: the device menu's replace actions (UI)
    list(APPEND SONORA_TEST_SOURCES test_hush_builtin_ui.cpp) # PLG-14g: the face in the device chain and its window
    list(APPEND SONORA_TEST_SOURCES test_sidechain_menu_ui.cpp) # PLG-14i: choosing the sidechain source
    list(APPEND SONORA_TEST_SOURCES test_clip_menu_creative_ui.cpp) # PLG-19/20: Make Riser, Generate Drum Fill
    list(APPEND SONORA_TEST_SOURCES test_sidechain_panel_ui.cpp) # PLG-22: hosted plug-in sidechain, the Sidechain panel
    list(APPEND SONORA_TEST_SOURCES test_hush_sidechain_engine.cpp) # PLG-22: Hush hears and shows its sidechain
    set_source_files_properties(../src/plugin/HushPlugin.cpp PROPERTIES COMPILE_DEFINITIONS SONORA_PLUGIN_NO_ENTRY=1)
    set_source_files_properties(../src/plugin/HushPluginEditor.cpp PROPERTIES COMPILE_DEFINITIONS JucePlugin_VersionString="${PROJECT_VERSION}")
    # PLG-17: Kickstract's VST3 processor/editor and its face in the device chain.
    list(APPEND SONORA_TEST_SOURCES test_kickstract_ui.cpp ../src/plugin/KickstractPlugin.cpp ../src/plugin/KickstractPluginEditor.cpp)
    set_source_files_properties(../src/plugin/KickstractPlugin.cpp PROPERTIES COMPILE_DEFINITIONS SONORA_PLUGIN_NO_ENTRY=1)
    # PLG-18: Stutter's VST3 processor/editor and its face in the device chain.
    list(APPEND SONORA_TEST_SOURCES test_stutter_ui.cpp ../src/plugin/StutterPlugin.cpp ../src/plugin/StutterPluginEditor.cpp)
    set_source_files_properties(../src/plugin/StutterPlugin.cpp PROPERTIES COMPILE_DEFINITIONS SONORA_PLUGIN_NO_ENTRY=1)
    set_source_files_properties(../src/plugin/StutterPluginEditor.cpp PROPERTIES COMPILE_DEFINITIONS JucePlugin_VersionString="${PROJECT_VERSION}")
    # PLG-21: Phase Align's VST3 processor/editor and its face in the device chain.
    list(APPEND SONORA_TEST_SOURCES test_phase_align_ui.cpp ../src/plugin/PhaseAlignPlugin.cpp ../src/plugin/PhaseAlignPluginEditor.cpp)
    set_source_files_properties(../src/plugin/PhaseAlignPlugin.cpp PROPERTIES COMPILE_DEFINITIONS SONORA_PLUGIN_NO_ENTRY=1)
    set_source_files_properties(../src/plugin/PhaseAlignPluginEditor.cpp PROPERTIES COMPILE_DEFINITIONS JucePlugin_VersionString="${PROJECT_VERSION}")
    # PLG-19: Sweep's VST3 processor/editor and its face in the device chain.
    list(APPEND SONORA_TEST_SOURCES test_sweep_ui.cpp ../src/plugin/SweepPlugin.cpp ../src/plugin/SweepPluginEditor.cpp)
    set_source_files_properties(../src/plugin/SweepPlugin.cpp PROPERTIES COMPILE_DEFINITIONS SONORA_PLUGIN_NO_ENTRY=1)
    set_source_files_properties(../src/plugin/SweepPluginEditor.cpp PROPERTIES COMPILE_DEFINITIONS JucePlugin_VersionString="${PROJECT_VERSION}")
endif()

add_executable(sonora_tests ${SONORA_TEST_SOURCES})
target_link_libraries(sonora_tests PRIVATE sonora_agent sonora_collab sonora_packs) # Monocypher via sonora_core
if(SONORA_WITH_UI)
    target_link_libraries(sonora_tests PRIVATE sonora_ui)
endif()
target_compile_definitions(sonora_tests PRIVATE
    SONORA_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME sonora_tests COMMAND sonora_tests)

if(APPLE AND TARGET SonoraPhaseAlign_VST3)
    add_dependencies(sonora_tests SonoraPhaseAlign_VST3) # PLG-22: a real VST3 with a sidechain bus to host
endif()
