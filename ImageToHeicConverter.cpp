#include <iostream>

// ===== 解决方案步骤 1: 定义 NOMINMAX 来防止 Windows.h 定义 min/max 宏 =====
#define NOMINMAX
// =========================================================================

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

// ===== 解决方案步骤 2: 包含 <algorithm> 头文件以使用 std::max =====
#include <algorithm>
// =================================================================

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "pathcch.lib")

using Microsoft::WRL::ComPtr;

// 函数前向声明
HRESULT ConvertImageToHeic(const WCHAR* inputPath, const WCHAR* outputPath, float quality);
void ShowHelp(const WCHAR* appName);
bool IsSupportedImageFile(const std::wstring& fileName);

// 用于保护控制台输出，防止多线程打印信息混乱
std::mutex console_mutex;

// 工作线程函数
void Worker(
    const std::vector<std::wstring>* filesToProcess,
    const std::wstring* outputDir,
    std::atomic<size_t>* task_index,
    std::atomic<int>* success_count,
    std::atomic<int>* fail_count,
    float quality
) {
    // 每个使用COM的线程都必须自己初始化
    HRESULT hr_com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr_com)) {
        std::lock_guard<std::mutex> lock(console_mutex);
        wprintf(L"Error: Failed to initialize COM in worker thread. HR=0x%X\n", hr_com);
        return;
    }

    while (true) {
        // 原子地获取并增加任务索引，这是线程安全的
        size_t index = task_index->fetch_add(1);

        // 如果索引超出任务列表范围，说明所有任务都已分配完毕
        if (index >= filesToProcess->size()) {
            break;
        }

        const std::wstring& inputFile = (*filesToProcess)[index];

        // 构建输出路径
        const WCHAR* fileName = PathFindFileNameW(inputFile.c_str());
        WCHAR outPath[MAX_PATH];
        PathCchCombine(outPath, MAX_PATH, outputDir->c_str(), fileName);
        PathCchRenameExtension(outPath, MAX_PATH, L".heic");

        // 执行转换
        HRESULT hr = ConvertImageToHeic(inputFile.c_str(), outPath, quality);

        // 使用互斥锁保护控制台输出
        {
            std::lock_guard<std::mutex> lock(console_mutex);
            wprintf(L"[%zu/%zu] Converting %s ... %s\n",
                index + 1, filesToProcess->size(), PathFindFileNameW(inputFile.c_str()),
                SUCCEEDED(hr) ? L"OK" : L"FAILED");
        }

        // 使用原子操作更新计数器
        if (SUCCEEDED(hr)) {
            success_count->fetch_add(1);
        }
        else {
            fail_count->fetch_add(1);
        }
    }

    // 每个线程结束前必须释放自己初始化的COM
    CoUninitialize();
}

