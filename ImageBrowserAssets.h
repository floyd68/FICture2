#pragma once

#include <d2d1.h>
#include <wrl/client.h>

class ImageBrowserAssets
{
public:
    enum class FolderIconKind
    {
        Folder,
        FolderUp,
    };

    // Decodes an embedded Lucide folder PNG (IDR_PNG_FOLDER / IDR_PNG_FOLDER_UP) via WIC
    // and creates a D2D bitmap on the given render target. When tintColor is non-null,
    // the icon pixels are mixed toward that color (used for archive tile tinting).
    static bool CreateFolderIconBitmap(
        ID2D1RenderTarget* target,
        FolderIconKind kind,
        const D2D1_COLOR_F* tintColor,
        Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap);

    bool EnsureFolderBitmap(ID2D1RenderTarget* target, FolderIconKind kind = FolderIconKind::Folder);
    ID2D1Bitmap* FolderBitmap() const { return m_folderBitmap.Get(); }
    ID2D1Bitmap* FolderUpBitmap() const { return m_folderUpBitmap.Get(); }

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_folderBitmap {};
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_folderUpBitmap {};
    ID2D1RenderTarget* m_folderBitmapTarget { nullptr };
    ID2D1RenderTarget* m_folderUpBitmapTarget { nullptr };
};
