// Iris tests. A tiny self-contained runner: no test framework to download.
//
//   IrisTests.exe [path\to\Iris.exe]
//
// With the worker path, the end-to-end tests also run the real executable.

#include <windows.h>
#include <objbase.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "core/converter.h"
#include "core/errors.h"
#include "core/image.h"
#include "core/output_file.h"

using Microsoft::WRL::ComPtr;
using iris::Format;
using iris::Outcome;

namespace {

// --- Runner ----------------------------------------------------------------

struct TestCase {
    const char* name;
    void (*body)();
};

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*body)()) { Registry().push_back({name, body}); }
};

struct Failure {
    std::string message;
};

#define TEST(name)                                         \
    void name();                                           \
    const Registrar name##_registrar(#name, name);         \
    void name()

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) throw Failure{std::string(__FILE__ ":") + std::to_string(__LINE__) + \
                                        ": CHECK(" #condition ") failed"};                     \
    } while (0)

#define CHECK_HR(expression)                                                                  \
    do {                                                                                      \
        const HRESULT check_hr_ = (expression);                                               \
        if (FAILED(check_hr_)) {                                                              \
            char check_buffer_[32];                                                           \
            std::snprintf(check_buffer_, sizeof(check_buffer_), "0x%08X", unsigned(check_hr_)); \
            throw Failure{std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #expression \
                          " returned " + check_buffer_};                                      \
        }                                                                                     \
    } while (0)

std::wstring g_worker;

// --- Helpers ---------------------------------------------------------------

ComPtr<IWICImagingFactory> Factory() {
    ComPtr<IWICImagingFactory> factory;
    CHECK_HR(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    return factory;
}

struct Bgra {
    BYTE b, g, r, a;
};

struct Pixels {
    UINT width = 0;
    UINT height = 0;
    GUID container{};
    std::vector<Bgra> data;
    Bgra At(UINT x, UINT y) const { return data[static_cast<size_t>(y) * width + x]; }
};

bool Near(Bgra p, Bgra q, int tolerance = 12) {
    return std::abs(p.b - q.b) <= tolerance && std::abs(p.g - q.g) <= tolerance &&
           std::abs(p.r - q.r) <= tolerance && std::abs(p.a - q.a) <= tolerance;
}

constexpr Bgra kRed{0, 0, 255, 255};
constexpr Bgra kGreen{0, 255, 0, 255};
constexpr Bgra kBlue{255, 0, 0, 255};
constexpr Bgra kYellow{0, 255, 255, 255};
constexpr Bgra kWhite{255, 255, 255, 255};
constexpr Bgra kClear{0, 0, 0, 0};

class TempDir {
public:
    TempDir() {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        path_ = std::wstring(base) + L"IrisTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(counter_++);
        CreateDirectoryW(path_.c_str(), nullptr);
    }
    ~TempDir() { RemoveTree(iris::ToApiPath(path_)); }
    std::wstring operator/(const std::wstring& name) const { return path_ + L"\\" + name; }
    const std::wstring& path() const { return path_; }

    std::vector<std::wstring> Files() const {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW data;
        const HANDLE find = FindFirstFileW(iris::ToApiPath(path_ + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) return names;
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return names;
    }

private:
    static void RemoveTree(const std::wstring& path) {
        WIN32_FIND_DATAW data;
        const HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..") continue;
                const std::wstring child = path + L"\\" + name;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    RemoveTree(child);
                } else {
                    SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(child.c_str());
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        RemoveDirectoryW(path.c_str());
    }

    std::wstring path_;
    static inline std::atomic<int> counter_{0};
};

ComPtr<IWICBitmap> MakeBitmap(UINT width, UINT height, const std::function<Bgra(UINT, UINT)>& color) {
    std::vector<Bgra> data(static_cast<size_t>(width) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) data[static_cast<size_t>(y) * width + x] = color(x, y);
    }
    ComPtr<IWICBitmap> bitmap;
    CHECK_HR(Factory()->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                               static_cast<UINT>(data.size() * 4),
                                               reinterpret_cast<BYTE*>(data.data()), &bitmap));
    return bitmap;
}

// Four solid quadrants: red, green / blue, yellow. Easy to track through rotations.
ComPtr<IWICBitmap> Quadrants(UINT width, UINT height, bool transparentCorner = false) {
    return MakeBitmap(width, height, [=](UINT x, UINT y) {
        const bool right = x >= width / 2;
        const bool bottom = y >= height / 2;
        if (transparentCorner && right && bottom) return kClear;
        if (!bottom) return right ? kGreen : kRed;
        return right ? kYellow : kBlue;
    });
}

