#include "errors.h"

#include <wincodec.h>

namespace iris {

std::wstring DescribeError(HRESULT hr) {
    switch (hr) {
        case kErrorImageTooLarge:
            return L"The image is too large to convert safely.";
        case kErrorTooLargeForWebP:
            return L"WebP images cannot be wider or taller than 16383 pixels.";
        case kErrorCorruptImage:
        case WINCODEC_ERR_BADHEADER:
        case WINCODEC_ERR_BADIMAGE:
        case WINCODEC_ERR_BADSTREAMDATA:
        case WINCODEC_ERR_STREAMREAD:
        case WINCODEC_ERR_STREAMNOTAVAILABLE:
        case WINCODEC_ERR_FRAMEMISSING:
        case HRESULT_FROM_WIN32(ERROR_HANDLE_EOF):
            return L"The image is damaged or incomplete.";
        case WINCODEC_ERR_COMPONENTNOTFOUND:
        case WINCODEC_ERR_UNKNOWNIMAGEFORMAT:
            return L"The file is not a supported image.";
        case kErrorNoFreeName:
            return L"No free file name was found for the converted image.";
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):
        case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND):
            return L"The file no longer exists.";
        case E_ACCESSDENIED:
        case HRESULT_FROM_WIN32(ERROR_WRITE_PROTECT):
            return L"Access denied. The folder may be read-only.";
        case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION):
        case HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION):
            return L"The file is in use by another program.";
        case HRESULT_FROM_WIN32(ERROR_DISK_FULL):
        case STG_E_MEDIUMFULL:
            return L"There is not enough disk space.";
        case E_OUTOFMEMORY:
            return L"There is not enough memory to convert the image.";
    }

    // Prefer English to match the rest of Iris; fall back to the system language.
    constexpr DWORD kFlags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(kFlags, nullptr, static_cast<DWORD>(hr),
                                  MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                  reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0) {
        length = FormatMessageW(kFlags, nullptr, static_cast<DWORD>(hr), 0,
                                reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    }
    std::wstring message;
    if (length > 0) {
        message.assign(buffer, length);
        while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r')) {
            message.pop_back();
        }
    }
    LocalFree(buffer);

    if (message.empty()) {
        wchar_t code[32];
        wsprintfW(code, L"Error 0x%08X.", static_cast<unsigned>(hr));
        message = code;
    }
    return message;
}

}  // namespace iris
