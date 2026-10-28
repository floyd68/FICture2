#include "TestHarness.h"

#include "ImageAsyncBinding.h"
#include "ImageBrowserThumbImage.h"
#include "ImageBrowserThumbModel.h"
#include "ImageBrowserThumbPayloadCache.h"
#include "ImageBrowserThumbSizePolicy.h"
#include "ImageBrowserThumbStripController.h"
#include "ImageBrowserThumbnailPane.h"
#include "ThumbImageTile.h"
#include "ThumbTestSupport.h"
#include "CommonUtil.h"
#include "FD2D/Spinner.h"
#include "ImageCore/ImageCore.h"
#include "ImageCore/ImageDecodeDispatcher.h"

#include <DirectXTex.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Thumbnail size upgrades in strip tiles (#40): the decode-size policy
// (ThumbSizePolicy) applied by real tiles bound through the strip controller,
// with a recording decode service. Requests complete either with ImageCore's
// thumbnail fit of a declared source size, or with a real ImageCore decode of
// image bytes generated in the test (PNG, JPEG, mipmapped DDS).

namespace
{
    const Floar::VirtualPath kFolder(std::filesystem::path(L"C:\\FICture2Tests\\Sizes"));

    bool IsTestImage(const Floar::VirtualPath& path)
    {
        const std::wstring ext = path.GetExtension();
        return ext == L".png" || ext == L".jpg" || ext == L".dds";
    }

    std::wstring StableName(const wchar_t* prefix, const Floar::VirtualPath& path)
    {
        return std::wstring(prefix) + L"|" + ThumbItemModel::LookupKeyFor(path);
    }

    ImageSourceIdentity KnownIdentity(uint64_t size, uint64_t writeTime = 1)
    {
        ImageSourceIdentity identity {};
        identity.size = size;
        identity.writeTime = writeTime;
        identity.known = true;
        return identity;
    }

    // ImageCore's thumbnail fit (ImageDecodeDispatcher): min scale of the box,
    // rounded, never upscaled.
    void Fit(uint32_t sourceWidth, uint32_t sourceHeight, const ImageCore::Size& box, uint32_t& width, uint32_t& height)
    {
        width = sourceWidth;
        height = sourceHeight;
        if (static_cast<float>(sourceWidth) > box.w || static_cast<float>(sourceHeight) > box.h)
        {
            const float scale = (std::min)(box.w / static_cast<float>(sourceWidth), box.h / static_cast<float>(sourceHeight));
            width = static_cast<uint32_t>(std::round(sourceWidth * scale));
            height = static_cast<uint32_t>(std::round(sourceHeight * scale));
        }
    }

    // DirectXTex and WIC (through ImageCore's dispatcher) cache their factory
    // process-wide; keep the MTA alive for the rest of the process.
    void KeepMtaAlive()
    {
        static const HRESULT s_hrMta = []
        {
            CO_MTA_USAGE_COOKIE cookie {};
            return CoIncrementMTAUsage(&cookie);
        }();
        FIC2_REQUIRE(SUCCEEDED(s_hrMta));
    }

