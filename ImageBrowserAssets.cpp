#include "ImageBrowserAssets.h"

#include "framework.h"
#include "Resource.h"

#include <wincodec.h>

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

    hr = target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &m_folderBitmap);
    if (FAILED(hr) || !m_folderBitmap)
    {
        return false;
    }

    m_folderBitmapTarget = target;
    return true;
}
