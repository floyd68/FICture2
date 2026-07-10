#pragma once

#include <d2d1.h>
#include <wrl/client.h>

class ImageBrowserAssets
{
public:
    // Decodes the embedded folder PNG (IDR_PNG_FOLDER) via WIC and creates a D2D bitmap
    // on the given render target. When tintColor is non-null, the icon pixels are mixed
    // toward that color (used for archive tile tinting).
    static bool CreateFolderIconBitmap(
        ID2D1RenderTarget* target,
        const D2D1_COLOR_F* tintColor,
        Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap);

    bool EnsureFolderBitmap(ID2D1RenderTarget* target);
    ID2D1Bitmap* FolderBitmap() const { return m_folderBitmap.Get(); }

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_folderBitmap {};
    ID2D1RenderTarget* m_folderBitmapTarget { nullptr };
};
