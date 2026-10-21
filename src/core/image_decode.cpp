// Decoding: Windows Imaging Component for every format except WebP, which is
// decoded with libwebp because WIC only reads WebP when an optional Store codec
// is installed.

#include <shlwapi.h>
#include <propvarutil.h>
#include <webp/decode.h>
#include <webp/demux.h>

#include <cmath>
#include <cstring>

#include "errors.h"
#include "image.h"
#include "output_file.h"

using Microsoft::WRL::ComPtr;

namespace iris {
namespace {

// Limits for untrusted input: 2^28 pixels is 1 GiB as 32-bit BGRA.
constexpr UINT64 kMaxPixels = 1ull << 28;
constexpr UINT kMaxDimension = 65535;
constexpr UINT64 kMaxWebPFileSize = 256ull << 20;

HRESULT CheckDimensions(UINT width, UINT height) {
    if (width == 0 || height == 0) return kErrorCorruptImage;
    if (width > kMaxDimension || height > kMaxDimension) return kErrorImageTooLarge;
    if (static_cast<UINT64>(width) * height > kMaxPixels) return kErrorImageTooLarge;
    return S_OK;
}

bool IsRgbProfile(const std::vector<BYTE>& profile) {
    // ICC header: the data color space signature is at offset 16.
    return profile.size() >= 128 && std::memcmp(profile.data() + 16, "RGB ", 4) == 0;
}

void SanitizeResolution(double& dpi) {
    if (!std::isfinite(dpi) || dpi < 1.0 || dpi > 100000.0) dpi = 96.0;
}

bool PixelFormatHasAlpha(IWICImagingFactory* factory, REFWICPixelFormatGUID format) {
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo2> pixelInfo;
    BOOL transparency = FALSE;
    if (SUCCEEDED(factory->CreateComponentInfo(format, &info)) &&
        SUCCEEDED(info.As(&pixelInfo))) {
        pixelInfo->SupportsTransparency(&transparency);
    }
    return transparency != FALSE;
}

std::vector<BYTE> ReadRgbProfile(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return {};

    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(factory->CreateColorContext(&contexts[i]))) return {};
        raw[i] = contexts[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count))) return {};

    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type{};
        UINT size = 0;
        if (FAILED(raw[i]->GetType(&type)) || type != WICColorContextProfile) continue;
        if (FAILED(raw[i]->GetProfileBytes(0, nullptr, &size)) || size == 0) continue;
        std::vector<BYTE> profile(size);
        if (FAILED(raw[i]->GetProfileBytes(size, profile.data(), &size))) continue;
        if (IsRgbProfile(profile)) return profile;
    }
    return {};
}

// EXIF orientation (1-8) as stored in JPEG and TIFF files.
UINT ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    PROPVARIANT value;
    PropVariantInit(&value);
    UINT orientation = 1;
    if (SUCCEEDED(reader->GetMetadataByName(L"System.Photo.Orientation", &value)) &&
        value.vt == VT_UI2 && value.uiVal >= 1 && value.uiVal <= 8) {
        orientation = value.uiVal;
    }
    PropVariantClear(&value);
    return orientation;
}

// Rotations are clockwise and WIC applies the flip before the rotation.
WICBitmapTransformOptions OrientationTransform(UINT orientation) {
    switch (orientation) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipVertical);
        case 6: return WICBitmapTransformRotate90;
        case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

HRESULT ReadWholeStream(IStream* stream, std::vector<BYTE>& data) {
    STATSTG stat{};
    HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    if (stat.cbSize.QuadPart > kMaxWebPFileSize) return kErrorImageTooLarge;

    data.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG total = 0;
    while (total < data.size()) {
        ULONG read = 0;
        hr = stream->Read(data.data() + total, static_cast<ULONG>(data.size() - total), &read);
        if (FAILED(hr)) return hr;
        if (read == 0) return kErrorCorruptImage;
        total += read;
    }
    return S_OK;
}

HRESULT DecodeWebP(IWICImagingFactory* factory, IStream* stream, Image& image) {
    std::vector<BYTE> data;
    HRESULT hr = ReadWholeStream(stream, data);
    if (FAILED(hr)) return hr;

    const WebPData webpData{data.data(), data.size()};
    WebPDemuxer* demux = WebPDemux(&webpData);
    if (!demux) return kErrorCorruptImage;

    const UINT canvasWidth = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
    const UINT canvasHeight = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
    const uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
    WebPIterator frame{};
    ComPtr<IWICBitmap> bitmap;
    hr = CheckDimensions(canvasWidth, canvasHeight);
    if (SUCCEEDED(hr) && !WebPDemuxGetFrame(demux, 1, &frame)) hr = kErrorCorruptImage;
    if (SUCCEEDED(hr) && (frame.x_offset + frame.width > static_cast<int>(canvasWidth) ||
                          frame.y_offset + frame.height > static_cast<int>(canvasHeight))) {
        hr = kErrorCorruptImage;
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmap(canvasWidth, canvasHeight, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapCacheOnLoad, &bitmap);
    }
    if (SUCCEEDED(hr)) {
        // Decode straight into the bitmap memory. An animation frame can be smaller
        // than the canvas, in which case the rest stays transparent.
        const WICRect all{0, 0, static_cast<INT>(canvasWidth), static_cast<INT>(canvasHeight)};
        ComPtr<IWICBitmapLock> lock;
        UINT stride = 0;
        UINT size = 0;
        BYTE* pixels = nullptr;
        hr = bitmap->Lock(&all, WICBitmapLockWrite, &lock);
        if (SUCCEEDED(hr)) hr = lock->GetStride(&stride);
        if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&size, &pixels);
        if (SUCCEEDED(hr)) {
            std::memset(pixels, 0, size);
            const size_t offset = static_cast<size_t>(frame.y_offset) * stride + frame.x_offset * 4u;
            if (!WebPDecodeBGRAInto(frame.fragment.bytes, frame.fragment.size, pixels + offset,
                                    size - offset, static_cast<int>(stride))) {
                hr = kErrorCorruptImage;
            }
        }
    }
    if (SUCCEEDED(hr) && (flags & ICCP_FLAG)) {
        WebPChunkIterator chunk{};
        if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunk)) {
            std::vector<BYTE> profile(chunk.chunk.bytes, chunk.chunk.bytes + chunk.chunk.size);
            if (IsRgbProfile(profile)) image.rgbProfile = std::move(profile);
            WebPDemuxReleaseChunkIterator(&chunk);
        }
    }
    if (SUCCEEDED(hr)) {
        image.hasAlpha = frame.has_alpha || (flags & ANIMATION_FLAG) != 0;
        image.container = GUID_ContainerFormatWebp;
        image.pixels = bitmap;
    }
    if (frame.fragment.bytes) WebPDemuxReleaseIterator(&frame);
    WebPDemuxDelete(demux);
    return hr;
}

