// LIB-1: the rules shared by both downloadToFile implementations (HttpClient.h).
#include "platform/HttpClient.h"

namespace sonora::platform
{

bool downloadUrlAllowed(const juce::String& text)
{
    const juce::URL url(text);
    const auto scheme = url.getScheme().toLowerCase();
    if (! url.isWellFormed() || url.getDomain().isEmpty()) return false;
    if (scheme == "https") return true;
    if (scheme != "http") return false;
    auto host = url.getDomain().toLowerCase();
    if (host.startsWith("[")) host = host.fromFirstOccurrenceOf("[", false, false).upToFirstOccurrenceOf("]", false, false);
    else host = host.upToFirstOccurrenceOf(":", false, false);
    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

int64_t contentRangeStart(const juce::String& header, int64_t& total)
{
    total = 0;
    const auto text = header.trim();
    if (! text.startsWithIgnoreCase("bytes ")) return -1;
    const auto range = text.substring(6).trim();
    const auto first = range.upToFirstOccurrenceOf("-", false, false).trim();
    const auto size = range.fromFirstOccurrenceOf("/", false, false).trim();
    if (first.isEmpty() || ! first.containsOnly("0123456789") || ! range.contains("-") || ! range.contains("/")) return -1;
    if (size != "*")
    {
        if (size.isEmpty() || ! size.containsOnly("0123456789")) return -1;
        total = size.getLargeIntValue();
    }
    return first.getLargeIntValue();
}

} // namespace sonora::platform
