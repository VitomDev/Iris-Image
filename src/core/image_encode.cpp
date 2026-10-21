// Encoding: WIC for PNG, JPEG, BMP, GIF and TIFF; libwebp for WebP (WIC has no
// WebP encoder); ICO is written here as a multi-size icon with PNG entries.

#include <shlwapi.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <cstring>

#include "errors.h"
#include "image.h"

using Microsoft::WRL::ComPtr;

namespace iris {
namespace {

constexpr float kJpegQuality = 0.90f;
constexpr float kWebPQuality = 90.0f;
constexpr int kWebPMaxDimension = 16383;
constexpr UINT kIconSizes[] = {16, 24, 32, 48, 64, 128, 256};

struct FrameSettings {
    GUID container;
    WICPixelFormatGUID pixelFormat;
    const wchar_t* optionName = nullptr;  // One encoder option is all any format needs.
    VARIANT optionValue{};
    bool embedProfile = false;
    IWICPalette* palette = nullptr;
    int transparentIndex = -1;  // GIF only.
};

HRESULT ConvertPixels(IWICImagingFactory* factory, IWICBitmapSource* source,
                      REFWICPixelFormatGUID format, ComPtr<IWICBitmapSource>& result) {
    WICPixelFormatGUID current{};
    HRESULT hr = source->GetPixelFormat(&current);
    if (FAILED(hr)) return hr;
    if (current == format) {
        result = source;
        return S_OK;
    }
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source, format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeMedianCut);
    }
    if (SUCCEEDED(hr)) result = converter;
    return hr;
}

// Decodes the whole image into memory once, for encoders that read it repeatedly.
HRESULT LoadBgra(IWICImagingFactory* factory, IWICBitmapSource* source, REFWICPixelFormatGUID format,
                 ComPtr<IWICBitmap>& bitmap) {
    ComPtr<IWICBitmapSource> converted;
    HRESULT hr = ConvertPixels(factory, source, format, converted);
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromSource(converted.Get(), WICBitmapCacheOnLoad, &bitmap);
    return hr;
}

bool IsIndexed(IWICImagingFactory* factory, REFWICPixelFormatGUID format) {
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo2> pixelInfo;
    WICPixelFormatNumericRepresentation representation{};
    return SUCCEEDED(factory->CreateComponentInfo(format, &info)) && SUCCEEDED(info.As(&pixelInfo)) &&
           SUCCEEDED(pixelInfo->GetNumericRepresentation(&representation)) &&
           representation == WICPixelFormatNumericRepresentationIndexed;
}

// PNG and TIFF keep the source pixel format (16-bit, grayscale...) when the
// encoder supports it; everything else is written as 8-bit BGR(A).
WICPixelFormatGUID PreferredPixelFormat(IWICImagingFactory* factory, const Image& image, bool keepNative) {
    WICPixelFormatGUID native{};
    if (keepNative && SUCCEEDED(image.pixels->GetPixelFormat(&native)) && !IsIndexed(factory, native)) {
        return native;
    }
    return image.hasAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
}

HRESULT SetGifTransparency(IWICBitmapFrameEncode* frame, int index) {
    ComPtr<IWICMetadataQueryWriter> writer;
    HRESULT hr = frame->GetMetadataQueryWriter(&writer);
    if (FAILED(hr)) return hr;
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_TRUE;
    hr = writer->SetMetadataByName(L"/grctlext/TransparencyFlag", &value);
    if (SUCCEEDED(hr)) {
        value.vt = VT_UI1;
        value.bVal = static_cast<BYTE>(index);
        hr = writer->SetMetadataByName(L"/grctlext/TransparentColorIndex", &value);
    }
    return hr;
}