void SaveWithWic(IWICBitmapSource* source, const GUID& container, const std::wstring& path, UINT orientation = 0) {
    ComPtr<IWICImagingFactory> factory = Factory();
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    UINT width = 0, height = 0;
    CHECK_HR(source->GetSize(&width, &height));
    CHECK_HR(SHCreateStreamOnFileEx(iris::ToApiPath(path).c_str(), STGM_WRITE | STGM_CREATE, FILE_ATTRIBUTE_NORMAL,
                                    TRUE, nullptr, &stream));
    CHECK_HR(factory->CreateEncoder(container, nullptr, &encoder));
    CHECK_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    CHECK_HR(encoder->CreateNewFrame(&frame, &options));
    CHECK_HR(frame->Initialize(options.Get()));
    CHECK_HR(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    CHECK_HR(frame->SetPixelFormat(&format));
    if (orientation) {
        ComPtr<IWICMetadataQueryWriter> writer;
        CHECK_HR(frame->GetMetadataQueryWriter(&writer));
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_UI2;
        value.uiVal = static_cast<USHORT>(orientation);
        CHECK_HR(writer->SetMetadataByName(L"/app1/ifd/{ushort=274}", &value));
    }
    ComPtr<IWICFormatConverter> converter;
    CHECK_HR(factory->CreateFormatConverter(&converter));
    CHECK_HR(converter->Initialize(source, format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    CHECK_HR(frame->WriteSource(converter.Get(), nullptr));
    CHECK_HR(frame->Commit());
    CHECK_HR(encoder->Commit());
}

void SaveImage(IWICBitmapSource* source, Format format, const std::wstring& path) {
    iris::Image image;
    image.pixels = source;
    image.hasAlpha = true;
    ComPtr<IStream> stream;
    CHECK_HR(SHCreateStreamOnFileEx(iris::ToApiPath(path).c_str(), STGM_WRITE | STGM_CREATE, FILE_ATTRIBUTE_NORMAL,
                                    TRUE, nullptr, &stream));
    CHECK_HR(iris::EncodeImage(Factory().Get(), image, format, stream.Get()));
    CHECK_HR(stream->Commit(STGC_DEFAULT));
}

Pixels Load(const std::wstring& path) {
    ComPtr<IWICImagingFactory> factory = Factory();
    iris::Image image;
    CHECK_HR(iris::DecodeImage(factory.Get(), path, image));
    Pixels pixels;
    pixels.container = image.container;
    CHECK_HR(image.pixels->GetSize(&pixels.width, &pixels.height));
    ComPtr<IWICFormatConverter> converter;
    CHECK_HR(factory->CreateFormatConverter(&converter));
    CHECK_HR(converter->Initialize(image.pixels.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                   0, WICBitmapPaletteTypeCustom));
    pixels.data.resize(static_cast<size_t>(pixels.width) * pixels.height);
    CHECK_HR(converter->CopyPixels(nullptr, pixels.width * 4, static_cast<UINT>(pixels.data.size() * 4),
                                   reinterpret_cast<BYTE*>(pixels.data.data())));
    return pixels;
}

std::vector<BYTE> ReadBytes(const std::wstring& path) {
    std::vector<BYTE> bytes;
    const HANDLE file = CreateFileW(iris::ToApiPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    CHECK(ok && read == bytes.size());
    return bytes;
}

void WriteBytes(const std::wstring& path, const void* data, size_t size) {
    const HANDLE file = CreateFileW(iris::ToApiPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(file);
    CHECK(ok && written == size);
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(iris::ToApiPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

iris::FileResult Convert(const std::wstring& path, Format format) {
    return iris::ConvertFile(path, format);
}

void CheckQuadrants(const Pixels& p, Bgra topLeft, Bgra topRight, Bgra bottomLeft, Bgra bottomRight,
                    int tolerance = 12) {
    const UINT x0 = p.width / 4, x1 = p.width * 3 / 4, y0 = p.height / 4, y1 = p.height * 3 / 4;
    CHECK(Near(p.At(x0, y0), topLeft, tolerance));
    CHECK(Near(p.At(x1, y0), topRight, tolerance));
    CHECK(Near(p.At(x0, y1), bottomLeft, tolerance));
    CHECK(Near(p.At(x1, y1), bottomRight, tolerance));
}

// Converts a quadrant image saved as `from` into `to` and checks the result.
Pixels RoundTrip(const TempDir& dir, Format from, Format to, bool alpha) {
    const std::wstring source = dir / (std::wstring(L"source") + iris::FormatExtension(from));
    SaveImage(Quadrants(64, 48, alpha).Get(), from, source);
    const iris::FileResult result = Convert(source, to);
    CHECK(result.outcome == Outcome::Converted);
    CHECK(result.outputPath == dir / (std::wstring(L"source") + iris::FormatExtension(to)));
    Pixels pixels = Load(result.outputPath);
    CHECK(pixels.width == 64 && pixels.height == 48);
    return pixels;
}

// --- Conversion tests ------------------------------------------------------

TEST(PngToJpeg) {
    TempDir dir;
    const Pixels p = RoundTrip(dir, Format::Png, Format::Jpeg, false);
    CHECK(p.container == GUID_ContainerFormatJpeg);
    CheckQuadrants(p, kRed, kGreen, kBlue, kYellow, 24);
}

TEST(JpegToPng) {
    TempDir dir;
    const Pixels p = RoundTrip(dir, Format::Jpeg, Format::Png, false);
    CHECK(p.container == GUID_ContainerFormatPng);
    CheckQuadrants(p, kRed, kGreen, kBlue, kYellow, 24);
}

TEST(PngToWebPKeepsAlpha) {
    TempDir dir;
    const Pixels p = RoundTrip(dir, Format::Png, Format::WebP, true);
    CHECK(p.container == GUID_ContainerFormatWebp);
    CheckQuadrants(p, kRed, kGreen, kBlue, kClear, 16);
}

TEST(WebPToPng) {
    TempDir dir;
    const Pixels p = RoundTrip(dir, Format::WebP, Format::Png, true);
    CHECK(p.container == GUID_ContainerFormatPng);
    CheckQuadrants(p, kRed, kGreen, kBlue, kClear, 16);
}

TEST(PngToBmpKeepsAlpha) {
    TempDir dir;
    const Pixels p = RoundTrip(dir, Format::Png, Format::Bmp, true);
    CHECK(p.container == GUID_ContainerFormatBmp);
    CheckQuadrants(p, kRed, kGreen, kBlue, kClear, 0);
}

TEST(TiffEncodeAndDecode) {
    TempDir dir;
    const Pixels tiff = RoundTrip(dir, Format::Png, Format::Tiff, true);
    CHECK(tiff.container == GUID_ContainerFormatTiff);
    CheckQuadrants(tiff, kRed, kGreen, kBlue, kClear, 0);
    const iris::FileResult back = Convert(dir / L"source.tiff", Format::Png);
    CHECK(back.outcome == Outcome::Converted);
    CHECK(back.outputPath == dir / L"source (1).png");
    CheckQuadrants(Load(back.outputPath), kRed, kGreen, kBlue, kClear, 0);
}

TEST(GifKeepsTransparency) {
    TempDir dir;
    const Pixels p = RoundTrip(dir, Format::Png, Format::Gif, true);
    CHECK(p.container == GUID_ContainerFormatGif);
    CheckQuadrants(p, kRed, kGreen, kBlue, kClear, 8);
}

TEST(AnimatedGifUsesFirstFrame) {
    TempDir dir;
    const std::wstring path = dir / L"animated.gif";
    {
        ComPtr<IWICImagingFactory> factory = Factory();
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        CHECK_HR(SHCreateStreamOnFileEx(path.c_str(), STGM_WRITE | STGM_CREATE, 0, TRUE, nullptr, &stream));
        CHECK_HR(factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder));
        CHECK_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        for (Bgra color : {kRed, kBlue}) {
            ComPtr<IWICBitmapFrameEncode> frame;
            CHECK_HR(encoder->CreateNewFrame(&frame, nullptr));
            CHECK_HR(frame->Initialize(nullptr));
            CHECK_HR(frame->SetSize(16, 16));
            WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
            CHECK_HR(frame->SetPixelFormat(&format));
            ComPtr<IWICPalette> palette;
            CHECK_HR(factory->CreatePalette(&palette));
            CHECK_HR(palette->InitializePredefined(WICBitmapPaletteTypeFixedHalftone256, FALSE));
            CHECK_HR(frame->SetPalette(palette.Get()));
            ComPtr<IWICFormatConverter> converter;
            CHECK_HR(factory->CreateFormatConverter(&converter));
            CHECK_HR(converter->Initialize(MakeBitmap(16, 16, [=](UINT, UINT) { return color; }).Get(), format,
                                           WICBitmapDitherTypeNone, palette.Get(), 0, WICBitmapPaletteTypeFixedHalftone256));
            CHECK_HR(frame->WriteSource(converter.Get(), nullptr));
            CHECK_HR(frame->Commit());
        }
        CHECK_HR(encoder->Commit());
    }
    const iris::FileResult result = Convert(path, Format::Png);
    CHECK(result.outcome == Outcome::Converted);
    const Pixels p = Load(result.outputPath);
    CHECK(p.width == 16 && p.height == 16);
    CHECK(Near(p.At(8, 8), kRed, 40));
}

TEST(IcoIsValidMultiSizeIcon) {
    TempDir dir;
    const std::wstring source = dir / L"logo.png";
    SaveImage(Quadrants(300, 200, true).Get(), Format::Png, source);
    const iris::FileResult result = Convert(source, Format::Ico);
    CHECK(result.outcome == Outcome::Converted);

    const std::vector<BYTE> bytes = ReadBytes(result.outputPath);
    CHECK(bytes.size() > 6);
    const WORD reserved = *reinterpret_cast<const WORD*>(&bytes[0]);
    const WORD type = *reinterpret_cast<const WORD*>(&bytes[2]);
    const WORD count = *reinterpret_cast<const WORD*>(&bytes[4]);
    CHECK(reserved == 0 && type == 1 && count == 7);
    const UINT expected[] = {16, 24, 32, 48, 64, 128, 256};
    for (WORD i = 0; i < count; ++i) {
        const BYTE* entry = &bytes[6 + i * 16];
        const UINT size = entry[0] == 0 ? 256 : entry[0];
        const DWORD length = *reinterpret_cast<const DWORD*>(entry + 8);
        const DWORD offset = *reinterpret_cast<const DWORD*>(entry + 12);
        CHECK(size == expected[i] && entry[1] == entry[0]);
        CHECK(offset + length <= bytes.size());
        CHECK(std::memcmp(&bytes[offset], "\x89PNG", 4) == 0);
    }

    // Windows' own icon decoder must read every entry.
    ComPtr<IWICImagingFactory> factory = Factory();
    ComPtr<IWICBitmapDecoder> decoder;
    CHECK_HR(factory->CreateDecoderFromFilename(result.outputPath.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder));
    UINT frames = 0;
    CHECK_HR(decoder->GetFrameCount(&frames));
    CHECK(frames == 7);
    const Pixels largest = Load(result.outputPath);  // Iris decodes the largest entry.
    CHECK(largest.width == 256 && largest.height == 256);
    CHECK(largest.At(128, 10).a == 0);  // Letterbox padding is transparent.
    CHECK(Near(largest.At(70, 100), kRed, 16));
}

TEST(SmallImageToIcoIsNotUpscaled) {
    TempDir dir;
    const std::wstring source = dir / L"tiny.png";
    SaveImage(Quadrants(20, 20).Get(), Format::Png, source);
    const iris::FileResult result = Convert(source, Format::Ico);
    CHECK(result.outcome == Outcome::Converted);
    const std::vector<BYTE> bytes = ReadBytes(result.outputPath);
    CHECK(*reinterpret_cast<const WORD*>(&bytes[4]) == 1);  // Only 16x16.
}

TEST(JpegFlattensTransparencyOnWhite) {
    TempDir dir;
    const Pixels p = RoundTrip(dir, Format::Png, Format::Jpeg, true);
    CheckQuadrants(p, kRed, kGreen, kBlue, kWhite, 24);
}

TEST(ExifOrientationIsApplied) {
    // Where a stored pixel (x, y) is displayed for each EXIF orientation.
    auto display = [](UINT o, int x, int y, int w, int h) -> std::pair<int, int> {
        switch (o) {
            case 2: return {w - 1 - x, y};
            case 3: return {w - 1 - x, h - 1 - y};
            case 4: return {x, h - 1 - y};
            case 5: return {y, x};
            case 6: return {h - 1 - y, x};
            case 7: return {h - 1 - y, w - 1 - x};
            case 8: return {y, w - 1 - x};
            default: return {x, y};
        }
    };
    const int w = 64, h = 32;
    const ComPtr<IWICBitmap> stored = Quadrants(w, h);
    for (UINT orientation = 1; orientation <= 8; ++orientation) {
        TempDir dir;
        const std::wstring source = dir / L"photo.jpg";
        SaveWithWic(stored.Get(), GUID_ContainerFormatJpeg, source, orientation);
        const iris::FileResult result = Convert(source, Format::Png);
        CHECK(result.outcome == Outcome::Converted);
        const Pixels p = Load(result.outputPath);
        const bool swapped = orientation >= 5;
        CHECK(p.width == UINT(swapped ? h : w) && p.height == UINT(swapped ? w : h));
        const std::pair<int, int> samples[] = {{w / 4, h / 4}, {w * 3 / 4, h / 4}, {w / 4, h * 3 / 4}, {w * 3 / 4, h * 3 / 4}};
        const Bgra colors[] = {kRed, kGreen, kBlue, kYellow};
        for (int i = 0; i < 4; ++i) {
            const auto [x, y] = display(orientation, samples[i].first, samples[i].second, w, h);
            if (!Near(p.At(x, y), colors[i], 40)) {
                throw Failure{"orientation " + std::to_string(orientation) + ", quadrant " + std::to_string(i)};
            }
        }
    }
}

TEST(OddDimensionsArePreserved) {
    TempDir dir;
    const std::wstring source = dir / L"odd.png";
    SaveImage(Quadrants(1, 999).Get(), Format::Png, source);
    for (Format format : iris::kAllFormats) {
        if (format == Format::Png || format == Format::Ico) continue;
        const iris::FileResult result = Convert(source, format);
        CHECK(result.outcome == Outcome::Converted);
        const Pixels p = Load(result.outputPath);
        CHECK(p.width == 1 && p.height == 999);
    }
}

TEST(CorruptImageFailsCleanly) {
    TempDir dir;
    const std::wstring good = dir / L"good.png";
    SaveImage(Quadrants(64, 64).Get(), Format::Png, good);
    std::vector<BYTE> bytes = ReadBytes(good);
    bytes.resize(bytes.size() / 2);
    WriteBytes(dir / L"truncated.png", bytes.data(), bytes.size());
    std::vector<BYTE> noise(4096);
    for (size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<BYTE>(i * 7919 + 13);
    std::memcpy(noise.data(), "\x89PNG\r\n\x1a\n", 8);
    WriteBytes(dir / L"garbage.png", noise.data(), noise.size());
    const char riff[] = "RIFF\x10\0\0\0WEBPVP8 \x04\0\0\0abcd";
    WriteBytes(dir / L"broken.webp", riff, sizeof(riff) - 1);

    // Unreadable data must fail with a message and leave nothing behind.
    for (const wchar_t* name : {L"garbage.png", L"broken.webp"}) {
        for (Format format : {Format::Jpeg, Format::WebP, Format::Ico}) {
            const iris::FileResult result = Convert(dir / name, format);
            CHECK(result.outcome == Outcome::Failed);
            CHECK(!iris::DescribeError(result.error).empty());
        }
    }
    // Like other Windows viewers, WIC renders a truncated PNG with its missing
    // rows blank. Either outcome is fine as long as it is safe and complete.
    const iris::FileResult truncated = Convert(dir / L"truncated.png", Format::Bmp);
    if (truncated.outcome == Outcome::Converted) {
        CHECK(Load(truncated.outputPath).height == 64);
        CHECK(DeleteFileW(truncated.outputPath.c_str()));
    }
    CHECK(dir.Files().size() == 4);  // Nothing new, no temporary files left.
}

TEST(UnsupportedFileFails) {
    TempDir dir;
    const char text[] = "This is not an image.";
    WriteBytes(dir / L"notes.png", text, sizeof(text) - 1);
    WriteBytes(dir / L"empty.jpg", "", 0);
    const iris::FileResult result = Convert(dir / L"notes.png", Format::Jpeg);
    CHECK(result.outcome == Outcome::Failed);
    CHECK(iris::DescribeError(result.error) == L"The file is not a supported image.");
    CHECK(Convert(dir / L"empty.jpg", Format::Png).outcome == Outcome::Failed);
    CHECK(Convert(dir / L"missing.png", Format::Jpeg).outcome == Outcome::Failed);
    CHECK(dir.Files().size() == 2);
}

TEST(SameFormatIsSkipped) {
    TempDir dir;
    const std::wstring source = dir / L"photo.png";
    SaveImage(Quadrants(8, 8).Get(), Format::Png, source);
    CHECK(Convert(source, Format::Png).outcome == Outcome::AlreadyInFormat);
    CHECK(dir.Files().size() == 1);
}

TEST(OversizedWebPFailsWithoutLeftovers) {
    TempDir dir;
    const std::wstring source = dir / L"wide.png";
    SaveImage(MakeBitmap(17000, 1, [](UINT, UINT) { return kRed; }).Get(), Format::Png, source);
    const iris::FileResult result = Convert(source, Format::WebP);
    CHECK(result.outcome == Outcome::Failed && result.error == iris::kErrorTooLargeForWebP);
    CHECK(dir.Files().size() == 1);
}

TEST(HugeDimensionsAreRejected) {
    // A valid header announcing 60000 x 60000 pixels (3.6 gigapixels).
    TempDir dir;
    const BYTE header[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                           0, 0, 0xEA, 0x60, 0, 0, 0xEA, 0x60, 8, 6, 0, 0, 0, 0, 0, 0, 0};
    WriteBytes(dir / L"bomb.png", header, sizeof(header));
    const iris::FileResult result = Convert(dir / L"bomb.png", Format::Jpeg);
    CHECK(result.outcome == Outcome::Failed);
    CHECK(dir.Files().size() == 1);
}

// --- File handling tests ---------------------------------------------------

TEST(UnicodeAndSpacesInNames) {
    TempDir dir;
    const std::wstring source = dir / L"Ph\x00f6to \x5199\x771f \x0444\x043e\x0442\x043e #1 & (copy) \xd83d\xdcf7.png";
    SaveImage(Quadrants(16, 16).Get(), Format::Png, source);
    const iris::FileResult result = Convert(source, Format::WebP);
    CHECK(result.outcome == Outcome::Converted);
    CHECK(result.outputPath == dir / L"Ph\x00f6to \x5199\x771f \x0444\x043e\x0442\x043e #1 & (copy) \xd83d\xdcf7.webp");
    CHECK(Exists(result.outputPath));
}

TEST(ExistingFilesAreNeverOverwritten) {
    TempDir dir;
    const std::wstring source = dir / L"photo.png";
    SaveImage(Quadrants(16, 16).Get(), Format::Png, source);
    const std::vector<BYTE> original = ReadBytes(source);
    const char existing[] = "keep me";
    WriteBytes(dir / L"photo.webp", existing, sizeof(existing));

    CHECK(Convert(source, Format::WebP).outputPath == dir / L"photo (1).webp");
    CHECK(Convert(source, Format::WebP).outputPath == dir / L"photo (2).webp");
    CHECK(ReadBytes(dir / L"photo.webp").size() == sizeof(existing));
    CHECK(ReadBytes(source) == original);
}

TEST(NamesWithoutOrWithExtraDots) {
    CHECK(iris::CandidateName(L"C:\\a.b\\photo", L".png", 0) == L"C:\\a.b\\photo.png");
    CHECK(iris::CandidateName(L"C:\\a\\my.photo.v2.jpg", L".webp", 3) == L"C:\\a\\my.photo.v2 (3).webp");
    CHECK(iris::CandidateName(L"C:\\a\\.hidden.png", L".jpg", 0) == L"C:\\a\\.hidden.jpg");
}

TEST(ConcurrentConversionsGetDistinctNames) {
    TempDir dir;
    const std::wstring source = dir / L"shared.png";
    SaveImage(Quadrants(256, 256).Get(), Format::Png, source);
    constexpr int kThreads = 8;
    std::vector<std::wstring> outputs(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            outputs[i] = iris::ConvertFile(source, Format::Jpeg).outputPath;
            CoUninitialize();
        });
    }
    for (std::thread& thread : threads) thread.join();
    for (int i = 0; i < kThreads; ++i) {
        CHECK(!outputs[i].empty());
        for (int j = 0; j < i; ++j) CHECK(outputs[i] != outputs[j]);
        CHECK(Load(outputs[i]).width == 256);
    }
    CHECK(dir.Files().size() == kThreads + 1);
}

TEST(ReadOnlyFolderFailsCleanly) {
    TempDir dir;
    const std::wstring source = dir / L"photo.png";
    SaveImage(Quadrants(16, 16).Get(), Format::Png, source);

    // Deny creating files in the folder, keep everything else allowed.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x2;;;WD)(A;OICI;FA;;;WD)", SDDL_REVISION_1,
                                                                &descriptor, nullptr));
    CHECK(SetFileSecurityW(dir.path().c_str(), DACL_SECURITY_INFORMATION, descriptor));
    const iris::FileResult result = Convert(source, Format::Jpeg);
    PSECURITY_DESCRIPTOR open = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &open, nullptr);
    SetFileSecurityW(dir.path().c_str(), DACL_SECURITY_INFORMATION, open);
    LocalFree(descriptor);
    LocalFree(open);

    CHECK(result.outcome == Outcome::Failed);
    CHECK(result.error == E_ACCESSDENIED);
    CHECK(dir.Files().size() == 1);
}

TEST(LongPaths) {
    TempDir dir;
    std::wstring folder = dir.path();
    while (folder.size() < 300) {
        folder += L"\\a-rather-long-folder-name-for-testing";
        CHECK(CreateDirectoryW(iris::ToApiPath(folder).c_str(), nullptr));
    }
    const std::wstring source = folder + L"\\picture.png";
    SaveImage(Quadrants(16, 16).Get(), Format::Png, source);
    const iris::FileResult result = Convert(source, Format::Tiff);
    CHECK(result.outcome == Outcome::Converted);
    CHECK(result.outputPath.size() > MAX_PATH);
    CHECK(Load(result.outputPath).width == 16);
}

TEST(BatchContinuesAfterFailures) {
    TempDir dir;
    std::vector<std::wstring> paths;
    for (int i = 0; i < 12; ++i) {
        paths.push_back(dir / (L"image" + std::to_wstring(i) + L".png"));
        SaveImage(Quadrants(40 + i, 30).Get(), Format::Png, paths.back());
    }
    WriteBytes(dir / L"bad.png", "nope", 4);
    paths.insert(paths.begin() + 5, dir / L"bad.png");
    paths.push_back(dir / L"image3.png");  // Selected twice: gets its own name.

    const std::vector<iris::FileResult> results = iris::ConvertFiles(paths, Format::WebP);
    CHECK(results.size() == paths.size());
    int converted = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (i == 5) {
            CHECK(results[i].outcome == Outcome::Failed);
        } else {
            CHECK(results[i].outcome == Outcome::Converted);
            ++converted;
        }
    }
    CHECK(converted == 13);
    CHECK(Exists(dir / L"image3 (1).webp"));
    CHECK(dir.Files().size() == 13 + 13);
}

// --- End-to-end tests with Iris.exe ----------------------------------------

DWORD RunWorker(const std::wstring& arguments) {
    std::wstring commandLine = L"\"" + g_worker + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(g_worker.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                         &startup, &process));
    WaitForSingleObject(process.hProcess, 60000);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode;
}

