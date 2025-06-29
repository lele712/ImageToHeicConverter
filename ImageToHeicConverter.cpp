#include <iostream>

#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <shlwapi.h>
#include <pathcch.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "pathcch.lib")

using Microsoft::WRL::ComPtr;

// 新增：定义转换模式的枚举，让代码更清晰
enum class ConversionMode {
    ToHeic,
    ToJpeg
};

// 函数前向声明
HRESULT ConvertImage(const WCHAR* inputPath, const WCHAR* outputPath, float quality, const GUID& targetEncoderGuid); // 改造后的通用转换函数
void ShowHelp(const WCHAR* appName);
bool IsSupportedInputFile(const std::wstring& fileName, ConversionMode mode); // 改造后的文件支持判断函数
bool CheckHevcEncoderAvailability();

std::mutex console_mutex;

// === 修改：Worker线程函数，使其更通用 ===
void Worker(
    const std::vector<std::wstring>* filesToProcess,
    const std::wstring* outputDir,
    std::atomic<size_t>* task_index,
    std::atomic<int>* success_count,
    std::atomic<int>* fail_count,
    float quality,
    const WCHAR* targetExtension,       // 新增：接收目标文件后缀
    const GUID* pTargetEncoderGuid      // 新增：接收目标编码器GUID
) {
    HRESULT hr_com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr_com)) {
        std::lock_guard<std::mutex> lock(console_mutex);
        wprintf(L"Error: Failed to initialize COM in worker thread. HR=0x%X\n", hr_com);
        return;
    }

    while (true) {
        size_t index = task_index->fetch_add(1);
        if (index >= filesToProcess->size()) {
            break;
        }

        const std::wstring& inputFile = (*filesToProcess)[index];
        const WCHAR* fileName = PathFindFileNameW(inputFile.c_str());

        WCHAR finalOutPath[MAX_PATH];
        PathCchCombine(finalOutPath, MAX_PATH, outputDir->c_str(), fileName);
        PathCchRenameExtension(finalOutPath, MAX_PATH, targetExtension); // 使用传入的目标后缀

        std::wstring tempOutPath = std::wstring(finalOutPath) + L".tmp";

        // 调用通用的转换函数
        HRESULT hr = ConvertImage(inputFile.c_str(), tempOutPath.c_str(), quality, *pTargetEncoderGuid);

        bool final_success = false;
        std::wstring status_message;

        if (SUCCEEDED(hr)) {
            DeleteFileW(finalOutPath);
            if (MoveFileW(tempOutPath.c_str(), finalOutPath)) {
                final_success = true;
                status_message = L"OK";
            }
            else {
                DWORD lastError = GetLastError();
                if (lastError == ERROR_ACCESS_DENIED) {
                    status_message = L"FAILED (Permission Denied to Finalize)";
                }
                else {
                    wchar_t buffer[64];
                    swprintf_s(buffer, 64, L"FAILED (Move Error: %lu)", lastError);
                    status_message = buffer;
                }
                DeleteFileW(tempOutPath.c_str());
            }
        }
        else {
            if (hr == E_ACCESSDENIED) { status_message = L"FAILED (Permission Denied)"; }
            else if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL)) { status_message = L"FAILED (Disk Full)"; }
            else if (hr == WINCODEC_ERR_BADHEADER) { status_message = L"FAILED (Corrupt Input File)"; }
            else {
                wchar_t buffer[64];
                swprintf_s(buffer, 64, L"FAILED (Code: 0x%08X)", static_cast<unsigned int>(hr));
                status_message = buffer;
            }
            DeleteFileW(tempOutPath.c_str());
        }

        {
            std::lock_guard<std::mutex> lock(console_mutex);
            // 优化输出，显示转换方向
            wprintf(L"[%zu/%zu] Converting %s -> %s ... %s\n",
                index + 1, filesToProcess->size(),
                PathFindFileNameW(inputFile.c_str()),
                PathFindFileNameW(finalOutPath),
                status_message.c_str());
        }

        if (final_success) {
            success_count->fetch_add(1);
        }
        else {
            fail_count->fetch_add(1);
        }
    }

    CoUninitialize();
}

