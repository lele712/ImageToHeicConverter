#include <iostream>
#include <windows.h>
#include <wincodec.h>       // WIC
#include <wrl/client.h>     // ComPtr
#include <vector>           // 用于存储文件列表
#include <string>
#include <stdexcept>        // 用于 stof 异常处理
#include <shlwapi.h>        // 用于 PathFindFileNameW
#include <pathcch.h>        // 用于 PathCchRenameExtension

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "pathcch.lib")

using Microsoft::WRL::ComPtr;

// 1. 修改 ConvertImageToHeic 函数，增加 quality 参数
HRESULT ConvertImageToHeic(const WCHAR* inputPath, const WCHAR* outputPath, float quality);

// 2. 更新帮助信息
void ShowHelp(const WCHAR* appName) {
    wprintf(L"HEIC Converter - Converts standard images to HEIC using Windows API.\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  %s -i <inputs...> -o <output_dir> [-q quality]\n\n", PathFindFileNameW(appName));
    wprintf(L"Arguments:\n");
    wprintf(L"  -i, --input   One or more input files or directories.\n");
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

int wmain(int argc, wchar_t* argv[]) {
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
    // 3. 增加质量变量，并设一个特殊值表示“未设置”
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
                    // 将字符串转为浮点数，并从 0-100 范围转换到 0.0-1.0 范围
                    quality = std::stof(argv[++i]) / 100.0f;
                    if (quality < 0.0f || quality > 1.0f) {
                        wprintf(L"Warning: Quality must be between 0 and 100. Using default.\n");
                        quality = -1.0f; // 值无效，重置为默认
                    }
                }
                catch (const std::invalid_argument&) {
                    wprintf(L"Warning: Invalid quality value. It must be a number. Using default.\n");
                    quality = -1.0f;
                }
                catch (const std::out_of_range&) {
                    wprintf(L"Warning: Quality value is out of range. Using default.\n");
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
        }
    }

    // 执行转换
    int successCount = 0;
    int failCount = 0;
    if (!filesToProcess.empty()) {
        wprintf(L"\nStarting conversion of %zu files...\n", filesToProcess.size());
    }

    for (const auto& inputFile : filesToProcess) {
        const WCHAR* fileName = PathFindFileNameW(inputFile.c_str());
        WCHAR outPath[MAX_PATH];
        PathCchCombine(outPath, MAX_PATH, outputDir.c_str(), fileName);
        PathCchRenameExtension(outPath, MAX_PATH, L".heic");

        wprintf(L"Converting %s -> %s ... ", PathFindFileNameW(inputFile.c_str()), PathFindFileNameW(outPath));
        // 4. 将解析出的 quality 值传递给转换函数
        hr = ConvertImageToHeic(inputFile.c_str(), outPath, quality);
        if (SUCCEEDED(hr)) {
            wprintf(L"OK\n");
            successCount++;
        }
        else {
            wprintf(L"FAILED (HR=0x%X)\n", hr);
            failCount++;
        }
    }

    wprintf(L"\nConversion finished. %d successful, %d failed.\n", successCount, failCount);

    CoUninitialize();
    return 0;
}


// 5. 修改核心转换函数以处理质量参数
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

    // 检查 quality 是否是有效值（即不是哨兵值 -1.0f）
    if (quality >= 0.0f && quality <= 1.0f) {
        PROPBAG2 option = { 0 };
        wchar_t propName[] = L"ImageQuality"; // 创建一个可修改的 wchar_t 数组副本
        option.pstrName = propName;           // 将副本的指针赋值给 pstrName
        VARIANT varValue;
        VariantInit(&varValue);
        varValue.vt = VT_R4; // VT_R4 表示一个 4 字节的浮点数
        varValue.fltVal = quality;
        hr = pPropertyBag->Write(1, &option, &varValue);
        if (FAILED(hr)) {
            // 即使设置失败，也只是警告，不中断整个过程
            wprintf(L" (Warning: failed to set quality) ");
        }
    }
    // 如果 quality 是 -1.0f，则此代码块被跳过，不设置任何质量属性，编码器将使用默认值。

    hr = pFrameEncode->Initialize(pPropertyBag.Get());
    if (FAILED(hr)) return hr;

    hr = pFrameEncode->WriteSource(pFrameDecode.Get(), NULL);
    if (FAILED(hr)) return hr;

    hr = pFrameEncode->Commit();
    if (FAILED(hr)) return hr;

    hr = pEncoder->Commit();
    return hr;
}