TEST(WorkerConvertsArguments) {
    if (g_worker.empty()) return;
    TempDir dir;
    SaveImage(Quadrants(32, 32).Get(), Format::Png, dir / L"a b.png");
    SaveImage(Quadrants(32, 32).Get(), Format::Png, dir / L"c.png");
    CHECK(RunWorker(L"convert jpg \"" + (dir / L"a b.png") + L"\" \"" + (dir / L"c.png") + L"\"") == 0);
    CHECK(Exists(dir / L"a b.jpg") && Exists(dir / L"c.jpg"));
}

TEST(WorkerReadsAndDeletesListFile) {
    if (g_worker.empty()) return;
    TempDir dir;
    std::wstring list;
    for (int i = 0; i < 3; ++i) {
        const std::wstring path = dir / (L"list" + std::to_wstring(i) + L".png");
        SaveImage(Quadrants(16, 16).Get(), Format::Png, path);
        list += path + L"\r\n";
    }
    TempDir listDir;
    const std::wstring listPath = listDir / L"selection.txt";
    WriteBytes(listPath, list.data(), list.size() * sizeof(wchar_t));
    CHECK(RunWorker(L"convert bmp --list \"" + listPath + L"\"") == 0);
    for (int i = 0; i < 3; ++i) CHECK(Exists(dir / (L"list" + std::to_wstring(i) + L".bmp")));
    CHECK(!Exists(listPath));
}

