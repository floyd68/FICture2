#include "ScreenshotUtil.h"
#include "AppLog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
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

    bool SaveClientRectPng(HWND hwnd, const D2D1_RECT_F& clientRect, const std::wstring& pngPath)
    {
        if (hwnd == nullptr || !IsWindow(hwnd) || pngPath.empty())
        {
            return false;
        }

        const int left = static_cast<int>(std::floor(clientRect.left));
        const int top = static_cast<int>(std::floor(clientRect.top));
        const int right = static_cast<int>(std::ceil(clientRect.right));
        const int bottom = static_cast<int>(std::ceil(clientRect.bottom));
        const int width = (std::max)(1, right - left);
        const int height = (std::max)(1, bottom - top);

        // Prefer screen capture: flip-model DXGI swap chains often yield a black
        // window DC via GetDC(hwnd)/BitBlt. Desktop BitBlt sees the composed pixels.
        POINT screenOrigin { left, top };
        if (!ClientToScreen(hwnd, &screenOrigin))
        {
            return false;
        }

        HDC screenDc = GetDC(nullptr);
        if (screenDc == nullptr)
        {
            return false;
        }

        HDC memDc = CreateCompatibleDC(screenDc);
        if (memDc == nullptr)
        {
            ReleaseDC(nullptr, screenDc);
            return false;
        }

        BITMAPINFO bmi {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib == nullptr || bits == nullptr)
        {
            DeleteDC(memDc);
            ReleaseDC(nullptr, screenDc);
            return false;
        }

        HGDIOBJ old = SelectObject(memDc, dib);
        constexpr DWORD kCaptureFlags = SRCCOPY | CAPTUREBLT;
        BOOL bltOk = BitBlt(
            memDc,
            0,
            0,
            width,
            height,
            screenDc,
            screenOrigin.x,
            screenOrigin.y,
            kCaptureFlags);

        // Fallback: PrintWindow with PW_RENDERFULLCONTENT (Win8.1+), then crop.
        if (!bltOk)
        {
            constexpr UINT kPwRenderFullContent = 0x00000002;
            RECT client {};
            GetClientRect(hwnd, &client);
            const int fullW = (std::max)(1, static_cast<int>(client.right - client.left));
            const int fullH = (std::max)(1, static_cast<int>(client.bottom - client.top));

            BITMAPINFO fullBmi = bmi;
            fullBmi.bmiHeader.biWidth = fullW;
            fullBmi.bmiHeader.biHeight = -fullH;
            void* fullBits = nullptr;
            HBITMAP fullDib = CreateDIBSection(screenDc, &fullBmi, DIB_RGB_COLORS, &fullBits, nullptr, 0);
            if (fullDib != nullptr && fullBits != nullptr)
            {
                HDC fullDc = CreateCompatibleDC(screenDc);
                if (fullDc != nullptr)
                {
                    HGDIOBJ fullOld = SelectObject(fullDc, fullDib);
                    if (PrintWindow(hwnd, fullDc, kPwRenderFullContent))
                    {
                        bltOk = BitBlt(memDc, 0, 0, width, height, fullDc, left, top, SRCCOPY);
                    }
                    SelectObject(fullDc, fullOld);
                    DeleteDC(fullDc);
                }
                DeleteObject(fullDib);
            }
        }

        SelectObject(memDc, old);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);

        if (!bltOk)
        {
            DeleteObject(dib);
            return false;
        }

        Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic));
        if (FAILED(hr) || !wic)
        {
            DeleteObject(dib);
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap;
        hr = wic->CreateBitmapFromHBITMAP(dib, nullptr, WICBitmapIgnoreAlpha, &wicBitmap);
        DeleteObject(dib);
        if (FAILED(hr) || !wicBitmap)
        {
            return false;
        }

        Microsoft::WRL::ComPtr<IWICStream> stream;
        hr = wic->CreateStream(&stream);
        if (FAILED(hr) || !stream)
        {
            return false;
        }
        hr = stream->InitializeFromFilename(pngPath.c_str(), GENERIC_WRITE);
        if (FAILED(hr))
        {
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr) || !encoder)
        {
            return false;
        }
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(hr))
        {
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> props;
        hr = encoder->CreateNewFrame(&frame, &props);
        if (FAILED(hr) || !frame)
        {
            return false;
        }
        hr = frame->Initialize(props.Get());
        if (FAILED(hr))
        {
            return false;
        }
        hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
        if (FAILED(hr))
        {
            return false;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
        if (FAILED(hr))
        {
            return false;
        }

        hr = frame->WriteSource(wicBitmap.Get(), nullptr);
        if (FAILED(hr))
        {
            return false;
        }
        hr = frame->Commit();
        if (FAILED(hr))
        {
            return false;
        }
        hr = encoder->Commit();
        if (FAILED(hr))
        {
            FIC2_LOG_ERROR("[Screenshot] PNG encode commit failed (hr=0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }

        return true;
    }
}
