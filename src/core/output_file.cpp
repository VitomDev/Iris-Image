#include "output_file.h"

#include <shlwapi.h>

#include "errors.h"

namespace iris {
namespace {

constexpr int kMaxNameAttempts = 10000;

size_t FileNameStart(std::wstring_view path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? 0 : separator + 1;
}

std::wstring RandomToken() {
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t token[40];
    wsprintfW(token, L"%08lx%04x%04x", guid.Data1, guid.Data2, guid.Data3);
    return token;
}

}  // namespace

std::wstring ToApiPath(const std::wstring& path) {
    constexpr size_t kSafeLength = MAX_PATH - 20;  // Room for the temporary name.
    if (path.size() < kSafeLength || path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    if (path.size() > 2 && path[1] == L':') return L"\\\\?\\" + path;
    return path;
}

std::wstring CandidateName(std::wstring_view sourcePath, std::wstring_view extension, int index) {
    const size_t nameStart = FileNameStart(sourcePath);
    size_t stemEnd = sourcePath.find_last_of(L'.');
    if (stemEnd == std::wstring_view::npos || stemEnd < nameStart) stemEnd = sourcePath.size();

    std::wstring name(sourcePath.substr(0, stemEnd));
    if (index > 0) {
        name += L" (";
        name += std::to_wstring(index);
        name += L')';
    }
    name += extension;
    return name;
}

OutputFile::~OutputFile() {
    if (!tempPath_.empty()) DeleteFileW(ToApiPath(tempPath_).c_str());
}

HRESULT OutputFile::Create(std::wstring_view sourcePath, Microsoft::WRL::ComPtr<IStream>& stream) {
    // The leading dot and hidden attribute keep the file out of sight while it is written.
    tempPath_.assign(sourcePath.substr(0, FileNameStart(sourcePath)));
    tempPath_ += L".iris-" + RandomToken() + L".tmp";

    // STGM_FAILIFTHERE maps to CREATE_NEW, so an existing file is never touched.
    const HRESULT hr = SHCreateStreamOnFileEx(
        ToApiPath(tempPath_).c_str(), STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_FAILIFTHERE,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, TRUE, nullptr, &stream);
    if (FAILED(hr)) tempPath_.clear();
    return hr;
}

HRESULT OutputFile::Commit(std::wstring_view sourcePath, std::wstring_view extension,
                           std::wstring& finalPath) {
    const std::wstring tempApiPath = ToApiPath(tempPath_);
    for (int index = 0; index < kMaxNameAttempts; ++index) {
        std::wstring candidate = CandidateName(sourcePath, extension, index);
        // Without MOVEFILE_REPLACE_EXISTING the rename fails if the name is taken,
        // which is atomic with respect to other processes creating the same name.
        if (MoveFileExW(tempApiPath.c_str(), ToApiPath(candidate).c_str(), MOVEFILE_WRITE_THROUGH)) {
            tempPath_.clear();
            SetFileAttributesW(ToApiPath(candidate).c_str(), FILE_ATTRIBUTE_NORMAL);
            finalPath = std::move(candidate);
            return S_OK;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            return HRESULT_FROM_WIN32(error);
        }
    }
    return kErrorNoFreeName;
}

}  // namespace iris