TEST(WorkerRejectsBadArguments) {
    if (g_worker.empty()) return;
    CHECK(RunWorker(L"") == 2);
    CHECK(RunWorker(L"convert avif C:\\x.png") == 2);
    CHECK(RunWorker(L"delete png C:\\x.png") == 2);
}


// --- Shell extension tests (load the real IrisShell.dll) ------------------

constexpr CLSID kRootCommand = {0x4D8C2E61, 0x7A3B, 0x4F0E, {0x9C, 0x15, 0x2B, 0x6E, 0x8A, 0x1D, 0x3F, 0x47}};

ComPtr<IExplorerCommand> LoadRootCommand() {
    const std::wstring dll = g_worker.substr(0, g_worker.find_last_of(L'\\')) + L"\\IrisShell.dll";
    const HMODULE module = LoadLibraryW(dll.c_str());
    CHECK(module != nullptr);
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
    CHECK(getClassObject != nullptr);
    ComPtr<IClassFactory> factory;
    CHECK_HR(getClassObject(kRootCommand, IID_PPV_ARGS(&factory)));
    ComPtr<IExplorerCommand> root;
    CHECK_HR(factory->CreateInstance(nullptr, IID_PPV_ARGS(&root)));
    return root;
}

ComPtr<IShellItemArray> ItemArray(const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const std::wstring& path : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        CHECK_HR(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr));
        ids.push_back(id);
    }
    ComPtr<IShellItemArray> items;
    CHECK_HR(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()),
                                               const_cast<PCIDLIST_ABSOLUTE_ARRAY>(ids.data()), &items));
    for (PIDLIST_ABSOLUTE id : ids) CoTaskMemFree(id);
    return items;
}

