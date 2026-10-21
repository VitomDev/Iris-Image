#include "formats.h"

#include <windows.h>

namespace iris {
namespace {

struct FormatInfo {
    Format format;
    const wchar_t* label;
    const wchar_t* extension;
    const wchar_t* id;
};

constexpr FormatInfo kFormats[] = {
    {Format::Png, L"PNG", L".png", L"png"},
    {Format::Jpeg, L"JPG", L".jpg", L"jpg"},
    {Format::WebP, L"WebP", L".webp", L"webp"},
    {Format::Bmp, L"BMP", L".bmp", L"bmp"},
    {Format::Gif, L"GIF", L".gif", L"gif"},
    {Format::Tiff, L"TIFF", L".tiff", L"tiff"},
    {Format::Ico, L"ICO", L".ico", L"ico"},
};

struct Alias {
    const wchar_t* name;
    Format format;
};

constexpr Alias kExtensionAliases[] = {
    {L"png", Format::Png},   {L"jpg", Format::Jpeg},  {L"jpeg", Format::Jpeg},
    {L"jpe", Format::Jpeg},  {L"jfif", Format::Jpeg}, {L"webp", Format::WebP},
    {L"bmp", Format::Bmp},   {L"dib", Format::Bmp},   {L"gif", Format::Gif},
    {L"tif", Format::Tiff},  {L"tiff", Format::Tiff}, {L"ico", Format::Ico},
};

const FormatInfo& Info(Format format) {
    for (const FormatInfo& info : kFormats) {
        if (info.format == format) return info;
    }
    return kFormats[0];
}

bool EqualsIgnoreCase(std::wstring_view a, const wchar_t* b) {
    const int length = static_cast<int>(a.size());
    return CompareStringOrdinal(a.data(), length, b, -1, TRUE) == CSTR_EQUAL;
}

}  // namespace

const wchar_t* FormatLabel(Format format) { return Info(format).label; }
const wchar_t* FormatExtension(Format format) { return Info(format).extension; }
const wchar_t* FormatId(Format format) { return Info(format).id; }

bool ParseFormatId(std::wstring_view id, Format& format) {
    return FormatFromExtension(id, format);
}

bool FormatFromExtension(std::wstring_view extension, Format& format) {
    if (!extension.empty() && extension.front() == L'.') extension.remove_prefix(1);
    for (const Alias& alias : kExtensionAliases) {
        if (EqualsIgnoreCase(extension, alias.name)) {
            format = alias.format;
            return true;
        }
    }
    return false;
}

std::wstring_view PathExtension(std::wstring_view path) {
    const size_t separator = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return {};
    if (separator != std::wstring_view::npos && dot < separator) return {};
    return path.substr(dot);
}

}  // namespace iris
