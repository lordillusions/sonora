# Imports the prebuilt Skia from ${SONORA_THIRD_PARTY_DIR}/skia (JetBrains skia-pack, m144, Release).
# Provides the INTERFACE target `sonora::skia`.
#
#   macOS   arm64  out/Release-macos-arm64     Ganesh Metal + GL, CoreText fonts
#   Linux   x64    out/Release-linux-x64       Ganesh GL, fontconfig + bundled FreeType (COM-7)
#   Windows x64    out/Release-windows-x64     Ganesh GL + D3D, DirectWrite, static CRT /MT (COM-7)
#
# The compile definitions below MUST match how the prebuilt was configured, otherwise
# class layouts differ and you get crashes that look like memory corruption.
# If you upgrade Skia, re-check them (see docs/adr/0002-skia.md and skia-pack's script/build.py).

set(SONORA_SKIA_DIR "${SONORA_THIRD_PARTY_DIR}/skia")
if(APPLE)
    set(SONORA_SKIA_LIB_DIR "${SONORA_SKIA_DIR}/out/Release-macos-arm64")
elseif(WIN32)
    set(SONORA_SKIA_LIB_DIR "${SONORA_SKIA_DIR}/out/Release-windows-x64")
else()
    set(SONORA_SKIA_LIB_DIR "${SONORA_SKIA_DIR}/out/Release-linux-x64")
endif()

if(WIN32)
    set(_sonora_skia_probe "${SONORA_SKIA_LIB_DIR}/skia.lib")
else()
    set(_sonora_skia_probe "${SONORA_SKIA_LIB_DIR}/libskia.a")
endif()
if(NOT EXISTS "${_sonora_skia_probe}")
    message(FATAL_ERROR
        "Skia not found at ${SONORA_SKIA_LIB_DIR}. Run scripts/bootstrap.sh first.")
endif()

add_library(sonora_skia INTERFACE)
add_library(sonora::skia ALIAS sonora_skia)

target_include_directories(sonora_skia SYSTEM INTERFACE "${SONORA_SKIA_DIR}")

# Dependents before their dependencies (GNU ld resolves static libraries in order): codecs before zlib.
if(WIN32)
    # The Windows package prefixes the codec libraries with "lib" and has no FreeType (DirectWrite fonts).
    set(_sonora_skia_codecs libpng libjpeg libwebp libwebp_sse41 d3d12allocator)
else()
    set(_sonora_skia_codecs png jpeg webp webp_sse41)
endif()
set(_sonora_skia_libs
    skparagraph skshaper skunicode_icu skunicode_core skia
    harfbuzz icu skcms ${_sonora_skia_codecs} zlib expat wuffs bentleyottmann)

if(APPLE)
    target_compile_definitions(sonora_skia INTERFACE
        SK_RELEASE            # prebuilt is Release; our Debug builds must not flip SK_DEBUG
        SK_GANESH
        SK_METAL
        SK_GL
        SK_BUILD_FOR_MAC)
elseif(WIN32)
    target_compile_definitions(sonora_skia INTERFACE
        SK_RELEASE
        SK_GANESH
        SK_GL
        SK_DIRECT3D
        SK_BUILD_FOR_WIN
        SK_FONT_HOST_USE_SYSTEM_SETTINGS)
else()
    # skia-pack builds Linux x64 with gcc-10, -fno-rtti and _GLIBCXX_USE_CXX11_ABI=0: never pass
    # std::string or other ABI-tagged standard types across the Skia boundary.
    target_compile_definitions(sonora_skia INTERFACE
        SK_RELEASE
        SK_GANESH
        SK_GL
        SK_BUILD_FOR_UNIX)
    list(APPEND _sonora_skia_libs freetype2)
endif()

foreach(lib IN LISTS _sonora_skia_libs)
    if(WIN32)
        target_link_libraries(sonora_skia INTERFACE "${SONORA_SKIA_LIB_DIR}/${lib}.lib")
    else()
        target_link_libraries(sonora_skia INTERFACE "${SONORA_SKIA_LIB_DIR}/lib${lib}.a")
    endif()
endforeach()

if(APPLE)
    target_link_libraries(sonora_skia INTERFACE
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreText"
        "-framework CoreGraphics"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework OpenGL")
elseif(WIN32)
    target_link_libraries(sonora_skia INTERFACE d3d12 dxgi d3dcompiler opengl32 usp10 ole32 user32 gdi32 fontsub)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SONORA_FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    find_package(Threads REQUIRED)
    target_link_libraries(sonora_skia INTERFACE PkgConfig::SONORA_FONTCONFIG Threads::Threads ${CMAKE_DL_LIBS})
endif()
