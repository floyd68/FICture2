#pragma once

#include <d2d1.h>
#include <wrl/client.h>

class ImageBrowserAssets
{
public:
    bool EnsureFolderBitmap(ID2D1RenderTarget* target);
    ID2D1Bitmap* FolderBitmap() const { return m_folderBitmap.Get(); }

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_folderBitmap {};
    ID2D1RenderTarget* m_folderBitmapTarget { nullptr };
};
