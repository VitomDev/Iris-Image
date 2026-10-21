#pragma once

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <string>
#include <string_view>

namespace iris {

// Adds the \\?\ prefix to long absolute paths so Win32 file APIs accept them.
std::wstring ToApiPath(const std::wstring& path);

// Returns "<directory>\<stem><suffix><extension>" where suffix is "" for
// index 0 and " (index)" otherwise, e.g. "photo (2).webp".
std::wstring CandidateName(std::wstring_view sourcePath, std::wstring_view extension, int index);

// A converted image being written next to its source.
//
// The image is encoded into a hidden temporary file in the destination folder
// and only renamed to its final name once it is complete, so a failed or
// interrupted conversion never leaves a partial image behind. The rename never
// replaces an existing file, which makes name collisions safe even when several
// conversions run at the same time.
class OutputFile {
public:
    OutputFile() = default;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    ~OutputFile();  // Deletes the temporary file unless Commit succeeded.

    // Creates the temporary file in the folder of sourcePath.
    HRESULT Create(std::wstring_view sourcePath, Microsoft::WRL::ComPtr<IStream>& stream);

    // Moves the finished temporary file to the first free candidate name. The
    // stream returned by Create must be released first.
    HRESULT Commit(std::wstring_view sourcePath, std::wstring_view extension, std::wstring& finalPath);

private:
    std::wstring tempPath_;
};

}  // namespace iris
