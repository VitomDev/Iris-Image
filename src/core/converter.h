#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "formats.h"

namespace iris {

enum class Outcome {
    Converted,
    AlreadyInFormat,  // Nothing written: re-encoding would only lose quality.
    Failed,
};

struct FileResult {
    Outcome outcome = Outcome::Failed;
    HRESULT error = S_OK;     // Set when outcome is Failed.
    std::wstring outputPath;  // Set when outcome is Converted.
};

// Converts one image into a new file next to it. COM must be initialized on
// the calling thread.
FileResult ConvertFile(const std::wstring& sourcePath, Format target);

// Converts several images with a small, bounded number of worker threads.
// Results are in the same order as the paths. One failure never stops the rest.
std::vector<FileResult> ConvertFiles(const std::vector<std::wstring>& sourcePaths, Format target);

}  // namespace iris