    // A `width` x `height` gradient encoded by WIC (PNG or JPEG).
    std::vector<uint8_t> EncodeWic(REFGUID container, uint32_t width, uint32_t height)
    {
        KeepMtaAlive();
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        FIC2_REQUIRE(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))));
        Microsoft::WRL::ComPtr<IStream> stream;
        FIC2_REQUIRE(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)));
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        FIC2_REQUIRE(SUCCEEDED(factory->CreateEncoder(container, nullptr, &encoder)));
        FIC2_REQUIRE(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> props;
        FIC2_REQUIRE(SUCCEEDED(encoder->CreateNewFrame(&frame, &props)));
        FIC2_REQUIRE(SUCCEEDED(frame->Initialize(props.Get())));
        FIC2_REQUIRE(SUCCEEDED(frame->SetSize(width, height)));
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        FIC2_REQUIRE(SUCCEEDED(frame->SetPixelFormat(&format)));
        FIC2_REQUIRE(format == GUID_WICPixelFormat24bppBGR);

        const UINT stride = width * 3u;
        std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t* row = pixels.data() + static_cast<size_t>(y) * stride;
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x * 3 + 0] = static_cast<uint8_t>(x * 255 / width);
                row[x * 3 + 1] = static_cast<uint8_t>(y * 255 / height);
                row[x * 3 + 2] = 0x60;
            }
        }
        FIC2_REQUIRE(SUCCEEDED(frame->WritePixels(height, stride, static_cast<UINT>(pixels.size()), pixels.data())));
        FIC2_REQUIRE(SUCCEEDED(frame->Commit()));
        FIC2_REQUIRE(SUCCEEDED(encoder->Commit()));

        HGLOBAL global = nullptr;
        FIC2_REQUIRE(SUCCEEDED(GetHGlobalFromStream(stream.Get(), &global)));
        STATSTG stat {};
        FIC2_REQUIRE(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)));
        const auto* data = static_cast<const uint8_t*>(GlobalLock(global));
        FIC2_REQUIRE(data != nullptr);
        std::vector<uint8_t> bytes(data, data + stat.cbSize.QuadPart);
        GlobalUnlock(global);
        return bytes;
    }

    // A `width` x `height` BGRA8 DDS with its full mip chain.
    std::vector<uint8_t> MakeMipmappedDds(size_t width, size_t height)
    {
        KeepMtaAlive();
        DirectX::ScratchImage base;
        FIC2_REQUIRE(SUCCEEDED(base.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, 1)));
        const DirectX::Image* image = base.GetImage(0, 0, 0);
        FIC2_REQUIRE(image != nullptr);
        for (size_t y = 0; y < height; ++y)
        {
            uint8_t* row = image->pixels + y * image->rowPitch;
            for (size_t x = 0; x < width; ++x)
            {
                row[x * 4 + 0] = static_cast<uint8_t>(x * 4);
                row[x * 4 + 1] = static_cast<uint8_t>(y * 4);
                row[x * 4 + 2] = 0x80;
                row[x * 4 + 3] = 0xFF;
            }
        }
        DirectX::ScratchImage mips;
        FIC2_REQUIRE(SUCCEEDED(DirectX::GenerateMipMaps(*image,
            DirectX::TEX_FILTER_LINEAR | DirectX::TEX_FILTER_FORCE_NON_WIC, 0, mips)));
        FIC2_REQUIRE(mips.GetMetadata().mipLevels > 1);
        DirectX::Blob blob;
        FIC2_REQUIRE(SUCCEEDED(DirectX::SaveToDDSMemory(mips.GetImages(), mips.GetImageCount(),
            mips.GetMetadata(), DirectX::DDS_FLAGS_NONE, blob)));
        const auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
        return std::vector<uint8_t>(bytes, bytes + blob.GetBufferSize());
    }

    // An image file of the listing: its source size (requests complete with
    // ImageCore's fit of it), or encoded bytes (requests complete with a real
    // ImageCore decode of them).
    struct SourceFile
    {
        std::wstring name {};
        uint32_t width { 0 };
        uint32_t height { 0 };
        std::vector<uint8_t> bytes {};
    };

    // A strip over `files` (images only, model index = file order by name),
    // with the browser's wiring: aspects and source-limited heights go to the
    // model, row height changes go to the bound tiles.
    struct SizeStrip
    {
        ThumbTestSupport::ComScope com {};
        ThumbTestSupport::Canvas canvas {};
        ThumbTestSupport::RecordingDecodeService decodes {};
        std::shared_ptr<ThumbPayloadCache> payloads { std::make_shared<ThumbPayloadCache>() };
        ThumbItemModel model {};
        ThumbTileSet tiles {};
        std::shared_ptr<ThumbStripPanel> panel { std::make_shared<ThumbStripPanel>() };
        ImageBrowserThumbStripController controller {};
        std::vector<SourceFile> files {};
        std::vector<ImageSourceIdentity> identities {};
        float rowHeight { 128.0f };
        bool deferred { false };

        explicit SizeStrip(std::vector<SourceFile> sources)
            : files(std::move(sources))
        {
            FIC2_REQUIRE(canvas.Create(64, 64));
            ImageAsyncBinding::SetDecodeServiceForTesting(&decodes);
            std::sort(files.begin(), files.end(),
                [](const SourceFile& a, const SourceFile& b) { return a.name < b.name; });
            for (size_t i = 0; i < files.size(); ++i)
            {
                identities.push_back(KnownIdentity(1000 + i));
            }
            List();
            panel->SetTiles(&tiles);
        }

        ~SizeStrip()
        {
            tiles.Clear();
            panel->ClearChildren();
            ImageAsyncBinding::SetDecodeServiceForTesting(nullptr);
        }

        // Lists the folder (again), with the current identities.
        void List()
        {
            std::vector<Floar::VirtualFileEntry> listing;
            for (const SourceFile& file : files)
            {
                listing.emplace_back(Floar::VirtualPath(kFolder.hostPath / file.name), false, 0, 0);
            }
            model.Reset(kFolder, false, &IsTestImage);
            (void)model.Merge(listing, identities);
        }

        void Materialize(ThumbIndexRange range)
        {
            ImageBrowserThumbStripController::MaterializeContext context {};
            context.panel = panel;
            context.tiles = &tiles;
            context.thumbW = rowHeight;
            context.thumbH = rowHeight;
            context.model = &model;
            context.range = range;
            context.visible = range;
            context.payloads = payloads;
            context.makeStableName = &StableName;
            context.deferSizeUpgrades = deferred;
            context.onImageAspect = [this](const std::wstring& key, float aspect) { (void)model.SetAspect(key, aspect); };
            context.onImageSourceLimited = [this](const std::wstring& key, uint32_t height)
            {
                (void)model.SetSourceLimited(key, height);
            };
            controller.MaterializeTiles(context);
        }

        // As ImageBrowser::UpdateThumbSizingFromPane: bound tiles get the new row height.
        void SetRowHeight(float height)
        {
            rowHeight = height;
            for (const ThumbTile& tile : tiles.Tiles())
            {
                tile.imageTile->SetFixedHeight(height);
            }
        }

        // As ImageBrowser::UpdateThumbSizeUpgradeDeferral (splitter drag).
        void SetDeferred(bool value)
        {
            deferred = value;
            for (const ThumbTile& tile : tiles.Tiles())
            {
                tile.imageTile->SetSizeUpgradesDeferred(value);
            }
        }

        // One frame: completed decodes are admitted, then every bound tile renders.
        void Frame()
        {
            ImageBrowserThumbStripController::AdmitCompletedPayloads(tiles);
            for (const ThumbTile& tile : tiles.Tiles())
            {
                FIC2_REQUIRE(SUCCEEDED(canvas.Render(*tile.imageTile)));
            }
        }

        // Decodes complete, then a frame shows them (and evaluates upgrades).
        void Settle(int frames = 3)
        {
            for (int i = 0; i < frames; ++i)
            {
                CompleteOutstanding();
                Frame();
            }
        }

        std::wstring PathOf(size_t index) const
        {
            return CommonUtil::NormalizePath(model.At(index).path.wstring());
        }

        const SourceFile& FileOf(size_t index) const
        {
            const std::wstring name = model.At(index).path.filename().wstring();
            for (const SourceFile& file : files)
            {
                if (file.name == name)
                {
                    return file;
                }
            }
            FIC2_REQUIRE(false);
            return files.front();
        }

        size_t RequestsFor(size_t index) const { return decodes.ThumbnailRequestsFor(PathOf(index)); }

        ImageCore::Size LatestBox(size_t index) const
        {
            const ImageCore::ImageHandle handle = decodes.LatestThumbnailFor(PathOf(index));
            FIC2_REQUIRE(handle != 0);
            return decodes.Requests()[handle - 1].request.targetSize;
        }

        D2D1_SIZE_F BitmapOf(size_t index) const
        {
            const ThumbTile* tile = tiles.Find(index);
            FIC2_REQUIRE(tile != nullptr);
            return tile->image->GetBitmapSize();
        }

        bool SpinnerActive(size_t index) const
        {
            const auto& children = tiles.Find(index)->image->Children();
            const auto it = children.find(L"loadingSpinner");
            FIC2_REQUIRE(it != children.end());
            return std::static_pointer_cast<FD2D::Spinner>(it->second)->Active();
        }

        // Decodes `request` of `file` as ImageCore would.
        ImageCore::DecodedImage Decode(const SourceFile& file, const ImageCore::ImageRequest& request) const
        {
            if (file.bytes.empty())
            {
                uint32_t width = 0;
                uint32_t height = 0;
                Fit(file.width, file.height, request.targetSize, width, height);
                return ThumbTestSupport::MakeBgraImage(width, height);
            }
            KeepMtaAlive();
            ImageCore::RegisterBuiltInDecoders();
            ImageCore::DecodeInput input {};
            input.bytes = std::span<const uint8_t>(file.bytes.data(), file.bytes.size());
            input.header = std::span<const uint8_t>(file.bytes.data(), (std::min)(file.bytes.size(), size_t { 512 }));
            ImageCore::ImageDecodeDispatcher dispatcher;
            ImageCore::PipelineResult result = dispatcher.Decode(request, input);
            FIC2_REQUIRE(SUCCEEDED(result.hr));
            return std::move(result.image);
        }

        // Completes every request that is neither canceled nor completed.
        void CompleteOutstanding()
        {
            for (const auto& request : decodes.Requests())
            {
                if (request.canceled || request.completed)
                {
                    continue;
                }
                for (size_t index = 0; index < model.DisplayCount(); ++index)
                {
                    if (PathOf(index) == request.request.source)
                    {
                        decodes.Complete(request.handle, S_OK, Decode(FileOf(index), request.request));
                        break;
                    }
                }
            }
        }

        // Inserts a cached payload of item `index`, decoded for `box`.
        void Cache(size_t index, uint32_t width, uint32_t height, const ImageCore::Size& box)
        {
            ImageAsyncBinding::Payload payload {};
            const ImageCore::DecodedImage image = ThumbTestSupport::MakeBgraImage(width, height);
            payload.blocks = image.blocks;
            payload.width = width;
            payload.height = height;
            payload.rowPitch = width * 4u;
            payload.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            payload.sourcePath = PathOf(index);
            (void)payloads->Insert(PathOf(index), model.At(index).identity, ThumbDecodeOptions {}, box, payload);
        }
    };

    std::vector<SourceFile> Squares(size_t count, uint32_t side = 1000)
    {
        std::vector<SourceFile> files;
        for (size_t i = 0; i < count; ++i)
        {
            files.push_back({ L"sq" + std::to_wstring(i) + L".png", side, side });
        }
        return files;
    }
}

