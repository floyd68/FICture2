#include "ScreenshotUtil.h"
#include "AppLog.h"

#include "FD2D/Backplate.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace ScreenshotUtil
{
    bool ShowSavePngDialog(
        HWND owner,
        const std::wstring& initialFolder,
        const std::wstring& initialFileName,
        std::wstring& outPath)
    {
        using Microsoft::WRL::ComPtr;

        ComPtr<IFileSaveDialog> dialog;
        HRESULT hr = CoCreateInstance(
            CLSID_FileSaveDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        if (FAILED(hr) || !dialog)
        {
            return false;
        }

        static constexpr COMDLG_FILTERSPEC kFilters[] =
        {
            { L"PNG image (*.png)", L"*.png" },
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(kFilters)), kFilters);
        dialog->SetFileTypeIndex(1);
        dialog->SetDefaultExtension(L"png");
        dialog->SetTitle(L"Save Screenshot");
        if (!initialFileName.empty())
        {
            dialog->SetFileName(initialFileName.c_str());
        }
        if (!initialFolder.empty())
        {
            ComPtr<IShellItem> folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    initialFolder.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
                folder)
            {
                dialog->SetFolder(folder.Get());
            }
        }

        DWORD flags = 0;
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);

        hr = dialog->Show(owner);
        if (FAILED(hr))
        {
            return false;
        }

        ComPtr<IShellItem> item;
        hr = dialog->GetResult(&item);
        if (FAILED(hr) || !item)
        {
            return false;
        }

        PWSTR path = nullptr;
        hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (FAILED(hr) || !path)
        {
            return false;
        }

        outPath = path;
        CoTaskMemFree(path);
        return true;
    }

    bool SaveRenderSurfaceRectPng(
        FD2D::Backplate& backplate,
        const D2D1_RECT_F& logicalRect,
        const std::wstring& pngPath)
    {
        if (pngPath.empty())
        {
            return false;
        }

        // Refresh the application-owned offscreen frame, then read it back.
        // Unlike desktop/window DC capture this cannot include pixels from
        // occluding apps.
        backplate.Render();

        std::vector<std::uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
        UINT stride = 0;
        if (FAILED(backplate.ReadComposedPixels(
                logicalRect,
                pixels,
                width,
                height,
                stride)) ||
            pixels.empty() ||
            width == 0 ||
            height == 0)
        {
            return false;
        }

        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        HRESULT result = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(result) || !factory)
        {
            return false;
        }

        ComPtr<IWICStream> stream;
        result = factory->CreateStream(&stream);
        if (FAILED(result) || !stream)
        {
            return false;
        }
        result = stream->InitializeFromFilename(pngPath.c_str(), GENERIC_WRITE);
        if (FAILED(result))
        {
            return false;
        }

        ComPtr<IWICBitmapEncoder> encoder;
        result = factory->CreateEncoder(
            GUID_ContainerFormatPng,
            nullptr,
            &encoder);
        if (FAILED(result) || !encoder)
        {
            return false;
        }
        result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(result))
        {
            return false;
        }

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        result = encoder->CreateNewFrame(&frame, &properties);
        if (FAILED(result) || !frame)
        {
            return false;
        }
        result = frame->Initialize(properties.Get());
        if (FAILED(result))
        {
            return false;
        }
        result = frame->SetSize(width, height);
        if (FAILED(result))
        {
            return false;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        result = frame->SetPixelFormat(&format);
        if (FAILED(result) ||
            !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
        {
            return false;
        }

        result = frame->WritePixels(
            height,
            stride,
            static_cast<UINT>(pixels.size()),
            pixels.data());
        if (FAILED(result))
        {
            return false;
        }
        result = frame->Commit();
        if (FAILED(result))
        {
            return false;
        }
        result = encoder->Commit();
        if (FAILED(result))
        {
            FIC2_LOG_ERROR(
                "[Screenshot] PNG encode commit failed (hr=0x{:08X})",
                static_cast<unsigned>(result));
            return false;
        }

        return true;
    }
}
