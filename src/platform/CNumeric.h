#pragma once
// COM-7: locale-independent strtod. Parses with an explicit "C" numeric locale without touching the
// thread or process locale (the UI may run under a locale whose decimal separator is ',').
//
// macOS/Linux: newlocale + strtod_l (POSIX 2008). Windows: _create_locale + _strtod_l (MSVC CRT).
// Header-only; usable from every layer.

#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace sonora::platform
{

class CNumericLocale
{
public:
#if defined(_WIN32)
    CNumericLocale() : locale_(_create_locale(LC_NUMERIC, "C")) {}
    ~CNumericLocale() { if (locale_ != nullptr) _free_locale(locale_); }
#else
    CNumericLocale() : locale_(newlocale(LC_NUMERIC_MASK, "C", nullptr)) {}
    ~CNumericLocale() { if (locale_ != nullptr) freelocale(locale_); }
#endif
    CNumericLocale(const CNumericLocale&) = delete;
    CNumericLocale& operator=(const CNumericLocale&) = delete;

    bool valid() const { return locale_ != nullptr; }

    // Same contract as strtod: sets errno (ERANGE) and *end. Requires valid().
    double strtod(const char* text, char** end) const
    {
#if defined(_WIN32)
        return _strtod_l(text, end, locale_);
#else
        return strtod_l(text, end, locale_);
#endif
    }

private:
#if defined(_WIN32)
    _locale_t locale_;
#else
    locale_t locale_;
#endif
};

} // namespace sonora::platform
