# Sonora — open DAW that opens Ableton Live sets (.als).
# See docs/ARCHITECTURE.md before changing anything here.

cmake_minimum_required(VERSION 3.25)

# The prebuilt macOS Skia in third_party/skia is arm64-only (see docs/adr/0002-skia.md).
# Other platforms: docs/PLATFORMS.md (COM-7).
if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "")
endif()

project(Sonora VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# skia-pack's Windows libraries are built with the static release CRT (/MT, _ITERATOR_DEBUG_LEVEL=0; checked with
# `strings skia.lib`), so every MSVC target uses /MT and Windows UI builds must be Release or RelWithDebInfo (COM-7).
# The main thread also gets the 8 MB stack it has on macOS/Linux (Windows default: 1 MB). Tests keep several
# WavetableSynth (~412 KB each) on the stack and overflowed it.
# Source files are UTF-8: without /utf-8, MSVC stores literals such as "\u201C" or "×" in the Windows code page, and
# Skia then gets invalid UTF-8 (the likely cause of the save prompt test crashing sonora_tests on Windows CI).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " /STACK:8388608")
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")
endif()

option(SONORA_BUILD_TESTS "Build unit tests" ON)
option(SONORA_WITH_UI "Build Skia UI and the app (OFF = headless core/engine only)" ON)
option(SONORA_BUILD_TOOLS "Build CLI tools (als_dump, ui_snapshot)" ON)

# Where scripts/bootstrap.sh put JUCE and Skia. The Linux container keeps its own copy outside the
# source tree (ci/linux/build.sh), so a macOS checkout and the container never share binaries.
set(SONORA_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH "Folder holding JUCE/ and skia/")

add_subdirectory("${SONORA_THIRD_PARTY_DIR}/JUCE" "${CMAKE_BINARY_DIR}/third_party/JUCE" EXCLUDE_FROM_ALL)
if(SONORA_WITH_UI)
    include(cmake/Skia.cmake)
endif()
include(cmake/SonoraJuce.cmake)
include(cmake/Zstd.cmake)
include(cmake/CollabNet.cmake) # option SONORA_WITH_COLLAB_NET, target sonora_collab_net (COL-3)

add_subdirectory(src)

if(SONORA_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(SONORA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