std::vector<ComPtr<IExplorerCommand>> SubCommands(IExplorerCommand* root) {
    ComPtr<IEnumExplorerCommand> enumerator;
    CHECK_HR(root->EnumSubCommands(&enumerator));
    std::vector<ComPtr<IExplorerCommand>> commands;
    ComPtr<IExplorerCommand> command;
    while (enumerator->Next(1, &command, nullptr) == S_OK) commands.push_back(std::move(command));
    return commands;
}

std::wstring Title(IExplorerCommand* command) {
    PWSTR title = nullptr;
    CHECK_HR(command->GetTitle(nullptr, &title));
    std::wstring result = title;
    CoTaskMemFree(title);
    return result;
}

EXPCMDSTATE State(IExplorerCommand* command, IShellItemArray* items) {
    EXPCMDSTATE state{};
    CHECK_HR(command->GetState(items, FALSE, &state));
    return state;
}

TEST(ShellMenuStructure) {
    if (g_worker.empty()) return;
    const ComPtr<IExplorerCommand> root = LoadRootCommand();
    CHECK(Title(root.Get()) == L"Iris \x2014 Convert to...");
    EXPCMDFLAGS flags = 0;
    CHECK_HR(root->GetFlags(&flags));
    CHECK(flags & ECF_HASSUBCOMMANDS);
    const std::vector<ComPtr<IExplorerCommand>> commands = SubCommands(root.Get());
    const wchar_t* expected[] = {L"PNG", L"JPG", L"WebP", L"BMP", L"GIF", L"TIFF", L"ICO"};
    CHECK(commands.size() == 7);
    for (size_t i = 0; i < commands.size(); ++i) CHECK(Title(commands[i].Get()) == expected[i]);
}