HRESULT WriteFrame(IWICImagingFactory* factory, IWICBitmapSource* source, const Image& image,
                   FrameSettings settings, IStream* output) {
    UINT width = 0;
    UINT height = 0;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    HRESULT hr = source->GetSize(&width, &height);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(settings.container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(output, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr) && settings.optionName) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(settings.optionName);
        hr = options->Write(1, &option, &settings.optionValue);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    if (SUCCEEDED(hr)) hr = frame->SetResolution(image.dpiX, image.dpiY);

    // The encoder may answer with the closest pixel format it supports.
    WICPixelFormatGUID format = settings.pixelFormat;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && settings.palette) hr = frame->SetPalette(settings.palette);
    if (SUCCEEDED(hr) && settings.embedProfile && !image.rgbProfile.empty()) {
        // A profile the encoder cannot store is not worth failing the conversion for.
        ComPtr<IWICColorContext> profile;
        if (SUCCEEDED(factory->CreateColorContext(&profile)) &&
            SUCCEEDED(profile->InitializeFromMemory(image.rgbProfile.data(),
                                                    static_cast<UINT>(image.rgbProfile.size())))) {
            IWICColorContext* contexts[] = {profile.Get()};
            frame->SetColorContexts(1, contexts);
        }
    }
    if (SUCCEEDED(hr) && settings.transparentIndex >= 0) {
        hr = SetGifTransparency(frame.Get(), settings.transparentIndex);
    }

    ComPtr<IWICBitmapSource> pixels;
    if (SUCCEEDED(hr)) hr = ConvertPixels(factory, source, format, pixels);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(pixels.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

HRESULT LockPixels(IWICBitmap* bitmap, ComPtr<IWICBitmapLock>& lock, UINT& stride, BYTE*& data) {
    UINT width = 0;
    UINT height = 0;
    UINT size = 0;
    HRESULT hr = bitmap->GetSize(&width, &height);
    const WICRect all{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    if (SUCCEEDED(hr)) hr = bitmap->Lock(&all, WICBitmapLockRead | WICBitmapLockWrite, &lock);
    if (SUCCEEDED(hr)) hr = lock->GetStride(&stride);
    if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&size, &data);
    return hr;
}

HRESULT EncodeJpeg(IWICImagingFactory* factory, const Image& image, IStream* output) {
    FrameSettings settings{GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR};
    settings.optionName = L"ImageQuality";
    settings.optionValue.vt = VT_R4;
    settings.optionValue.fltVal = kJpegQuality;
    settings.embedProfile = true;
    if (!image.hasAlpha) return WriteFrame(factory, image.pixels.Get(), image, settings, output);

    // JPEG has no transparency: composite onto white. With premultiplied alpha
    // that is simply color + (255 - alpha) per channel.
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = LoadBgra(factory, image.pixels.Get(), GUID_WICPixelFormat32bppPBGRA, bitmap);
    if (FAILED(hr)) return hr;
    {
        ComPtr<IWICBitmapLock> lock;
        UINT stride = 0;
        BYTE* data = nullptr;
        UINT width = 0;
        UINT height = 0;
        hr = bitmap->GetSize(&width, &height);
        if (SUCCEEDED(hr)) hr = LockPixels(bitmap.Get(), lock, stride, data);
        if (FAILED(hr)) return hr;
        for (UINT y = 0; y < height; ++y) {
            BYTE* pixel = data + static_cast<size_t>(y) * stride;
            for (UINT x = 0; x < width; ++x, pixel += 4) {
                const BYTE background = static_cast<BYTE>(255 - pixel[3]);
                pixel[0] = static_cast<BYTE>(pixel[0] + background);
                pixel[1] = static_cast<BYTE>(pixel[1] + background);
                pixel[2] = static_cast<BYTE>(pixel[2] + background);
                pixel[3] = 255;
            }
        }
    }
    return WriteFrame(factory, bitmap.Get(), image, settings, output);
}

HRESULT EncodeGif(IWICImagingFactory* factory, const Image& image, IStream* output) {
    // Building the palette and mapping pixels both read the image, so decode it once.
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICPalette> palette;
    ComPtr<IWICFormatConverter> indexed;
    HRESULT hr = LoadBgra(factory, image.pixels.Get(), GUID_WICPixelFormat32bppBGRA, bitmap);
    if (SUCCEEDED(hr)) hr = factory->CreatePalette(&palette);
    if (SUCCEEDED(hr)) hr = palette->InitializeFromBitmap(bitmap.Get(), 256, image.hasAlpha);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&indexed);
    if (SUCCEEDED(hr)) {
        // Pixels under 50% opacity map to the transparent palette entry.
        hr = indexed->Initialize(bitmap.Get(), GUID_WICPixelFormat8bppIndexed,
                                 WICBitmapDitherTypeErrorDiffusion, palette.Get(), 50.0,
                                 WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) return hr;

    FrameSettings settings{GUID_ContainerFormatGif, GUID_WICPixelFormat8bppIndexed};
    settings.palette = palette.Get();
    if (image.hasAlpha) {
        WICColor colors[256] = {};
        UINT count = 0;
        hr = palette->GetColors(256, colors, &count);
        if (FAILED(hr)) return hr;
        for (UINT i = 0; i < count; ++i) {
            if ((colors[i] >> 24) == 0) {
                settings.transparentIndex = static_cast<int>(i);
                break;
            }
        }
    }
    return WriteFrame(factory, indexed.Get(), image, settings, output);
}

HRESULT WriteAll(IStream* output, const void* data, size_t size) {
    const BYTE* bytes = static_cast<const BYTE*>(data);
    while (size > 0) {
        const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size, 1u << 30));
        ULONG written = 0;
        const HRESULT hr = output->Write(bytes, chunk, &written);
        if (FAILED(hr)) return hr;
        if (written == 0) return STG_E_MEDIUMFULL;
        bytes += written;
        size -= written;
    }
    return S_OK;
}