// Icons hold several sizes of the same picture; use the largest one.
UINT LargestFrame(IWICBitmapDecoder* decoder) {
    UINT count = 0;
    decoder->GetFrameCount(&count);
    UINT best = 0;
    UINT64 bestArea = 0;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT width = 0;
        UINT height = 0;
        if (FAILED(decoder->GetFrame(i, &frame)) || FAILED(frame->GetSize(&width, &height))) continue;
        const UINT64 area = static_cast<UINT64>(width) * height;
        if (area > bestArea) {
            bestArea = area;
            best = i;
        }
    }
    return best;
}

HRESULT DecodeWithWic(IWICImagingFactory* factory, IStream* stream, Image& image) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;
    hr = decoder->GetContainerFormat(&image.container);
    if (FAILED(hr)) return hr;

    // Multi-frame files (animated GIF, multi-page TIFF) contribute their first frame.
    const UINT frameIndex = image.container == GUID_ContainerFormatIco ? LargestFrame(decoder.Get()) : 0;
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(frameIndex, &frame);
    if (FAILED(hr)) return hr;

    UINT width = 0;
    UINT height = 0;
    WICPixelFormatGUID pixelFormat{};
    hr = frame->GetSize(&width, &height);
    if (SUCCEEDED(hr)) hr = CheckDimensions(width, height);
    if (SUCCEEDED(hr)) hr = frame->GetPixelFormat(&pixelFormat);
    if (FAILED(hr)) return hr;

    if (FAILED(frame->GetResolution(&image.dpiX, &image.dpiY))) image.dpiX = image.dpiY = 96.0;
    SanitizeResolution(image.dpiX);
    SanitizeResolution(image.dpiY);
    image.hasAlpha = PixelFormatHasAlpha(factory, pixelFormat);
    image.rgbProfile = ReadRgbProfile(factory, frame.Get());

    const WICBitmapTransformOptions transform = OrientationTransform(ReadOrientation(frame.Get()));
    if (transform == WICBitmapTransformRotate0) {
        image.pixels = frame;
        return S_OK;
    }
    ComPtr<IWICBitmapFlipRotator> rotator;
    hr = factory->CreateBitmapFlipRotator(&rotator);
    if (SUCCEEDED(hr)) hr = rotator->Initialize(frame.Get(), transform);
    if (FAILED(hr)) return hr;
    image.pixels = rotator;
    // Rotate90 and Rotate270 both set this bit; they swap the axes.
    if (transform & WICBitmapTransformRotate90) std::swap(image.dpiX, image.dpiY);
    return S_OK;
}

}  // namespace

HRESULT DecodeImage(IWICImagingFactory* factory, const std::wstring& path, Image& image) {
    ComPtr<IStream> stream;
    HRESULT hr = SHCreateStreamOnFileEx(ToApiPath(path).c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                        FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
    if (FAILED(hr)) return hr;

    BYTE header[12] = {};
    ULONG read = 0;
    hr = stream->Read(header, sizeof(header), &read);
    if (FAILED(hr)) return hr;
    const LARGE_INTEGER start{};
    hr = stream->Seek(start, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;

    const bool isWebP = read == sizeof(header) && std::memcmp(header, "RIFF", 4) == 0 &&
                        std::memcmp(header + 8, "WEBP", 4) == 0;
    return isWebP ? DecodeWebP(factory, stream.Get(), image) : DecodeWithWic(factory, stream.Get(), image);
}

bool IsInFormat(const Image& image, Format format) {
    switch (format) {
        case Format::Png: return image.container == GUID_ContainerFormatPng;
        case Format::Jpeg: return image.container == GUID_ContainerFormatJpeg;
        case Format::WebP: return image.container == GUID_ContainerFormatWebp;
        case Format::Bmp: return image.container == GUID_ContainerFormatBmp;
        case Format::Gif: return image.container == GUID_ContainerFormatGif;
        case Format::Tiff: return image.container == GUID_ContainerFormatTiff;
        case Format::Ico: return image.container == GUID_ContainerFormatIco;
    }
    return false;
}

}  // namespace iris