TEST(ShellHidesFormatOfWholeSelection) {
    if (g_worker.empty()) return;
    TempDir dir;
    for (const wchar_t* name : {L"a.png", L"b.PNG", L"c.jpeg"}) {
        SaveImage(Quadrants(8, 8).Get(), name[2] == L'j' ? Format::Jpeg : Format::Png, dir / name);
    }
    const std::vector<ComPtr<IExplorerCommand>> commands = SubCommands(LoadRootCommand().Get());
    const ComPtr<IShellItemArray> pngs = ItemArray({dir / L"a.png", dir / L"b.PNG"});
    const ComPtr<IShellItemArray> mixed = ItemArray({dir / L"a.png", dir / L"c.jpeg"});
    const ComPtr<IShellItemArray> jpeg = ItemArray({dir / L"c.jpeg"});
    CHECK(State(commands[0].Get(), pngs.Get()) == ECS_HIDDEN);   // PNG
    CHECK(State(commands[1].Get(), pngs.Get()) == ECS_ENABLED);  // JPG
    CHECK(State(commands[0].Get(), mixed.Get()) == ECS_ENABLED);
    CHECK(State(commands[1].Get(), mixed.Get()) == ECS_ENABLED);
    CHECK(State(commands[1].Get(), jpeg.Get()) == ECS_HIDDEN);   // .jpeg counts as JPG
}