// === 修改：主函数wmain，负责模式调度 ===
int wmain(int argc, wchar_t* argv[]) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { wprintf(L"Failed to initialize COM. HR = 0x%X\n", hr); return 1; }

    if (!CheckHevcEncoderAvailability()) {
        wprintf(L"\nError: HEIC/HEVC component is unavailable or not fully functional on this system.\n");
        wprintf(L"This program requires the official \"HEVC Video Extensions\" to read/write HEIC files.\n\n");
        wprintf(L"Please install it from the Microsoft Store. Trying the free version first is recommended:\n");
        wprintf(L"1. (Free) HEVC Video Extensions from Device Manufacturer:\n   https://www.microsoft.com/store/productId/9N4WGH0Z6VHQ\n\n");
        wprintf(L"2. (Paid Alternative) HEVC Video Extensions:\n   https://www.microsoft.com/store/productId/9NMZLZ57R3T7\n\n");
        wprintf(L"After installation, please run this program again.\n");
        CoUninitialize();
        system("pause");
        return 1;
    }

    if (argc <= 1) { ShowHelp(argv[0]); CoUninitialize(); return 1; }

    std::vector<std::wstring> inputPaths;
    std::wstring outputDir;
    float quality = -1.0f;
    ConversionMode mode = ConversionMode::ToHeic; // 默认模式：转为HEIC

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"-h" || arg == L"--help") { ShowHelp(argv[0]); CoUninitialize(); return 0; }
        if (arg == L"-i" || arg == L"--input") { while (i + 1 < argc && argv[i + 1][0] != L'-') { inputPaths.push_back(argv[++i]); } }
        else if (arg == L"-o" || arg == L"--output") { if (i + 1 < argc) { outputDir = argv[++i]; } }
        else if (arg == L"-q" || arg == L"--quality") { if (i + 1 < argc) { try { quality = std::stof(argv[++i]) / 100.0f; if (quality < 0.0f || quality > 1.0f) { wprintf(L"Warning: Quality must be between 0 and 100. Using default quality.\n"); quality = -1.0f; } } catch (const std::exception&) { wprintf(L"Warning: Invalid quality value. It must be a number. Using default quality.\n"); quality = -1.0f; } } }
        // 新增对 --to 参数的解析
        else if (arg == L"--to") {
            if (i + 1 < argc) {
                std::wstring format = argv[++i];
                std::transform(format.begin(), format.end(), format.begin(), ::tolower); // 转为小写，方便比较
                if (format == L"jpeg" || format == L"jpg") {
                    mode = ConversionMode::ToJpeg;
                }
            }
        }
    }

    if (inputPaths.empty() || outputDir.empty()) { wprintf(L"\nError: Both input and output paths must be specified.\n\n"); ShowHelp(argv[0]); CoUninitialize(); return 1; }
    if (GetFileAttributesW(outputDir.c_str()) == INVALID_FILE_ATTRIBUTES) { if (!CreateDirectoryW(outputDir.c_str(), NULL)) { wprintf(L"Error: Failed to create output directory: %s\n", outputDir.c_str()); CoUninitialize(); return 1; } }

    // 根据模式确定转换参数
    const WCHAR* targetExtension;
    GUID targetEncoderGuid;

    if (mode == ConversionMode::ToJpeg) {
        targetExtension = L".jpg";
        targetEncoderGuid = GUID_ContainerFormatJpeg;
        wprintf(L"Mode: HEIC -> JPEG\n");
    }
    else { // 默认 ToHeic
        targetExtension = L".heic";
        targetEncoderGuid = GUID_ContainerFormatHeif;
        wprintf(L"Mode: Image -> HEIC\n");
    }

    std::vector<std::wstring> filesToProcess;
    for (const auto& path : inputPaths) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) { wprintf(L"Warning: Input path not found, skipping: %s\n", path.c_str()); continue; }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring searchPath = path + L"\\*";
            WIN32_FIND_DATAW findData;
            HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
            if (hFind != INVALID_HANDLE_VALUE) {
                do { if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { std::wstring fullPath = path + L"\\" + findData.cFileName; if (IsSupportedInputFile(fullPath, mode)) { filesToProcess.push_back(fullPath); } } } while (FindNextFileW(hFind, &findData) != 0);
                FindClose(hFind);
            }
        }
        else { if (IsSupportedInputFile(path, mode)) { filesToProcess.push_back(path); } else { wprintf(L"Warning: Unsupported input file for this mode, skipping: %s\n", path.c_str()); } }
    }

    if (filesToProcess.empty()) { wprintf(L"\nNo supported image files found to process for the selected mode.\n"); CoUninitialize(); return 0; }

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    const unsigned int num_threads = std::max(1u, (unsigned int)sysInfo.dwNumberOfProcessors);
    wprintf(L"\nFound %zu files. Starting conversion on %u threads...\n\n", filesToProcess.size(), num_threads);

    std::atomic<size_t> task_index(0);
    std::atomic<int> success_count(0);
    std::atomic<int> fail_count(0);

    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < num_threads; ++i) { threads.emplace_back(Worker, &filesToProcess, &outputDir, &task_index, &success_count, &fail_count, quality, targetExtension, &targetEncoderGuid); }
    for (auto& t : threads) { if (t.joinable()) { t.join(); } }

    wprintf(L"\nConversion finished. %d successful, %d failed.\n", success_count.load(), fail_count.load());
    CoUninitialize();
    return 0;
}


// --- 辅助函数实现 ---

