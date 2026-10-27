#pragma once

// Helpers shared by the tests that drive a real ImageBrowserMainImage with a
// controlled decode service and render it on a windowless backplate:
//  - a decode service that records requests and lets the test complete them;
//  - a scope that installs it (and clears the process GPU cache around it);
//  - solid BC1 / BC7 blocks whose decoded values are exact by construction.

#include "ImageAsyncBinding.h"
#include "ImageGpuResourceCache.h"
#include "FD2D/tests/WindowlessTestSupport.h"

#include <array>
#include <cstdint>
#include <vector>

namespace MainImageTestSupport
{
    using FD2DWindowlessTest::Bgra;

    class ControlledDecodeService : public IImageDecodeService
    {
    public:
        struct Request
        {
            ImageCore::ImageRequest request {};
            ImageCore::DecodedImageLoadCallback callback {};
        };

        ImageCore::ImageHandle RequestDecoded(const ImageCore::ImageRequest& request,
            ImageCore::DecodedImageLoadCallback callback, ImageCore::RequestPriority) override
        {
            requests.push_back({ request, std::move(callback) });
            return static_cast<ImageCore::ImageHandle>(requests.size());
        }
        void Cancel(ImageCore::ImageHandle) override {}
        void SetPriority(ImageCore::ImageHandle, ImageCore::RequestPriority) override {}

        std::vector<Request> requests;
    };

    struct ScopedDecodeService
    {
        explicit ScopedDecodeService(IImageDecodeService* service)
        {
            ImageAsyncBinding::SetDecodeServiceForTesting(service);
            ImageGpuResourceCache::Instance().Clear();
        }
        ~ScopedDecodeService()
        {
            ImageGpuResourceCache::Instance().Clear();
            ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
        }
        ScopedDecodeService(const ScopedDecodeService&) = delete;
        ScopedDecodeService& operator=(const ScopedDecodeService&) = delete;
    };

    // --- Solid test blocks -------------------------------------------------

    // BC1: both endpoints the 5:6:5 color, every index 0.
    inline std::vector<uint8_t> SolidBc1(uint16_t rgb565)
    {
        std::vector<uint8_t> block(8, uint8_t(0));
        block[0] = static_cast<uint8_t>(rgb565 & 0xFF);
        block[1] = static_cast<uint8_t>(rgb565 >> 8);
        block[2] = block[0];
        block[3] = block[1];
        return block;
    }

    // The decoded value of SolidBc1(rgb565) (bit replication to 8 bits).
    inline Bgra Bc1Color(uint16_t rgb565)
    {
        const int r5 = (rgb565 >> 11) & 0x1F;
        const int g6 = (rgb565 >> 5) & 0x3F;
        const int b5 = rgb565 & 0x1F;
        return Bgra { static_cast<uint8_t>((b5 << 3) | (b5 >> 2)), static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
            static_cast<uint8_t>((r5 << 3) | (r5 >> 2)), 255 };
    }

    // BC7 mode 6 (one subset, 7-bit RGBA endpoints plus a p-bit each, 4-bit
    // indices) with both p-bits 1 and every index 7 (weight 30/64): each
    // channel value v in [2, 254] comes from endpoints v-1 | 1 and v+1 | 1
    // (odd, so the p-bit is 1) and decodes to exactly v when v is even
    // ((34*(v-1) + 30*(v+1) + 32) >> 6 == v). 255 is both endpoints 255.
    inline std::vector<uint8_t> SolidBc7(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
    {
        std::array<uint8_t, 16> bytes {};
        unsigned bit = 0;
        const auto put = [&](uint32_t value, unsigned count)
        {
            for (unsigned i = 0; i < count; ++i, ++bit)
            {
                if ((value >> i) & 1u)
                {
                    bytes[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
                }
            }
        };
        const auto endpoints = [&](uint8_t v)
        {
            if (v == 255)
            {
                put(0x7F, 7);
                put(0x7F, 7);
            }
            else
            {
                put(static_cast<uint32_t>(v - 1) >> 1, 7);   // endpoint 0: (v-1) = c<<1 | 1
                put(static_cast<uint32_t>(v + 1) >> 1, 7);   // endpoint 1: (v+1) = c<<1 | 1
            }
        };
        put(1u << 6, 7);   // mode 6
        endpoints(r);
        endpoints(g);
        endpoints(b);
        endpoints(a);
        put(1, 1);      // p-bit 0
        put(1, 1);      // p-bit 1
        put(7, 3);      // anchor index (implicit high bit 0)
        for (int i = 1; i < 16; ++i)
        {
            put(7, 4);
        }
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    }
}
