#include "ImageBrowserAssets.h"

#include "framework.h"
#include "Resource.h"

#include <wincodec.h>
#include <vector>

bool ImageBrowserAssets::CreateFolderIconBitmap(
    ID2D1RenderTarget* target,
    const D2D1_COLOR_F* tintColor,
    Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap)
{
    if (target == nullptr)
    {
        return false;
    }

    // Load PNG bytes from RCDATA resource.
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC hrsrc = FindResourceW(module, MAKEINTRESOURCEW(IDR_PNG_FOLDER), RT_RCDATA);
    if (!hrsrc)
    {
        return false;
    }

    HGLOBAL hglob = LoadResource(module, hrsrc);
    if (!hglob)
    {
        return false;
    }

    void* data = LockResource(hglob);
    DWORD size = SizeofResource(module, hrsrc);
    if (!data || size == 0)
    {
        return false;
    }

    // Decode via WIC from memory.
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic));
    if (FAILED(hr) || !wic)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    hr = wic->CreateStream(&stream);
    if (FAILED(hr) || !stream)
    {
        return false;
    }

    hr = stream->InitializeFromMemory(reinterpret_cast<BYTE*>(data), size);
    if (FAILED(hr))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter)
    {
        return false;
    }

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (tintColor == nullptr)
    {
        hr = target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap);
        if (FAILED(hr) || !bitmap)
        {
            return false;
        }
    }
    else
    {
        UINT width = 0;
        UINT height = 0;
        hr = converter->GetSize(&width, &height);
        if (FAILED(hr) || width == 0 || height == 0)
        {
            return false;
        }

        const UINT stride = width * 4;
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * static_cast<size_t>(height));
        hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(hr))
        {
            return false;
        }

        // Mix icon pixels toward the tint color (premultiplied BGRA).
        const float mixFactor = 0.50f;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
        {
            const float alpha = static_cast<float>(pixels[i + 3]) / 255.0f;
            if (alpha <= 0.0f)
            {
                continue;
            }

            const float targetB = tintColor->b * alpha * 255.0f;
            const float targetG = tintColor->g * alpha * 255.0f;
            const float targetR = tintColor->r * alpha * 255.0f;

            pixels[i + 0] = static_cast<BYTE>((1.0f - mixFactor) * pixels[i + 0] + mixFactor * targetB);
            pixels[i + 1] = static_cast<BYTE>((1.0f - mixFactor) * pixels[i + 1] + mixFactor * targetG);
            pixels[i + 2] = static_cast<BYTE>((1.0f - mixFactor) * pixels[i + 2] + mixFactor * targetR);
        }

        Microsoft::WRL::ComPtr<IWICBitmap> tintedBitmap;
        hr = wic->CreateBitmapFromMemory(
            width,
            height,
            GUID_WICPixelFormat32bppPBGRA,
            stride,
            static_cast<UINT>(pixels.size()),
            pixels.data(),
            &tintedBitmap);
        if (FAILED(hr) || !tintedBitmap)
        {
            return false;
        }

        hr = target->CreateBitmapFromWicBitmap(tintedBitmap.Get(), nullptr, &bitmap);
        if (FAILED(hr) || !bitmap)
        {
            return false;
        }
    }

    outBitmap = bitmap;
    return true;
}

bool ImageBrowserAssets::EnsureFolderBitmap(ID2D1RenderTarget* target)
{
    if (target == nullptr)
    {
        return false;
    }

    if (m_folderBitmap && m_folderBitmapTarget == target)
    {
        return true;
    }

    m_folderBitmap.Reset();
    m_folderBitmapTarget = nullptr;

    if (!CreateFolderIconBitmap(target, nullptr, m_folderBitmap))
    {
        return false;
    }

    m_folderBitmapTarget = target;
    return true;
}
