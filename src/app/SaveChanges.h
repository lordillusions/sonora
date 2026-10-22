#pragma once

#include <functional>
#include <utility>

namespace sonora::app
{

// NativeMessageBox::showAsync returns zero-based button indices, unlike the
// legacy AlertWindow result convention. Save, Don't Save, Cancel are 0, 1, 2.
inline void finishSaveChanges(int buttonIndex,
                              const std::function<void(std::function<void(bool)>)>& save,
                              std::function<void()> proceed)
{
    if (buttonIndex == 0)
        save([next = std::move(proceed)](bool saved) { if (saved) next(); });
    else if (buttonIndex == 1)
        proceed();
}

} // namespace sonora::app