// An item whose only cached size is smaller than the row's bucket shows it,
// but it does not satisfy the bucket: one upgrade is requested, and the cached
// thumbnail stays until the larger one arrives.
FIC2_TEST(ThumbSizeUpgrade_UndersizedCacheHitIsNotAcceptedForALargerBucket)
{
    SizeStrip strip(Squares(1));
    strip.Cache(0, 64, 64, { 64.0f, 64.0f });

    strip.rowHeight = 128.0f;
    strip.Materialize({ 0, 1 });
    FIC2_CHECK(strip.RequestsFor(0) == 0);   // shown from the cache first
    strip.Frame();
    FIC2_CHECK(strip.BitmapOf(0).height == 64.0f);
    FIC2_REQUIRE(strip.RequestsFor(0) == 1);   // ... and upgraded
    FIC2_CHECK(strip.LatestBox(0).h == 128.0f);
    FIC2_CHECK(!strip.SpinnerActive(0));       // the cached thumbnail stays visible
    strip.Frame();
    FIC2_CHECK(strip.RequestsFor(0) == 1);

    strip.Settle();
    FIC2_CHECK(strip.BitmapOf(0).height == 128.0f);
    FIC2_CHECK(strip.RequestsFor(0) == 1);

    // At the cached entry's own bucket it is enough: no request.
    SizeStrip atBucket(Squares(1));
    atBucket.Cache(0, 64, 64, { 64.0f, 64.0f });
    atBucket.rowHeight = 64.0f;
    atBucket.Materialize({ 0, 1 });
    atBucket.Settle();
    FIC2_CHECK(atBucket.BitmapOf(0).height == 64.0f);
    FIC2_CHECK(atBucket.RequestsFor(0) == 0);
}

