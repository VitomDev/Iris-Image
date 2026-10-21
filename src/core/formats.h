#pragma once

#include <string_view>

namespace iris {

enum class Format { Png, Jpeg, WebP, Bmp, Gif, Tiff, Ico };

inline constexpr Format kAllFormats[] = {
    Format::Png, Format::Jpeg, Format::WebP, Format::Bmp,
    Format::Gif, Format::Tiff, Format::Ico,
};

// Menu label, e.g. "JPG".
const wchar_t* FormatLabel(Format format);

// Extension used for new files, including the dot, e.g. ".jpg".
const wchar_t* FormatExtension(Format format);

// Command-line name, e.g. "jpg".
const wchar_t* FormatId(Format format);

// Accepts the command-line names (case-insensitive) plus "jpeg" and "tif".
bool ParseFormatId(std::wstring_view id, Format& format);

// Maps a file extension (with or without the dot) to a format. Accepts the
// usual aliases (.jpeg, .jpe, .jfif, .tif).
bool FormatFromExtension(std::wstring_view extension, Format& format);

// Returns the extension of a path including the dot, or an empty view.
std::wstring_view PathExtension(std::wstring_view path);

}  // namespace iris
