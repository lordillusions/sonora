set(SONORA_TEST_SOURCES
    main.cpp
    test_core.cpp
    test_app_data_folder.cpp
    test_als.cpp
    test_sample_drop.cpp
    test_engine.cpp
    test_devices.cpp
    test_delay_reverb.cpp
    test_filter_steps.cpp
    test_native_expansion.cpp
    test_swiss_army_meter.cpp
    test_native_plugins.cpp
    test_compact_devices.cpp
    test_serum_state.cpp
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
    test_project.cpp
    test_updates.cpp
    test_control.cpp
    test_device_control.cpp
    test_op.cpp
    test_model_codec.cpp
    test_collab_session.cpp
    test_collab_protocol.cpp
    test_agent.cpp
    test_platform.cpp
)
if(SONORA_WITH_UI)
    list(APPEND SONORA_TEST_SOURCES test_resynth_ui.cpp test_transport_warp_ui.cpp test_sample_drop_ui.cpp)
    list(APPEND SONORA_TEST_SOURCES test_device_faces.cpp test_editor_workflow_ui.cpp test_browser_tags_ui.cpp test_context_track_ui.cpp test_track_state_ui.cpp test_shortcuts_ui.cpp test_updates_ui.cpp)
    list(APPEND SONORA_TEST_SOURCES test_ui.cpp test_clip_envelope_ui.cpp test_audio_clip.cpp test_grid.cpp test_grid_ui.cpp test_tools_ui.cpp test_clip_launch_ui.cpp test_native_plugin_ui.cpp test_curve_lfo_ui.cpp test_wavetable_ui.cpp test_wavetable_fx_ui.cpp test_wavetable_modulation_ui.cpp test_wavetable_routing_ui.cpp test_wavetable_sub_ui.cpp test_wavetable_quality_ui.cpp test_wavetable_noise_ui.cpp test_wavetable_window_ui.cpp test_wavetable_drag_modulation_ui.cpp test_wavetable_preset_browser_ui.cpp test_brand_ui.cpp)
    # PLG-11: the VST3's processor and editor, built here against the app's JUCE (no plug-in wrapper needed).
    list(APPEND SONORA_TEST_SOURCES test_wavetable_plugin.cpp ../src/plugin/WavetablePlugin.cpp ../src/plugin/WavetablePluginEditor.cpp)
endif()

add_executable(sonora_tests ${SONORA_TEST_SOURCES})
target_link_libraries(sonora_tests PRIVATE sonora_agent sonora_collab)
if(SONORA_WITH_UI)
    target_link_libraries(sonora_tests PRIVATE sonora_ui)
endif()
target_compile_definitions(sonora_tests PRIVATE
    SONORA_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_test(NAME sonora_tests COMMAND sonora_tests)
