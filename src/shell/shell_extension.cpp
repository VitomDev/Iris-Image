// IrisShell.dll: the "Iris — Convert to..." Explorer command.
//
// This code runs inside explorer.exe, so it does as little as possible: it
// reads the selected paths and starts Iris.exe, which does the actual work in
// its own process. No image is ever opened here.
//
// The same COM class serves both menus:
//  - Windows 11 modern menu: declared in the sparse package manifest.
//  - Classic menu (Windows 10, and Windows 11 "Show more options" when the
//    package is not registered): an ExplorerCommandHandler registry verb.

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/implements.h>
#include <wrl/module.h>

#include <string>
#include <vector>

#include "../core/formats.h"

using namespace Microsoft::WRL;

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

constexpr wchar_t kMenuTitle[] = L"Iris \x2014 Convert to...";
// Above this many selected items, skip the same-format check to keep the menu fast.
constexpr DWORD kMaxItemsToInspect = 512;
// CreateProcess accepts 32767 characters; longer selections go through a file.
constexpr size_t kMaxCommandLine = 30000;

std::wstring ModuleDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path.data(),
                                                static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return path.substr(0, path.find_last_of(L'\\'));
}

std::wstring WorkerPath() { return ModuleDirectory() + L"\\Iris.exe"; }

HRESULT SelectedPaths(IShellItemArray* items, std::vector<std::wstring>& paths) {
    DWORD count = 0;
    HRESULT hr = items ? items->GetCount(&count) : E_INVALIDARG;
    for (DWORD i = 0; SUCCEEDED(hr) && i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return hr;
}

// Writes the paths to a temporary UTF-16 file that Iris.exe deletes after reading.
HRESULT WriteListFile(const std::vector<std::wstring>& paths, std::wstring& listPath) {
    wchar_t directory[MAX_PATH + 1];
    if (!GetTempPathW(ARRAYSIZE(directory), directory)) return HRESULT_FROM_WIN32(GetLastError());
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t name[64];
    wsprintfW(name, L"Iris-%08lx%04x%04x.txt", guid.Data1, guid.Data2, guid.Data3);
    listPath = std::wstring(directory) + name;

    std::wstring content;
    for (const std::wstring& path : paths) content += path + L'\n';

    const HANDLE file = CreateFileW(listPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(content.size() * sizeof(wchar_t));
    const bool ok = WriteFile(file, content.data(), size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(listPath.c_str());
        return E_FAIL;
    }
    return S_OK;
}

HRESULT LaunchWorker(iris::Format format, const std::vector<std::wstring>& paths) {
    // Windows paths cannot contain quotes, so quoting each argument is enough.
    const std::wstring worker = WorkerPath();
    std::wstring commandLine = L"\"" + worker + L"\" convert " + iris::FormatId(format);
    for (const std::wstring& path : paths) commandLine += L" \"" + path + L"\"";

    std::wstring listPath;
    if (commandLine.size() > kMaxCommandLine) {
        const HRESULT hr = WriteListFile(paths, listPath);
        if (FAILED(hr)) return hr;
        commandLine = L"\"" + worker + L"\" convert " + iris::FormatId(format) + L" --list \"" + listPath + L"\"";
    }

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(worker.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_DEFAULT_ERROR_MODE, nullptr, nullptr, &startup, &process)) {
        const DWORD error = GetLastError();
        if (!listPath.empty()) DeleteFileW(listPath.c_str());
        return HRESULT_FROM_WIN32(error);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return S_OK;
}

// True when every selected file already has the extension of the format.
bool AllItemsInFormat(IShellItemArray* items, iris::Format format) {
    DWORD count = 0;
    if (!items || FAILED(items->GetCount(&count)) || count == 0 || count > kMaxItemsToInspect) return false;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR name = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) ||
            FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name))) {
            return false;
        }
        iris::Format itemFormat{};
        const bool same = iris::FormatFromExtension(iris::PathExtension(name), itemFormat) && itemFormat == format;
        CoTaskMemFree(name);
        if (!same) return false;
    }
    return true;
}

HRESULT ReturnString(const wchar_t* text, LPWSTR* result) { return SHStrDupW(text, result); }

class FormatCommand final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
public:
    explicit FormatCommand(iris::Format format) : format_(format) {}

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* title) override {
        return ReturnString(iris::FormatLabel(format_), title);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
        *icon = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* name) override {
        *name = GUID_NULL;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        // Converting a file to its own format would only recompress it. Hide that
        // entry when the whole selection already uses the format; for mixed
        // selections Iris.exe skips those files instead.
        *state = AllItemsInFormat(items, format_) ? ECS_HIDDEN : ECS_ENABLED;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override try {
        std::vector<std::wstring> paths;
        HRESULT hr = SelectedPaths(items, paths);
        if (SUCCEEDED(hr) && !paths.empty()) hr = LaunchWorker(format_, paths);
        return hr;
    } catch (...) {
        return E_UNEXPECTED;
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        *commands = nullptr;
        return E_NOTIMPL;
    }

private:
    iris::Format format_;
};

class CommandEnumerator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumExplorerCommand> {
public:
    HRESULT RuntimeClassInitialize() {
        for (iris::Format format : iris::kAllFormats) {
            ComPtr<FormatCommand> command = Make<FormatCommand>(format);
            if (!command) return E_OUTOFMEMORY;
            commands_.push_back(command);
        }
        return S_OK;
    }

    IFACEMETHODIMP Next(ULONG count, IExplorerCommand** result, ULONG* fetched) override {
        ULONG produced = 0;
        for (; produced < count && next_ < commands_.size(); ++produced, ++next_) {
            commands_[next_].CopyTo(&result[produced]);
        }
        if (fetched) *fetched = produced;
        return produced == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        next_ = (std::min)(next_ + count, commands_.size());
        return next_ < commands_.size() ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override {
        next_ = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumExplorerCommand** result) override {
        *result = nullptr;
        return E_NOTIMPL;
    }

private:
    std::vector<ComPtr<IExplorerCommand>> commands_;
    size_t next_ = 0;
};

class __declspec(uuid("4D8C2E61-7A3B-4F0E-9C15-2B6E8A1D3F47")) RootCommand final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
public:
    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* title) override { return ReturnString(kMenuTitle, title); }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override try {
        return ReturnString((WorkerPath() + L",0").c_str(), icon);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* name) override {
        *name = __uuidof(RootCommand);
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* state) override {
        *state = ECS_ENABLED;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_HASSUBCOMMANDS;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        return MakeAndInitialize<CommandEnumerator>(commands);
    }
};

CoCreatableClass(RootCommand);

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    return Module<InProc>::GetModule().GetClassObject(clsid, iid, object);
}

STDAPI DllCanUnloadNow() {
    return Module<InProc>::GetModule().GetObjectCount() == 0 ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