// A cached size larger than the bucket is reused, and of several sufficient
// sizes the smallest is shown.
FIC2_TEST(ThumbSizeUpgrade_ALargerCachedSizeIsReused)
{
    SizeStrip strip(Squares(1));
    strip.Cache(0, 64, 64, { 64.0f, 64.0f });
    strip.Cache(0, 256, 256, { 256.0f, 256.0f });

    strip.rowHeight = 128.0f;
    strip.Materialize({ 0, 1 });
    strip.Settle();
    FIC2_CHECK(strip.BitmapOf(0).height == 256.0f);
    FIC2_CHECK(strip.RequestsFor(0) == 0);

    // Rebound with a 128 entry too: the smallest sufficient one.
    strip.Cache(0, 128, 128, { 128.0f, 128.0f });
    strip.Materialize({ 0, 0 });
    strip.Materialize({ 0, 1 });
    strip.Settle();
    FIC2_CHECK(strip.BitmapOf(0).height == 128.0f);
    FIC2_CHECK(strip.RequestsFor(0) == 0);

    // A smaller row height keeps the larger thumbnail (no smaller decode).
    strip.SetRowHeight(48.0f);
    strip.Settle();
    FIC2_CHECK(strip.BitmapOf(0).height == 128.0f);
    FIC2_CHECK(strip.RequestsFor(0) == 0);
}

