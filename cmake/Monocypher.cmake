# COM-4l: Monocypher's Ed25519 (CC0 / BSD-2-Clause, scripts/bootstrap.sh) verifies Linux updates against the EdDSA key the
# macOS and Windows updaters use. Consumers link `sonora_monocypher` and test `#if SONORA_HAVE_MONOCYPHER`; without the
# download the target still exists (=0) and the Linux updater reports updates off.
set(_sonora_mono_dir "${SONORA_THIRD_PARTY_DIR}/monocypher/src")
if(EXISTS "${_sonora_mono_dir}/optional/monocypher-ed25519.c")
    add_library(sonora_monocypher STATIC "${_sonora_mono_dir}/monocypher.c" "${_sonora_mono_dir}/optional/monocypher-ed25519.c")
    target_include_directories(sonora_monocypher SYSTEM PUBLIC "${_sonora_mono_dir}" "${_sonora_mono_dir}/optional")
    target_compile_definitions(sonora_monocypher PUBLIC SONORA_HAVE_MONOCYPHER=1)
    set_target_properties(sonora_monocypher PROPERTIES POSITION_INDEPENDENT_CODE TRUE C_VISIBILITY_PRESET hidden)
else()
    add_library(sonora_monocypher INTERFACE)
    target_compile_definitions(sonora_monocypher INTERFACE SONORA_HAVE_MONOCYPHER=0)
    message(STATUS "Sonora: Monocypher not found in ${_sonora_mono_dir}: Linux updates off (run scripts/bootstrap.sh)")
endif()