HRESULT EncodeWebP(IWICImagingFactory* factory, const Image& image, IStream* output) {
    UINT width = 0;
    UINT height = 0;
    HRESULT hr = image.pixels->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width > kWebPMaxDimension || height > kWebPMaxDimension) return kErrorTooLargeForWebP;

    WebPConfig config;
    WebPPicture picture;
    if (!WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, kWebPQuality) || !WebPPictureInit(&picture)) {
        return E_UNEXPECTED;
    }
    picture.width = static_cast<int>(width);
    picture.height = static_cast<int>(height);

    {
        // Lossy WebP is YUV inside: importing BGR(A) converts straight into it,
        // so this temporary buffer is the only full-size BGR copy and is freed
        // before encoding starts.
        const UINT bytesPerPixel = image.hasAlpha ? 4 : 3;
        const UINT stride = width * bytesPerPixel;
        ComPtr<IWICBitmapSource> pixels;
        hr = ConvertPixels(factory, image.pixels.Get(),
                           image.hasAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR,
                           pixels);
        if (FAILED(hr)) return hr;
        std::vector<BYTE> buffer(static_cast<size_t>(stride) * height);
        hr = pixels->CopyPixels(nullptr, stride, static_cast<UINT>(buffer.size()), buffer.data());
        if (FAILED(hr)) return hr;
        const int imported = image.hasAlpha
                                 ? WebPPictureImportBGRA(&picture, buffer.data(), static_cast<int>(stride))
                                 : WebPPictureImportBGR(&picture, buffer.data(), static_cast<int>(stride));
        if (!imported) return E_OUTOFMEMORY;
    }

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;
    const int encoded = WebPEncode(&config, &picture);
    const WebPEncodingError encodeError = picture.error_code;
    WebPPictureFree(&picture);
    if (!encoded) {
        WebPMemoryWriterClear(&writer);
        return encodeError == VP8_ENC_ERROR_OUT_OF_MEMORY ? E_OUTOFMEMORY : E_FAIL;
    }

    if (image.rgbProfile.empty()) {
        hr = WriteAll(output, writer.mem, writer.size);
    } else {
        // Wrap the bitstream in an extended container carrying the ICC profile.
        WebPMux* mux = WebPMuxNew();
        const WebPData bitstream{writer.mem, writer.size};
        const WebPData profile{image.rgbProfile.data(), image.rgbProfile.size()};
        WebPData assembled{};
        hr = mux && WebPMuxSetImage(mux, &bitstream, 0) == WEBP_MUX_OK &&
                     WebPMuxSetChunk(mux, "ICCP", &profile, 0) == WEBP_MUX_OK &&
                     WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK
                 ? WriteAll(output, assembled.bytes, assembled.size)
                 : E_OUTOFMEMORY;
        WebPDataClear(&assembled);
        WebPMuxDelete(mux);
    }
    WebPMemoryWriterClear(&writer);
    return hr;
}

HRESULT StreamBytes(IStream* stream, std::vector<BYTE>& bytes) {
    STATSTG stat{};
    HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    bytes.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    const LARGE_INTEGER start{};
    hr = stream->Seek(start, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (SUCCEEDED(hr)) hr = stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
    if (SUCCEEDED(hr) && read != bytes.size()) hr = E_FAIL;
    return hr;
}

// Renders one square icon entry: the image scaled to fit, centered on transparency.
HRESULT RenderIconEntry(IWICImagingFactory* factory, IWICBitmap* source, UINT size, std::vector<BYTE>& png) {
    UINT width = 0;
    UINT height = 0;
    HRESULT hr = source->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    const UINT longest = std::max(width, height);
    const UINT scaledWidth = std::max(1u, static_cast<UINT>((static_cast<UINT64>(width) * size + longest / 2) / longest));
    const UINT scaledHeight = std::max(1u, static_cast<UINT>((static_cast<UINT64>(height) * size + longest / 2) / longest));

    const UINT stride = size * 4;
    std::vector<BYTE> square(static_cast<size_t>(stride) * size, 0);
    const size_t offset = static_cast<size_t>((size - scaledHeight) / 2) * stride + (size - scaledWidth) / 2 * 4;

    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) {
        hr = scaler->Initialize(source, scaledWidth, scaledHeight, WICBitmapInterpolationModeHighQualityCubic);
    }
    if (SUCCEEDED(hr)) {
        hr = scaler->CopyPixels(nullptr, stride, static_cast<UINT>(square.size() - offset), square.data() + offset);
    }

    ComPtr<IWICBitmap> entry;
    ComPtr<IStream> stream;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateBitmapFromMemory(size, size, GUID_WICPixelFormat32bppBGRA, stride,
                                             static_cast<UINT>(square.size()), square.data(), &entry);
    }
    if (SUCCEEDED(hr)) {
        stream.Attach(SHCreateMemStream(nullptr, 0));
        if (!stream) hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        Image entryImage;
        entryImage.dpiX = entryImage.dpiY = 96.0;
        hr = WriteFrame(factory, entry.Get(), entryImage,
                        FrameSettings{GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA}, stream.Get());
    }
    if (SUCCEEDED(hr)) hr = StreamBytes(stream.Get(), png);
    return hr;
}