// An already bound tile whose row grows past its payload takes a sufficient
// size from the cache (decoded earlier, e.g. by another tile) instead of
// requesting a decode: shown on the same render, no request.
FIC2_TEST(ThumbSizeUpgrade_RowGrowthOnABoundTileReusesALargerCachedSize)
{
    SizeStrip strip(Squares(1));
    strip.Cache(0, 128, 128, { 128.0f, 128.0f });
    strip.Cache(0, 256, 256, { 256.0f, 256.0f });

    strip.rowHeight = 128.0f;
    strip.Materialize({ 0, 1 });
    strip.Settle();
    FIC2_CHECK(strip.BitmapOf(0).height == 128.0f);   // the smallest sufficient one
    FIC2_CHECK(strip.RequestsFor(0) == 0);

    strip.SetRowHeight(192.0f);
    strip.Frame();   // no decode completes
    FIC2_CHECK(strip.RequestsFor(0) == 0);
    FIC2_CHECK(strip.BitmapOf(0).height == 256.0f);
    strip.Frame();
    FIC2_CHECK(strip.RequestsFor(0) == 0);
    FIC2_CHECK(strip.BitmapOf(0).height == 256.0f);

    // With no sufficient cached size the upgrade is still requested once.
    strip.SetRowHeight(320.0f);
    strip.Frame();
    strip.Frame();
    FIC2_CHECK(strip.RequestsFor(0) == 1);
    FIC2_CHECK(strip.BitmapOf(0).height == 256.0f);   // shown until it arrives
}

// A source smaller than the target, dragged across several buckets (live and
// deferred): exactly one decode. Its source-limited height is recorded on
// the item model until the folder is listed again.
FIC2_TEST(ThumbSizeUpgrade_ASmallSourceDecodesOnceAcrossBuckets)
{
    SizeStrip strip({ { L"atBucket.png", 40, 30 } });
    strip.rowHeight = 48.0f;
    strip.Materialize({ 0, 1 });
    strip.Settle();
    FIC2_REQUIRE(strip.RequestsFor(0) == 1);
    FIC2_CHECK(strip.BitmapOf(0).width == 40.0f && strip.BitmapOf(0).height == 30.0f);
    const std::wstring key = strip.model.At(0).lookupKey;
    FIC2_CHECK(strip.model.SourceLimitedHeightOf(key) == 30);

    for (const float row : { 64.0f, 96.0f, 128.0f, 150.0f, 192.0f, 256.0f, 64.0f, 256.0f })
    {
        strip.SetRowHeight(row);
        strip.Settle();
    }
    strip.SetDeferred(true);
    for (const float row : { 32.0f, 256.0f, 100.0f, 200.0f })
    {
        strip.SetRowHeight(row);
        strip.Settle();
    }
    strip.SetDeferred(false);
    strip.Settle();
    FIC2_CHECK(strip.RequestsFor(0) == 1);

    // Rebound (recycled tile, cached source-limited entry): still nothing.
    strip.Materialize({ 0, 0 });
    strip.Materialize({ 0, 1 });
    strip.SetRowHeight(256.0f);
    strip.Settle();
    FIC2_CHECK(strip.RequestsFor(0) == 1);

    // Listed again: the record is cleared.
    strip.List();
    FIC2_CHECK(strip.model.SourceLimitedHeightOf(key) == 0);
}

