#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "formats.h"

namespace iris {

// A decoded source image, ready to be encoded into another format.
struct Image {
    // First frame (largest entry for icons) with EXIF orientation already applied.
    // Pixels are decoded lazily by WIC while the encoder pulls them.
    Microsoft::WRL::ComPtr<IWICBitmapSource> pixels;
    GUID container = GUID_NULL;     // Actual format of the file, from its content.
    bool hasAlpha = false;
    std::vector<BYTE> rgbProfile;   // Embedded ICC profile, kept only for RGB profiles.
    double dpiX = 96.0;
    double dpiY = 96.0;
};

// Opens and decodes an image file. The file is only read, never modified.
HRESULT DecodeImage(IWICImagingFactory* factory, const std::wstring& path, Image& image);

// True when the file content is already in the given format.
bool IsInFormat(const Image& image, Format format);

// Encodes the image into the stream using the Iris defaults for the format.
HRESULT EncodeImage(IWICImagingFactory* factory, const Image& image, Format format, IStream* output);

}  // namespace iris
