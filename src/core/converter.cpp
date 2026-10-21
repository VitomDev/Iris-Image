#include "converter.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "image.h"
#include "output_file.h"

using Microsoft::WRL::ComPtr;

namespace iris {
namespace {

// Image codecs are CPU and memory heavy; a few threads saturate the disk and
// keep peak memory predictable on large batches.
constexpr unsigned kMaxThreads = 4;

FileResult Failure(HRESULT hr) {
    FileResult result;
    result.error = hr;
    return result;
}

FileResult ConvertWithFactory(IWICImagingFactory* factory, const std::wstring& sourcePath, Format target) {
    Image image;
    HRESULT hr = DecodeImage(factory, sourcePath, image);
    if (FAILED(hr)) return Failure(hr);
    if (IsInFormat(image, target)) {
        FileResult result;
        result.outcome = Outcome::AlreadyInFormat;
        return result;
    }

    OutputFile output;
    {
        ComPtr<IStream> stream;
        hr = output.Create(sourcePath, stream);
        if (SUCCEEDED(hr)) hr = EncodeImage(factory, image, target, stream.Get());
        if (SUCCEEDED(hr)) hr = stream->Commit(STGC_DEFAULT);
        if (FAILED(hr)) return Failure(hr);  // The temporary file is deleted by OutputFile.
    }
    image = Image{};  // Close the source file before the rename.

    FileResult result;
    hr = output.Commit(sourcePath, FormatExtension(target), result.outputPath);
    if (FAILED(hr)) return Failure(hr);
    result.outcome = Outcome::Converted;
    return result;
}

HRESULT CreateFactory(ComPtr<IWICImagingFactory>& factory) {
    return CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
}

}  // namespace

FileResult ConvertFile(const std::wstring& sourcePath, Format target) {
    ComPtr<IWICImagingFactory> factory;
    const HRESULT hr = CreateFactory(factory);
    if (FAILED(hr)) return Failure(hr);
    return ConvertWithFactory(factory.Get(), sourcePath, target);
}

std::vector<FileResult> ConvertFiles(const std::vector<std::wstring>& sourcePaths, Format target) {
    std::vector<FileResult> results(sourcePaths.size());
    std::atomic<size_t> next{0};

    auto work = [&]() {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        ComPtr<IWICImagingFactory> factory;
        // RPC_E_CHANGED_MODE: the caller's thread already has COM in another mode.
        const HRESULT hr = FAILED(init) && init != RPC_E_CHANGED_MODE ? init : CreateFactory(factory);
        for (size_t i = next++; i < sourcePaths.size(); i = next++) {
            results[i] = SUCCEEDED(hr) ? ConvertWithFactory(factory.Get(), sourcePaths[i], target) : Failure(hr);
        }
        factory.Reset();
        if (SUCCEEDED(init)) CoUninitialize();
    };

    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const size_t threadCount = std::min<size_t>({sourcePaths.size(), hardware, kMaxThreads});
    std::vector<std::thread> threads;
    for (size_t i = 1; i < threadCount; ++i) threads.emplace_back(work);
    if (!sourcePaths.empty()) work();  // The calling thread works too.
    for (std::thread& thread : threads) thread.join();
    return results;
}

}  // namespace iris