// A small mipmapped DDS decodes at its source size through ImageCore (mip 0
// once no mip reaches the target, ImageCore #18), is source-limited, and is
// not requested again at larger buckets. A wide mipmapped DDS gets its
// aspect-shaped upgrade from the mip that reaches it.
FIC2_TEST(ThumbSizeUpgrade_ASmallMipmappedDdsDecodesAtSourceSize)
{
    SizeStrip strip({ { L"atBucket.dds", 48, 48, MakeMipmappedDds(48, 48) } });
    strip.rowHeight = 128.0f;
    strip.Materialize({ 0, 1 });
    strip.Settle();
    FIC2_REQUIRE(strip.RequestsFor(0) == 1);
    FIC2_CHECK(strip.BitmapOf(0).width == 48.0f && strip.BitmapOf(0).height == 48.0f);
    for (const float row : { 192.0f, 256.0f, 129.0f, 256.0f })
    {
        strip.SetRowHeight(row);
        strip.Settle();
    }
    FIC2_CHECK(strip.RequestsFor(0) == 1);
    FIC2_CHECK(strip.BitmapOf(0).width == 48.0f);

    // 192 x 96 with mips, first at row 32: the square box gives 32 x 16 (a
    // mip), the aspect-shaped upgrade 64 x 32. At row 128 no mip reaches the
    // box: mip 0 at source size, source-limited.
    SizeStrip wide({ { L"wide.dds", 192, 96, MakeMipmappedDds(192, 96) } });
    wide.rowHeight = 32.0f;
    wide.Materialize({ 0, 1 });
    wide.Settle();
    FIC2_CHECK(wide.RequestsFor(0) == 2);
    FIC2_CHECK(wide.BitmapOf(0).width == 64.0f && wide.BitmapOf(0).height == 32.0f);
    wide.SetRowHeight(128.0f);
    wide.Settle();
    FIC2_CHECK(wide.RequestsFor(0) == 3);
    FIC2_CHECK(wide.BitmapOf(0).width == 192.0f && wide.BitmapOf(0).height == 96.0f);
    wide.SetRowHeight(256.0f);
    wide.Settle();
    FIC2_CHECK(wide.RequestsFor(0) == 3);
}

// Rapid row-height reversals: during a (deferred) splitter drag nothing is
// upgraded; once it ends, one upgrade serves the final row height. Live
// reversals with an upgrade in flight never stack requests, and a larger
// decode serves every smaller row height afterwards.
FIC2_TEST(ThumbSizeUpgrade_RapidResizeReversals)
{
    {
        SizeStrip strip(Squares(3));
        strip.rowHeight = 64.0f;
        strip.Materialize({ 0, 3 });
        strip.Settle();
        for (size_t i = 0; i < 3; ++i)
        {
            FIC2_REQUIRE(strip.RequestsFor(i) == 1);
        }

        strip.SetDeferred(true);
        for (int cycle = 0; cycle < 10; ++cycle)
        {
            for (const float row : { 256.0f, 64.0f, 200.0f, 48.0f, 129.0f })
            {
                strip.SetRowHeight(row);
                strip.Settle(1);
            }
        }
        strip.SetRowHeight(200.0f);
        strip.Settle();
        for (size_t i = 0; i < 3; ++i)
        {
            FIC2_CHECK(strip.RequestsFor(i) == 1);
            FIC2_CHECK(strip.BitmapOf(i).height == 64.0f);
        }

        strip.SetDeferred(false);   // drag ended
        strip.Settle();
        for (size_t i = 0; i < 3; ++i)
        {
            FIC2_CHECK(strip.RequestsFor(i) == 2);
            FIC2_CHECK(strip.LatestBox(i).h == 256.0f);
            FIC2_CHECK(strip.BitmapOf(i).height == 256.0f);
        }

        for (int cycle = 0; cycle < 20; ++cycle)
        {
            strip.SetRowHeight((cycle % 2) ? 64.0f : 256.0f);
            strip.Settle(1);
        }
        for (size_t i = 0; i < 3; ++i)
        {
            FIC2_CHECK(strip.RequestsFor(i) == 2);
        }
    }

    {
        // Live (not deferred), with the upgrade still in flight.
        SizeStrip strip(Squares(1));
        strip.rowHeight = 64.0f;
        strip.Materialize({ 0, 1 });
        strip.Settle();
        strip.SetRowHeight(256.0f);
        strip.Frame();
        FIC2_REQUIRE(strip.RequestsFor(0) == 2);
        for (int cycle = 0; cycle < 10; ++cycle)
        {
            for (const float row : { 64.0f, 256.0f, 129.0f, 200.0f })
            {
                strip.SetRowHeight(row);
                strip.Frame();
            }
        }
        FIC2_CHECK(strip.RequestsFor(0) == 2);
        FIC2_CHECK(strip.decodes.OutstandingThumbnails() == 1);
        FIC2_CHECK(strip.BitmapOf(0).height == 64.0f);
        strip.Settle();
        FIC2_CHECK(strip.BitmapOf(0).height == 256.0f);
        FIC2_CHECK(strip.RequestsFor(0) == 2);
    }
}

