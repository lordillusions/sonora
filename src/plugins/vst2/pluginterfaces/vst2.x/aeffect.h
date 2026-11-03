#pragma once
// PLG-15: JUCE's VST2 host includes this path inside namespace Vst2. FST (third_party/fst, GPL-3.0-or-later) provides
// the interface as clean-room headers; scripts/bootstrap.sh fetches it. See docs/adr/0003-plugin-format.md.
// FST marks opcodes it has not documented yet as deprecated and prints notes; JUCE's host uses a few, so both are quiet.
#ifndef FST_DONT_DEPRECATE_UNKNOWN
 #define FST_DONT_DEPRECATE_UNKNOWN 1
#endif
#ifndef FST_QUIET
 #define FST_QUIET 1 // fst-host-layout.patch: no "document origin" notes
#endif
#if defined(__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-W#pragma-messages"
 #pragma clang diagnostic ignored "-Wc99-extensions"
 #pragma clang diagnostic ignored "-Wc++98-compat-extra-semi"
#elif defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wpedantic"
#elif defined(_MSC_VER)
 #pragma warning(push)
 #pragma warning(disable: 4200) // zero-sized array in struct
#endif
#include <fst/aeffect.h>
#if defined(__clang__)
 #pragma clang diagnostic pop
#elif defined(__GNUC__)
 #pragma GCC diagnostic pop
#elif defined(_MSC_VER)
 #pragma warning(pop)
#endif
