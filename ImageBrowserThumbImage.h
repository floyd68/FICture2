#pragma once

#include "FD2D/Wnd.h"
#include "FD2D/Image.h"
#include "FD2D/Spinner.h"
#include "ImageAsyncBinding.h"
#include "ImageCore/ImageRequest.h"
#include "SelectionStyle.h"

#include <functional>
#include <memory>
#include <string>
#include <wrl/client.h>

// FICture2 thumbnail image control: owns TextureImage + Spinner + ImageAsyncBinding.
// Ports FD2D::ThumbImage behavior with path-gated pending apply.
class ImageBrowserThumbImage : public FD2D::Wnd
{
public:
    using ClickHandler = std::function<void()>;

    ImageBrowserThumbImage();
    explicit ImageBrowserThumbImage(const std::wstring& name);
    ~ImageBrowserThumbImage() override;

    FD2D::Size Measure(FD2D::Size available) override;
    void Arrange(FD2D::Rect finalRect) override;

    void SetThumbnailSize(const FD2D::Size& size);
    HRESULT SetSourceFile(const std::wstring& filePath);

    void SetSelected(bool selected);
    bool Selected() const { return m_selected; }
    void SetSelectionStyle(const FD2D::SelectionStyle& style);

    void SetOnClick(ClickHandler handler);
    void SetLoadingSpinnerEnabled(bool enabled);

    D2D1_SIZE_F GetBitmapSize() const;

    void OnAttached(FD2D::Backplate& backplate) override;
    void OnGraphicsInvalidated(
        FD2D::GraphicsInvalidationReason reason,
        const FD2D::GraphicsGeneration& generation) override;
    void OnRender(ID2D1RenderTarget* target) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    void InitChildren();
    void RequestImageLoad();
    void SyncTextureDrawState();
    void ApplyPendingPayload(ID2D1RenderTarget* target);
    void TryReuploadCpuPayload(ID2D1RenderTarget* target);
    void DrawSelectionOverlay(ID2D1RenderTarget* target);

    std::shared_ptr<FD2D::Image> m_texture {};
    std::shared_ptr<FD2D::Spinner> m_spinner {};
    ImageAsyncBinding m_binding {};
    ImageCore::ImageRequest m_request {};

    std::wstring m_filePath {};
    std::wstring m_loadedFilePath {};

    ImageAsyncBinding::Payload m_cpuPayload {};
    bool m_needsCpuReupload { false };

    bool m_selected { false };
    FD2D::SelectionStyle m_selectionStyle {};
    unsigned long long m_selectionAnimStartMs { 0 };
    unsigned long long m_selectionAnimMs { 150 };
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionShadowBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_selectionFillBrush {};

    ClickHandler m_onClick {};
    bool m_loadingSpinnerEnabled { true };
};