// The shown thumbnail stays (without a spinner over it) until the larger
// decode is admitted and uploaded.
FIC2_TEST(ThumbSizeUpgrade_TheShownThumbnailStaysUntilTheUpgradeArrives)
{
    SizeStrip strip(Squares(1));
    strip.rowHeight = 64.0f;
    strip.Materialize({ 0, 1 });
    FIC2_CHECK(strip.RequestsFor(0) == 1);
    strip.Frame();
    FIC2_CHECK(strip.SpinnerActive(0));   // a first decode: nothing to show yet
    strip.Settle();
    FIC2_REQUIRE(strip.BitmapOf(0).height == 64.0f);

    strip.SetRowHeight(128.0f);
    strip.Frame();
    FIC2_REQUIRE(strip.RequestsFor(0) == 2);
    FIC2_CHECK(strip.BitmapOf(0).height == 64.0f);
    FIC2_CHECK(!strip.SpinnerActive(0));

    strip.CompleteOutstanding();
    ImageBrowserThumbStripController::AdmitCompletedPayloads(strip.tiles);
    FIC2_CHECK(strip.BitmapOf(0).height == 64.0f);   // admitted, not uploaded yet
    strip.Frame();
    FIC2_CHECK(strip.BitmapOf(0).height == 128.0f);
}

// An upgrade in flight when its tile is recycled, or when its file is
// replaced: the late completion is never shown, on the recycled tile or on the
// replaced item, and the current request's result is.
FIC2_TEST(ThumbSizeUpgrade_CompletionAfterRecyclingOrReplacementIsDropped)
{
    {
        SizeStrip strip({ { L"a.png", 1000, 1000 }, { L"b.png", 1000, 1000 }, { L"c.png", 600, 300 } });
        strip.rowHeight = 64.0f;
        strip.Materialize({ 0, 1 });
        strip.Settle();
        strip.SetRowHeight(128.0f);
        strip.Frame();
        FIC2_REQUIRE(strip.RequestsFor(0) == 2);
        const ImageCore::ImageHandle upgrade = strip.decodes.LatestThumbnailFor(strip.PathOf(0));
        const auto tileA = strip.tiles.Find(0)->focus;

        // a leaves the range; its tile is rebound to c.
        strip.Materialize({ 2, 3 });
        FIC2_REQUIRE(strip.tiles.Find(2)->focus == tileA);
        FIC2_CHECK(strip.decodes.Requests()[upgrade - 1].canceled);
        FIC2_CHECK(strip.RequestsFor(2) == 1);

        strip.decodes.Complete(upgrade, S_OK, ThumbTestSupport::MakeBgraImage(128, 128));
        strip.Frame();
        FIC2_CHECK(strip.BitmapOf(2).width == 0.0f);
        const auto cachedA = strip.payloads->Find(strip.PathOf(0), strip.model.At(0).identity, ThumbDecodeOptions {});
        FIC2_CHECK(cachedA.size() == 1 && cachedA[0].height == 64);   // the late 128 was dropped

        strip.Settle();
        FIC2_CHECK(strip.BitmapOf(2).height == 128.0f);   // c's own decode (square box, then its aspect)
        FIC2_CHECK(strip.BitmapOf(2).width == 256.0f);
    }

    {
        SizeStrip strip({ { L"a.png", 1000, 1000 } });
        strip.rowHeight = 64.0f;
        strip.Materialize({ 0, 1 });
        strip.Settle();
        strip.SetRowHeight(128.0f);
        strip.Frame();
        FIC2_REQUIRE(strip.RequestsFor(0) == 2);
        const ImageCore::ImageHandle upgrade = strip.decodes.LatestThumbnailFor(strip.PathOf(0));

        // a.png is rewritten (new identity) and listed again: the upgrade is
        // superseded by a first decode of the new file.
        strip.identities[0] = KnownIdentity(strip.identities[0].size, 2);
        strip.List();
        strip.Materialize({ 0, 1 });
        FIC2_CHECK(strip.decodes.Requests()[upgrade - 1].canceled);
        FIC2_REQUIRE(strip.RequestsFor(0) == 3);
        strip.decodes.Complete(upgrade, S_OK, ThumbTestSupport::MakeBgraImage(128, 128));
        strip.Frame();
        FIC2_CHECK(strip.BitmapOf(0).height == 64.0f);   // the old picture, until the new one
        strip.Settle();
        FIC2_CHECK(strip.BitmapOf(0).height == 128.0f);
        FIC2_CHECK(strip.RequestsFor(0) == 3);
    }
}

