#pragma once

// Test doubles for thumbnail-strip tests: a decode service that records every
// request and completes, cancels or keeps it on demand, and a software D2D
// canvas that renders a window so staged payloads are presented.

#include "ImageAsyncBinding.h"
#include "ImageCore/DecodedImage.h"
#include "ImageCore/ImageRequest.h"
#include "FD2D/Wnd.h"

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ThumbTestSupport
{
    inline ImageCore::DecodedImage MakeBgraImage(uint32_t width, uint32_t height)
    {
        ImageCore::DecodedImage image {};
        image.width = width;
        image.height = height;
        image.rowPitchBytes = width * 4u;
        image.dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        image.blocks = std::make_shared<std::vector<uint8_t>>(
            static_cast<size_t>(width) * height * 4u, static_cast<uint8_t>(0x80));
        image.blockBytes = static_cast<uint32_t>(image.blocks->size());
        image.sourceMipLevels = 1;
        return image;
    }

    // Records decode requests and cancels. Nothing completes on its own: a
    // request stays outstanding until it is canceled or Complete()d.
    class RecordingDecodeService : public IImageDecodeService
    {
    public:
        struct Request
        {
            ImageCore::ImageHandle handle { 0 };
            ImageCore::ImageRequest request {};
            ImageCore::RequestPriority priority { ImageCore::RequestPriority::Visible };
            ImageCore::DecodedImageLoadCallback callback {};
            bool canceled { false };
            bool completed { false };
        };

        ImageCore::ImageHandle RequestDecoded(
            const ImageCore::ImageRequest& request,
            ImageCore::DecodedImageLoadCallback callback,
            ImageCore::RequestPriority priority) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const ImageCore::ImageHandle handle = static_cast<ImageCore::ImageHandle>(m_requests.size() + 1);
            m_requests.push_back({ handle, request, priority, std::move(callback), false, false });
            return handle;
        }

        void Cancel(ImageCore::ImageHandle handle) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (handle != 0 && handle <= m_requests.size())
            {
                m_requests[handle - 1].canceled = true;
            }
        }

        void SetPriority(ImageCore::ImageHandle handle, ImageCore::RequestPriority priority) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (handle != 0 && handle <= m_requests.size())
            {
                m_requests[handle - 1].priority = priority;
            }
        }

        // Delivers request `handle`'s completion (as a worker would), even
        // after it was canceled: ImageCore may finish a running request late.
        void Complete(ImageCore::ImageHandle handle, HRESULT hr, ImageCore::DecodedImage image = {})
        {
            ImageCore::DecodedImageLoadCallback callback;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                Request& request = m_requests.at(handle - 1);
                request.completed = true;
                callback = request.callback;
            }
            callback(hr, std::move(image));
        }

        std::vector<Request> Requests() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_requests;
        }

        size_t RequestCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_requests.size();
        }

        // Thumbnail requests that are neither canceled nor completed.
        size_t OutstandingThumbnails() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size_t count = 0;
            for (const Request& request : m_requests)
            {
                count += (IsThumbnail(request) && !request.canceled && !request.completed) ? 1 : 0;
            }
            return count;
        }

        // Thumbnail requests made for `source` (a normalized path).
        size_t ThumbnailRequestsFor(const std::wstring& source) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size_t count = 0;
            for (const Request& request : m_requests)
            {
                count += (IsThumbnail(request) && request.request.source == source) ? 1 : 0;
            }
            return count;
        }

        // The latest thumbnail request for `source`, or 0.
        ImageCore::ImageHandle LatestThumbnailFor(const std::wstring& source) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_requests.rbegin(); it != m_requests.rend(); ++it)
            {
                if (IsThumbnail(*it) && it->request.source == source)
                {
                    return it->handle;
                }
            }
            return 0;
        }

    private:
        static bool IsThumbnail(const Request& request)
        {
            return request.request.purpose == ImageCore::ImagePurpose::Thumbnail;
        }

        mutable std::mutex m_mutex;
        std::vector<Request> m_requests;
    };

    struct ScopedDecodeService
    {
        RecordingDecodeService service;

        ScopedDecodeService() { ImageAsyncBinding::SetDecodeServiceForTesting(&service); }
        ~ScopedDecodeService() { ImageAsyncBinding::SetDecodeServiceForTesting(nullptr); }
    };

    struct ComScope
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope()
        {
            if (SUCCEEDED(hr))
            {
                CoUninitialize();
            }
        }
    };

    // A software render target over a WIC bitmap (no window, no device).
    struct Canvas
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
        Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        Microsoft::WRL::ComPtr<ID2D1RenderTarget> target;

        // `width` x `height` pixels; `dpi` above 96 renders at a display scale
        // (dpi / 96 pixels per DIP).
        bool Create(UINT width, UINT height, float dpi = 96.0f)
        {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
                FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
                FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
            {
                return false;
            }
            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
            return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target));
        }

        HRESULT Render(FD2D::Wnd& wnd)
        {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0, 0, 1));
            wnd.OnRender(target.Get());
            return target->EndDraw();
        }
    };
}