bool CheckHevcEncoderAvailability() {
    ComPtr<IWICImagingFactory> pFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return false;
    ComPtr<IWICBitmapEncoder> pEncoder;
    hr = pFactory->CreateEncoder(GUID_ContainerFormatHeif, NULL, &pEncoder);
    if (FAILED(hr)) return false;
    ComPtr<IStream> pStream;
    hr = CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    if (FAILED(hr)) return false;
    hr = pEncoder->Initialize(pStream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;
    ComPtr<IWICBitmapFrameEncode> pFrameEncode;
    ComPtr<IPropertyBag2> pPropertyBag;
    hr = pEncoder->CreateNewFrame(&pFrameEncode, &pPropertyBag);
    if (FAILED(hr)) return false;
    hr = pFrameEncode->Initialize(pPropertyBag.Get());
    if (FAILED(hr)) return false;
    ComPtr<IWICBitmap> pBitmap;
    hr = pFactory->CreateBitmap(1, 1, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &pBitmap);
    if (FAILED(hr)) return false;
    hr = pFrameEncode->WriteSource(pBitmap.Get(), NULL);
    if (FAILED(hr)) return false;
    hr = pFrameEncode->Commit();
    if (FAILED(hr)) return false;
    return true;
}

// === 修改：ShowHelp函数，增加新参数说明 ===
void ShowHelp(const WCHAR* appName) {
    wprintf(L"HEIC Converter - Converts images to/from HEIC using Windows API.\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  %s -i <inputs...> -o <output_dir> [--to format] [-q quality]\n\n", PathFindFileNameW(appName));
    wprintf(L"Arguments:\n");
    wprintf(L"  -i, --input   One or more input files or directories.\n");
    wprintf(L"  -o, --output  The directory where converted files will be saved.\n");
    wprintf(L"  --to <format> (Optional) Specify output format. Can be 'jpeg' or 'heic'.\n");
    wprintf(L"                Default is 'heic'.\n");
    wprintf(L"  -q, --quality (Optional) Set the quality of the output image (0-100).\n");
    wprintf(L"                Default is a high quality setting.\n");
    wprintf(L"  -h, --help    Show this help message.\n\n");
    wprintf(L"Examples:\n");
    wprintf(L"  1. Convert JPG/PNG to HEIC (default mode):\n");
    wprintf(L"     %s -i C:\\pics -o D:\\HEIC_Output\n\n", PathFindFileNameW(appName));
    wprintf(L"  2. Convert HEIC to JPEG with 90 quality:\n");
    wprintf(L"     %s -i C:\\heic_pics -o D:\\JPEG_Output --to jpeg -q 90\n", PathFindFileNameW(appName));
}

// === 修改：IsSupportedImageFile函数，根据模式判断输入 ===
bool IsSupportedInputFile(const std::wstring& fileName, ConversionMode mode) {
    std::wstring extension = PathFindExtensionW(fileName.c_str());
    if (extension.empty()) return false;

    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (mode == ConversionMode::ToHeic) {
        const std::vector<std::wstring> supportedExtensions = { L".jpg", L".jpeg", L".png", L".bmp", L".tiff", L".gif" };
        for (const auto& ext : supportedExtensions) {
            if (extension == ext) return true;
        }
    }
    else if (mode == ConversionMode::ToJpeg) {
        // 根据要求，转为JPEG时，输入必须是HEIC
        if (extension == L".heic") {
            return true;
        }
    }
    return false;
}

// === 重构：通用的核心转换函数 (替换掉旧的ConvertImageToHeic) ===
HRESULT ConvertImage(const WCHAR* inputPath, const WCHAR* outputPath, float quality, const GUID& targetEncoderGuid) {
    HRESULT hr = S_OK;
    ComPtr<IWICImagingFactory> pFactory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapDecoder> pDecoder;
    hr = pFactory->CreateDecoderFromFilename(inputPath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameDecode> pFrameDecode;
    hr = pDecoder->GetFrame(0, &pFrameDecode);
    if (FAILED(hr)) return hr;

    ComPtr<IWICStream> pStream;
    hr = pFactory->CreateStream(&pStream);
    if (FAILED(hr)) return hr;
    hr = pStream->InitializeFromFilename(outputPath, GENERIC_WRITE);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapEncoder> pEncoder;
    // 使用传入的GUID来创建目标编码器
    hr = pFactory->CreateEncoder(targetEncoderGuid, NULL, &pEncoder);
    if (FAILED(hr)) return hr;

    hr = pEncoder->Initialize(pStream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameEncode> pFrameEncode;
    ComPtr<IPropertyBag2> pPropertyBag;
    hr = pEncoder->CreateNewFrame(&pFrameEncode, &pPropertyBag);
    if (FAILED(hr)) return hr;

    if (quality >= 0.0f && quality <= 1.0f) {
        PROPBAG2 option = { 0 };
        wchar_t propName[] = L"ImageQuality";
        option.pstrName = propName;
        VARIANT varValue;
        VariantInit(&varValue);
        varValue.vt = VT_R4;
        varValue.fltVal = quality;
        hr = pPropertyBag->Write(1, &option, &varValue);
        if (FAILED(hr)) { /* Warning can be logged here if needed */ }
    }

    hr = pFrameEncode->Initialize(pPropertyBag.Get());
    if (FAILED(hr)) return hr;

    hr = pFrameEncode->WriteSource(pFrameDecode.Get(), NULL);
    if (FAILED(hr)) return hr;

    hr = pFrameEncode->Commit();
    if (FAILED(hr)) return hr;

    hr = pEncoder->Commit();
    return hr;
}