// 主程序入口
int wmain(int argc, wchar_t* argv[]) {
    // 主线程COM初始化
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        wprintf(L"Failed to initialize COM. HR = 0x%X\n", hr);
        return 1;
    }

    if (argc <= 1) {
        ShowHelp(argv[0]);
        CoUninitialize();
        return 1;
    }

    std::vector<std::wstring> inputPaths;
    std::wstring outputDir;
    float quality = -1.0f; // -1.0 作为哨兵值，表示使用默认质量

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"-h" || arg == L"--help") {
            ShowHelp(argv[0]);
            CoUninitialize();
            return 0;
        }
        if (arg == L"-i" || arg == L"--input") {
            while (i + 1 < argc && argv[i + 1][0] != L'-') {
                inputPaths.push_back(argv[++i]);
            }
        }
        else if (arg == L"-o" || arg == L"--output") {
            if (i + 1 < argc) {
                outputDir = argv[++i];
            }
        }
        else if (arg == L"-q" || arg == L"--quality") {
            if (i + 1 < argc) {
                try {
                    quality = std::stof(argv[++i]) / 100.0f;
                    if (quality < 0.0f || quality > 1.0f) {
                        wprintf(L"Warning: Quality must be between 0 and 100. Using default.\n");
                        quality = -1.0f;
                    }
                }
                catch (const std::exception&) {
                    wprintf(L"Warning: Invalid quality value. It must be a number. Using default.\n");
                    quality = -1.0f;
                }
            }
        }
    }

    if (inputPaths.empty() || outputDir.empty()) {
        wprintf(L"Error: Both input and output paths must be specified.\n\n");
        ShowHelp(argv[0]);
        CoUninitialize();
        return 1;
    }

    if (GetFileAttributesW(outputDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(outputDir.c_str(), NULL)) {
            wprintf(L"Error: Failed to create output directory: %s\n", outputDir.c_str());
            CoUninitialize();
            return 1;
        }
    }

    std::vector<std::wstring> filesToProcess;
    for (const auto& path : inputPaths) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"Warning: Input path not found, skipping: %s\n", path.c_str());
            continue;
        }
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring searchPath = path + L"\\*";
            WIN32_FIND_DATAW findData;
            HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        std::wstring fullPath = path + L"\\" + findData.cFileName;
                        if (IsSupportedImageFile(fullPath)) {
                            filesToProcess.push_back(fullPath);
                        }
                    }
                } while (FindNextFileW(hFind, &findData) != 0);
                FindClose(hFind);
            }
        }
        else {
            if (IsSupportedImageFile(path)) {
                filesToProcess.push_back(path);
            }
            else {
                wprintf(L"Warning: Unsupported file type, skipping: %s\n", path.c_str());
            }
        }
    }

    if (filesToProcess.empty()) {
        wprintf(L"No supported image files found to process.\n");
        CoUninitialize();
        return 0;
    }

    // 并行处理逻辑
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    const unsigned int num_threads = std::max(1u, (unsigned int)sysInfo.dwNumberOfProcessors);
    wprintf(L"\nFound %zu files. Starting conversion on %u threads...\n\n", filesToProcess.size(), num_threads);

    std::atomic<size_t> task_index(0);
    std::atomic<int> success_count(0);
    std::atomic<int> fail_count(0);

    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < num_threads; ++i) {
        threads.emplace_back(
            Worker,
            &filesToProcess,
            &outputDir,
            &task_index,
            &success_count,
            &fail_count,
            quality
        );
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    wprintf(L"\nConversion finished. %d successful, %d failed.\n", success_count.load(), fail_count.load());

    // 主线程释放COM
    CoUninitialize();
    return 0;
}


// --- 辅助函数实现 ---

void ShowHelp(const WCHAR* appName) {
    wprintf(L"HEIC Converter - Converts standard images to HEIC using Windows API.\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  %s -i <inputs...> -o <output_dir> [-q quality]\n\n", PathFindFileNameW(appName));
    wprintf(L"Arguments:\n");
    wprintf(L"  -i, --input   One or more input files or directories.\n");
    wprintf(L"                If a directory is provided, all supported images inside will be processed.\n");
    wprintf(L"  -o, --output  The directory where converted .heic files will be saved.\n");
    wprintf(L"  -q, --quality (Optional) Set the quality of the output image (0-100).\n");
    wprintf(L"                Default is the Windows' built-in setting (usually high quality).\n");
    wprintf(L"  -h, --help    Show this help message.\n\n");
    wprintf(L"Example:\n");
    wprintf(L"  %s -i C:\\photo.jpg -o C:\\Out\n", PathFindFileNameW(appName));
    wprintf(L"  %s -i C:\\pics -o D:\\HEIC_Photos -q 80\n", PathFindFileNameW(appName));
}

bool IsSupportedImageFile(const std::wstring& fileName) {
    const std::vector<std::wstring> supportedExtensions = {
        L".jpg", L".jpeg", L".png", L".bmp", L".tiff", L".gif"
    };
    std::wstring extension = PathFindExtensionW(fileName.c_str());
    CharLowerW(&extension[0]);
    for (const auto& ext : supportedExtensions) {
        if (extension == ext) {
            return true;
        }
    }
    return false;
}

HRESULT ConvertImageToHeic(const WCHAR* inputPath, const WCHAR* outputPath, float quality) {
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
    hr = pFactory->CreateEncoder(GUID_ContainerFormatHeif, NULL, &pEncoder);
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
        if (FAILED(hr)) {
            //
        }
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