// Through real ImageCore decodes: 16:9 and 3:1 large sources, a camera-sized
// JPEG and a portrait source reach the displayed size (row height x row
// height * aspect) after at most one upgrade, with no repeated requests;
// growing the row height costs one more decode each. Rebound later with the
// aspect known to the model, the first request is already aspect-shaped: one
// decode.
FIC2_TEST(ThumbSizeUpgrade_SourcesReachTheDisplayedSizeAfterAtMostOneUpgrade)
{
    SizeStrip strip({
        { L"a_wide.png", 1920, 1080, EncodeWic(GUID_ContainerFormatPng, 1920, 1080) },
        { L"b_pano.png", 3000, 1000, EncodeWic(GUID_ContainerFormatPng, 3000, 1000) },
        { L"c_camera.jpg", 4032, 3024, EncodeWic(GUID_ContainerFormatJpeg, 4032, 3024) },
        { L"d_portrait.png", 1080, 1920, EncodeWic(GUID_ContainerFormatPng, 1080, 1920) },
    });
    const auto reachesDisplayedSize = [&](size_t index, float row)
    {
        const SourceFile& file = strip.FileOf(index);
        const D2D1_SIZE_F bitmap = strip.BitmapOf(index);
        const float displayedWidth = row * static_cast<float>(file.width) / static_cast<float>(file.height);
        std::printf("  %ls at row %.0f: %.0f x %.0f (displayed %.1f x %.0f)\n",
            file.name.c_str(), row, bitmap.width, bitmap.height, displayedWidth, row);
        return bitmap.height + 1.0f >= row && bitmap.width + 1.0f >= displayedWidth;
    };

    strip.rowHeight = 128.0f;
    strip.Materialize({ 0, 4 });
    strip.Settle(5);
    for (size_t i = 0; i < 4; ++i)
    {
        FIC2_CHECK(reachesDisplayedSize(i, 128.0f));
        FIC2_CHECK(strip.RequestsFor(i) <= 2);
    }
    FIC2_CHECK(strip.RequestsFor(3) == 1);   // portrait: the square box is already height-bound

    strip.SetRowHeight(200.0f);
    strip.Settle(5);
    for (size_t i = 0; i < 4; ++i)
    {
        FIC2_CHECK(reachesDisplayedSize(i, 200.0f));
        FIC2_CHECK(strip.LatestBox(i).h == 256.0f);
    }
    const size_t total = strip.decodes.RequestCount();
    FIC2_CHECK(total <= 4 + 3 + 4);
    strip.Settle(5);
    FIC2_CHECK(strip.decodes.RequestCount() == total);

    // Away, cache dropped, back at another bucket: the model's aspect shapes
    // the first request, so each item is decoded once.
    strip.Materialize({ 0, 0 });
    strip.payloads = std::make_shared<ThumbPayloadCache>();
    strip.rowHeight = 96.0f;
    strip.Materialize({ 0, 4 });
    strip.Settle(5);
    FIC2_CHECK(strip.decodes.RequestCount() == total + 4);
    for (size_t i = 0; i < 4; ++i)
    {
        FIC2_CHECK(reachesDisplayedSize(i, 96.0f));
    }
}

// Tiles bound while the splitter is dragged (the materialize context says so)
// show what is cached and defer the upgrade until the drag ends.
FIC2_TEST(ThumbSizeUpgrade_TilesBoundDuringADragDeferUpgrades)
{
    SizeStrip strip(Squares(2));
    strip.Cache(0, 64, 64, { 64.0f, 64.0f });
    strip.rowHeight = 192.0f;
    strip.deferred = true;
    strip.Materialize({ 0, 2 });
    strip.Settle();
    FIC2_CHECK(strip.RequestsFor(0) == 0);                // cached, undersized: deferred
    FIC2_CHECK(strip.BitmapOf(0).height == 64.0f);
    FIC2_CHECK(strip.RequestsFor(1) == 1);                // nothing cached: decoded at once
    FIC2_CHECK(strip.BitmapOf(1).height == 192.0f);

    strip.SetDeferred(false);
    strip.Settle();
    FIC2_CHECK(strip.RequestsFor(0) == 1);
    FIC2_CHECK(strip.BitmapOf(0).height == 192.0f);
    FIC2_CHECK(strip.RequestsFor(1) == 1);
}
