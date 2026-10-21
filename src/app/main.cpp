// Iris.exe: the conversion worker started by the Explorer context menu.
//
//   Iris.exe convert <format> <image>...
//   Iris.exe convert <format> --list <file>   (UTF-16 file, one path per line;
//                                             deleted after reading)
//
// It converts the images, reports failures with a notification and exits.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "../core/converter.h"
#include "../core/errors.h"
#include "notification.h"

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitUsage = 2;
constexpr int kExitFailures = 1;
constexpr size_t kMaxListedFailures = 3;

std::wstring FileName(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

bool ReadPathList(const std::wstring& listPath, std::vector<std::wstring>& paths) {
    const HANDLE file = CreateFileW(listPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart <= (64 << 20) && size.QuadPart % 2 == 0;
    std::wstring content;
    if (ok) {
        content.resize(static_cast<size_t>(size.QuadPart / 2));
        DWORD read = 0;
        ok = ReadFile(file, content.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) &&
             read == size.QuadPart;
    }
    CloseHandle(file);
    if (!ok) return false;

    size_t start = 0;
    while (start < content.size()) {
        size_t end = content.find(L'\n', start);
        if (end == std::wstring::npos) end = content.size();
        std::wstring line = content.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) paths.push_back(std::move(line));
        start = end + 1;
    }
    return true;
}

void ReportResults(const std::vector<std::wstring>& paths, const std::vector<iris::FileResult>& results,
                   iris::Format target) {
    size_t converted = 0;
    size_t alreadyInFormat = 0;
    std::vector<size_t> failures;
    for (size_t i = 0; i < results.size(); ++i) {
        switch (results[i].outcome) {
            case iris::Outcome::Converted: ++converted; break;
            case iris::Outcome::AlreadyInFormat: ++alreadyInFormat; break;
            case iris::Outcome::Failed: failures.push_back(i); break;
        }
    }

    const std::wstring label = iris::FormatLabel(target);
    if (failures.empty()) {
        // Success is visible in Explorer itself. Only explain why nothing happened.
        if (converted > 0 || alreadyInFormat == 0) return;
        const std::wstring message = paths.size() == 1
                                         ? FileName(paths[0]) + L" is already a " + label + L" image."
                                         : L"All selected images are already " + label + L" images.";
        iris::ShowNotification(L"Nothing to convert", message);
        return;
    }

    if (paths.size() == 1) {
        iris::ShowNotification(L"Could not convert " + FileName(paths[0]),
                               iris::DescribeError(results[0].error));
        return;
    }

    const std::wstring title = L"Converted " + std::to_wstring(converted) + L" of " +
                               std::to_wstring(paths.size()) + L" images to " + label;
    std::wstring message;
    for (size_t i = 0; i < failures.size() && i < kMaxListedFailures; ++i) {
        if (!message.empty()) message += L'\n';
        message += FileName(paths[failures[i]]) + L": " + iris::DescribeError(results[failures[i]].error);
    }
    if (failures.size() > kMaxListedFailures) {
        message += L"\nand " + std::to_wstring(failures.size() - kMaxListedFailures) + L" more failed.";
    }
    iris::ShowNotification(title, message);
}

int Run(int argc, wchar_t** argv) {
    iris::Format target{};
    if (argc < 4 || lstrcmpiW(argv[1], L"convert") != 0 || !iris::ParseFormatId(argv[2], target)) {
        return kExitUsage;
    }

    std::vector<std::wstring> paths;
    if (lstrcmpW(argv[3], L"--list") == 0) {
        if (argc != 5 || !ReadPathList(argv[4], paths)) return kExitUsage;
    } else {
        paths.assign(argv + 3, argv + argc);
    }
    if (paths.empty()) return kExitUsage;

    const std::vector<iris::FileResult> results = iris::ConvertFiles(paths, target);
    ReportResults(paths, results, target);
    for (const iris::FileResult& result : results) {
        if (result.outcome == iris::Outcome::Failed) return kExitFailures;
    }
    return kExitSuccess;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Keep conversions from competing with the foreground app.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(init)) return kExitFailures;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const int exitCode = argv ? Run(argc, argv) : kExitUsage;
    LocalFree(argv);
    CoUninitialize();
    return exitCode;
}