TEST(ShellMenuIsFastForLargeSelections) {
    if (g_worker.empty()) return;
    TempDir dir;
    std::vector<std::wstring> paths;
    const std::vector<BYTE> png = [&] {
        SaveImage(Quadrants(8, 8).Get(), Format::Png, dir / L"seed.png");
        return ReadBytes(dir / L"seed.png");
    }();
    for (int i = 0; i < 500; ++i) {
        paths.push_back(dir / (L"many" + std::to_wstring(i) + L".png"));
        WriteBytes(paths.back(), png.data(), png.size());
    }
    const ComPtr<IShellItemArray> items = ItemArray(paths);
    const ComPtr<IExplorerCommand> root = LoadRootCommand();
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const std::vector<ComPtr<IExplorerCommand>> commands = SubCommands(root.Get());
    for (const ComPtr<IExplorerCommand>& command : commands) State(command.Get(), items.Get());
    QueryPerformanceCounter(&end);
    const double ms = (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
    std::printf("         menu for 500 selected files built in %.2f ms\n", ms);
    CHECK(ms < 250);
}

TEST(ShellInvokeStartsWorker) {
    if (g_worker.empty()) return;
    TempDir dir;
    SaveImage(Quadrants(16, 16).Get(), Format::Png, dir / L"menu one.png");
    SaveImage(Quadrants(16, 16).Get(), Format::Png, dir / L"menu two.png");
    const std::vector<ComPtr<IExplorerCommand>> commands = SubCommands(LoadRootCommand().Get());
    CHECK_HR(commands[2]->Invoke(ItemArray({dir / L"menu one.png", dir / L"menu two.png"}).Get(), nullptr));  // WebP
    for (int i = 0; i < 200 && !(Exists(dir / L"menu one.webp") && Exists(dir / L"menu two.webp")); ++i) Sleep(50);
    CHECK(Exists(dir / L"menu one.webp") && Exists(dir / L"menu two.webp"));
    Sleep(200);  // Let the worker exit before the folder is removed.
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) g_worker = argv[1];
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;

    int failed = 0;
    for (const TestCase& test : Registry()) {
        const ULONGLONG start = GetTickCount64();
        try {
            test.body();
            std::printf("[ PASS ] %s (%llu ms)\n", test.name, GetTickCount64() - start);
        } catch (const Failure& failure) {
            ++failed;
            std::printf("[ FAIL ] %s\n         %s\n", test.name, failure.message.c_str());
        }
    }
    std::printf("\n%zu tests, %d failed\n", Registry().size(), failed);
    CoUninitialize();
    return failed == 0 ? 0 : 1;
}
