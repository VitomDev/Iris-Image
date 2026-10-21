#pragma once

#include <string>

namespace iris {

// Shows a standard Windows notification and returns once it has been
// dismissed or has timed out. Used only to report problems.
void ShowNotification(const std::wstring& title, const std::wstring& message);

}  // namespace iris
