// A temporary notification-area icon carrying a balloon. Windows 10 and 11 turn
// the balloon into a regular toast, which needs no app registration, and the
// icon is removed again as soon as the toast goes away.

#include "notification.h"

#include <windows.h>
#include <shellapi.h>

#include "resource.h"

namespace iris {
namespace {

constexpr UINT kCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kTimeoutTimer = 1;
constexpr UINT kTimeoutMs = 12000;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kCallbackMessage) {
        switch (LOWORD(lParam)) {
            case NIN_BALLOONTIMEOUT:
            case NIN_BALLOONHIDE:
            case NIN_BALLOONUSERCLICK:
                PostQuitMessage(0);
                return 0;
        }
    } else if (message == WM_TIMER && wParam == kTimeoutTimer) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void CopyTruncated(wchar_t* destination, size_t capacity, const std::wstring& text) {
    lstrcpynW(destination, text.c_str(), static_cast<int>(capacity));
    if (text.size() >= capacity && capacity > 4) lstrcpyW(destination + capacity - 4, L"...");
}

}  // namespace

void ShowNotification(const std::wstring& title, const std::wstring& message) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"IrisNotification";
    RegisterClassExW(&windowClass);
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Iris", 0, 0, 0, 0, 0,
                                        HWND_MESSAGE, nullptr, instance, nullptr);
    if (!window) return;

    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = window;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_INFO;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_IRIS), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    data.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
    lstrcpyW(data.szTip, L"Iris");
    CopyTruncated(data.szInfoTitle, ARRAYSIZE(data.szInfoTitle), title);
    CopyTruncated(data.szInfo, ARRAYSIZE(data.szInfo), message);

    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        SetTimer(window, kTimeoutTimer, kTimeoutMs, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    if (data.hIcon) DestroyIcon(data.hIcon);
    DestroyWindow(window);
}

}  // namespace iris
