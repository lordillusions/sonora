# Collab network transport (COL-3, docs/COLLAB_DESIGN.md section 8, docs/adr/0009-collab-sessions.md):
# libdatachannel (MPL-2.0) for WebRTC data channels and the WebSocket signalling client/server.
# scripts/bootstrap.sh clones it into third_party/libdatachannel. It is optional: without the checkout
# (or without OpenSSL off macOS) the option defaults OFF and collab networking is compiled out.
#
# Consumers link the INTERFACE target `sonora_collab_net` and test `#if SONORA_COLLAB_NET`; the target
# exists in both cases (SONORA_COLLAB_NET=1 with libdatachannel linked, =0 without).
#
# TLS/DTLS uses OpenSSL 3, linked statically so Sonora.app has no OpenSSL dylib:
#   macOS: third_party/openssl-macos-arm64, built by scripts/bootstrap.sh from pinned source with
#          -mmacosx-version-min=13.0 (Homebrew's openssl@3 targets the newest macOS only); see SONORA_OPENSSL_ROOT.
#   Linux: the distribution's libssl-dev. Windows: the static /MT libraries (libcrypto_static.lib) of an OpenSSL 3
#   install found by CMake's FindOpenSSL; with only DLL import libraries collab is OFF (the installer ships no DLL).

set(_sonora_dc_dir "${SONORA_THIRD_PARTY_DIR}/libdatachannel")

if(APPLE)
    set(SONORA_OPENSSL_ROOT "${SONORA_THIRD_PARTY_DIR}/openssl-macos-arm64" CACHE PATH
        "OpenSSL 3 prefix with static libssl.a/libcrypto.a used by libdatachannel (macOS)")
    if(SONORA_OPENSSL_ROOT STREQUAL "/opt/homebrew/opt/openssl@3") # early COL-3 default: built for macOS 26 only
        set(SONORA_OPENSSL_ROOT "${SONORA_THIRD_PARTY_DIR}/openssl-macos-arm64" CACHE PATH "" FORCE)
    endif()
    set(_sonora_dc_ssl_ok FALSE)
    if(EXISTS "${SONORA_OPENSSL_ROOT}/lib/libssl.a" AND EXISTS "${SONORA_OPENSSL_ROOT}/lib/libcrypto.a")
        set(_sonora_dc_ssl_ok TRUE)
    endif()
else()
    if(WIN32)
        # Static libraries built for /MT (libcrypto_static.lib in lib/VC/x64/MT of the usual OpenSSL installer,
        # preinstalled on GitHub's windows runners). These must be set before the first find_package: the cached
        # paths are reused by libdatachannel's own find_package.
        set(OPENSSL_USE_STATIC_LIBS TRUE)
        set(OPENSSL_MSVC_STATIC_RT TRUE)
    endif()
    find_package(OpenSSL 3 QUIET)
    set(_sonora_dc_ssl_ok ${OPENSSL_FOUND})
    # FindOpenSSL falls back to the import library (libcrypto.lib -> libcrypto-3-x64.dll) when no static one exists.
    # The installer ships no OpenSSL DLL, so Sonora.exe would not start: build without collab instead.
    if(WIN32 AND _sonora_dc_ssl_ok AND NOT OPENSSL_CRYPTO_LIBRARY MATCHES "_static")
        message(STATUS "Sonora: OpenSSL at ${OPENSSL_CRYPTO_LIBRARY} is not a static library")
        set(_sonora_dc_ssl_ok FALSE)
    endif()
endif()

set(_sonora_dc_default OFF)
if(EXISTS "${_sonora_dc_dir}/CMakeLists.txt" AND _sonora_dc_ssl_ok)
    set(_sonora_dc_default ON)
endif()
option(SONORA_WITH_COLLAB_NET "Build the collab network transport (libdatachannel + OpenSSL)" ${_sonora_dc_default})

add_library(sonora_collab_net INTERFACE)

if(NOT SONORA_WITH_COLLAB_NET)
    if(NOT EXISTS "${_sonora_dc_dir}/CMakeLists.txt")
        message(STATUS "Sonora: collab network OFF (no ${_sonora_dc_dir}; run scripts/bootstrap.sh)")
    elseif(NOT _sonora_dc_ssl_ok)
        message(STATUS "Sonora: collab network OFF (no static OpenSSL 3; macOS: run scripts/bootstrap.sh)")
    else()
        message(STATUS "Sonora: collab network OFF (SONORA_WITH_COLLAB_NET=OFF)")
    endif()
    target_compile_definitions(sonora_collab_net INTERFACE SONORA_COLLAB_NET=0)
    return()
endif()

if(NOT EXISTS "${_sonora_dc_dir}/CMakeLists.txt")
    message(FATAL_ERROR "SONORA_WITH_COLLAB_NET=ON but ${_sonora_dc_dir} is missing; run scripts/bootstrap.sh")
endif()

# block() keeps these switches out of the rest of Sonora's build (BUILD_SHARED_LIBS in particular).
block()
    set(BUILD_SHARED_LIBS OFF)
    set(NO_MEDIA ON)
    set(NO_WEBSOCKET OFF)
    set(NO_EXAMPLES ON)
    set(NO_TESTS ON)
    set(USE_GNUTLS OFF)
    set(USE_MBEDTLS OFF)
    set(USE_NICE OFF)
    set(PREFER_SYSTEM_LIB OFF)
    set(WARNINGS_AS_ERRORS OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # let these plain variables override option() in the subproject
    if(APPLE)
        set(OPENSSL_ROOT_DIR "${SONORA_OPENSSL_ROOT}")
        set(OPENSSL_USE_STATIC_LIBS TRUE)
        # libdatachannel forces the library paths from OPENSSL_ROOT_DIR; force the headers too so a cached
        # include dir from another OpenSSL can never pair with these libraries.
        set(OPENSSL_INCLUDE_DIR "${SONORA_OPENSSL_ROOT}/include" CACHE PATH "" FORCE)
    endif()
    # EXCLUDE_FROM_ALL: only what sonora_collab_net pulls in gets built (datachannel-static, juice, usrsctp).
    # SYSTEM: consumers see libdatachannel's headers as system headers, so they never warn in Sonora code.
    add_subdirectory("${_sonora_dc_dir}" "${CMAKE_BINARY_DIR}/third_party/libdatachannel" EXCLUDE_FROM_ALL SYSTEM)
endblock()

# Sonora's warning flags live on its own targets (juce_recommended_warning_flags), never on these.
# Silence the third-party code's own -Wall -Wextra so the build log stays warning-free.
foreach(_sonora_dc_target IN ITEMS datachannel-static juice-static usrsctp)
    if(TARGET ${_sonora_dc_target})
        if(MSVC)
            target_compile_options(${_sonora_dc_target} PRIVATE /w)
        else()
            target_compile_options(${_sonora_dc_target} PRIVATE -w)
        endif()
    endif()
endforeach()

target_link_libraries(sonora_collab_net INTERFACE datachannel-static)
target_compile_definitions(sonora_collab_net INTERFACE SONORA_COLLAB_NET=1)
get_target_property(_sonora_dc_version datachannel-static VERSION)
if(APPLE)
    message(STATUS "Sonora: collab network ON (libdatachannel ${_sonora_dc_version}, static OpenSSL from ${SONORA_OPENSSL_ROOT})")
else()
    message(STATUS "Sonora: collab network ON (libdatachannel ${_sonora_dc_version}, OpenSSL ${OPENSSL_VERSION})")
endif()