#pragma pack(push, 1)
struct IconDirectory {
    WORD reserved;
    WORD type;   // 1 = icon.
    WORD count;
};
struct IconDirectoryEntry {
    BYTE width;   // 0 means 256.
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytesInResource;
    DWORD imageOffset;
};
#pragma pack(pop)

// Writes a standard .ico with the common Windows icon sizes up to the size of
// the source (never upscaled), each stored as a 32-bit PNG entry.
HRESULT EncodeIco(IWICImagingFactory* factory, const Image& image, IStream* output) {
    ComPtr<IWICBitmap> bitmap;
    UINT width = 0;
    UINT height = 0;
    HRESULT hr = LoadBgra(factory, image.pixels.Get(), GUID_WICPixelFormat32bppBGRA, bitmap);
    if (SUCCEEDED(hr)) hr = bitmap->GetSize(&width, &height);
    if (FAILED(hr)) return hr;

    const UINT longest = std::max(width, height);
    std::vector<UINT> sizes;
    for (UINT size : kIconSizes) {
        if (size <= longest) sizes.push_back(size);
    }
    if (sizes.empty()) sizes.push_back(longest);

    std::vector<std::vector<BYTE>> entries(sizes.size());
    for (size_t i = 0; i < sizes.size() && SUCCEEDED(hr); ++i) {
        hr = RenderIconEntry(factory, bitmap.Get(), sizes[i], entries[i]);
    }
    if (FAILED(hr)) return hr;

    const IconDirectory directory{0, 1, static_cast<WORD>(sizes.size())};
    hr = WriteAll(output, &directory, sizeof(directory));
    DWORD offset = static_cast<DWORD>(sizeof(IconDirectory) + sizes.size() * sizeof(IconDirectoryEntry));
    for (size_t i = 0; i < sizes.size() && SUCCEEDED(hr); ++i) {
        const BYTE dimension = sizes[i] >= 256 ? 0 : static_cast<BYTE>(sizes[i]);
        const IconDirectoryEntry entry{dimension, dimension, 0, 0, 1, 32,
                                       static_cast<DWORD>(entries[i].size()), offset};
        hr = WriteAll(output, &entry, sizeof(entry));
        offset += entry.bytesInResource;
    }
    for (size_t i = 0; i < entries.size() && SUCCEEDED(hr); ++i) {
        hr = WriteAll(output, entries[i].data(), entries[i].size());
    }
    return hr;
}

}  // namespace

HRESULT EncodeImage(IWICImagingFactory* factory, const Image& image, Format format, IStream* output) {
    switch (format) {
        case Format::Png: {
            FrameSettings settings{GUID_ContainerFormatPng, PreferredPixelFormat(factory, image, true)};
            settings.embedProfile = true;
            return WriteFrame(factory, image.pixels.Get(), image, settings, output);
        }
        case Format::Tiff: {
            FrameSettings settings{GUID_ContainerFormatTiff, PreferredPixelFormat(factory, image, true)};
            settings.optionName = L"TiffCompressionMethod";
            settings.optionValue.vt = VT_UI1;
            settings.optionValue.bVal = WICTiffCompressionZIP;  // Lossless and widely supported.
            settings.embedProfile = true;
            return WriteFrame(factory, image.pixels.Get(), image, settings, output);
        }
        case Format::Bmp: {
            FrameSettings settings{GUID_ContainerFormatBmp, PreferredPixelFormat(factory, image, false)};
            if (image.hasAlpha) {
                settings.optionName = L"EnableV5Header32bppBGRA";
                settings.optionValue.vt = VT_BOOL;
                settings.optionValue.boolVal = VARIANT_TRUE;
            }
            return WriteFrame(factory, image.pixels.Get(), image, settings, output);
        }
        case Format::Jpeg: return EncodeJpeg(factory, image, output);
        case Format::Gif: return EncodeGif(factory, image, output);
        case Format::WebP: return EncodeWebP(factory, image, output);
        case Format::Ico: return EncodeIco(factory, image, output);
    }
    return E_INVALIDARG;
}

}  // namespace iris
