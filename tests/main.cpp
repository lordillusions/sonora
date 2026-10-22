#include "TestFramework.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    // Unbuffered: when a test crashes the process (CI, other OSes), the last printed line names the test before it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const auto& t : sonora::test::registry())
    {
        if (filter != nullptr && std::strstr(t.name, filter) == nullptr)
            continue;
        ++run;
        const int before = sonora::test::failureCount();
        try
        {
            t.fn();
        }
        catch (const sonora::test::RequireFailed&)
        {
        }
        catch (const std::exception& e)
        {
            ++sonora::test::failureCount();
            std::printf("    EXCEPTION %s\n", e.what());
        }
        std::printf("%s %s\n", sonora::test::failureCount() == before ? "[ OK ]" : "[FAIL]", t.name);
    }
    std::printf("\n%d tests, %d failed checks\n", run, sonora::test::failureCount());
    return sonora::test::failureCount() == 0 ? 0 : 1;
}
