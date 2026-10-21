#pragma once

#include <windows.h>

#include <string>

namespace iris {

// Iris-specific failures, reported alongside regular Win32 and WIC HRESULTs.
inline constexpr HRESULT kErrorImageTooLarge = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0201);
inline constexpr HRESULT kErrorTooLargeForWebP = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0202);
inline constexpr HRESULT kErrorCorruptImage = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0203);
inline constexpr HRESULT kErrorNoFreeName = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0204);

// Short, user-facing description of a conversion failure.
std::wstring DescribeError(HRESULT hr);

}  // namespace iris
