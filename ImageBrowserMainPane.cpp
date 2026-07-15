#include "ImageBrowserMainPane.h"
#include "FD2D/Util.h"

#include "framework.h"

#include <wrl/client.h>
#include <vector>

class InfoBar : public FD2D::Wnd
{
public:
    InfoBar()
        : Wnd(L"infoBar")
    {
        SetPadding(6.0f);

        m_host = std::make_shared<FD2D::DockPanel>(L"infoDock");

        m_leftText = std::make_shared<FD2D::Text>(L"infoLeft");
        m_leftText->SetFont(L"Segoe UI", 11.0f);
        m_leftText->SetColor(D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f));
        m_leftText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_leftText->SetEllipsisTrimmingEnabled(true);
        m_leftText->SetTooltipOnTruncation(true);
        m_leftText->SetCopyTextOnRightClick(true);

        m_rightText = std::make_shared<FD2D::Text>(L"infoRight");
        m_rightText->SetFont(L"Segoe UI", 11.0f);
        m_rightText->SetColor(D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f));
        m_rightText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_rightText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        // Wide enough for "100% 270°" (zoom% plus an optional rotation angle).
        m_rightText->SetFixedWidth(96.0f);

        // DockPanel: Fill stops subsequent docking, so add Right first, then Fill.
        m_host->AddChild(m_rightText);
        m_host->SetChildDock(m_rightText, FD2D::Dock::Right);
        m_host->AddChild(m_leftText);
        m_host->SetChildDock(m_leftText, FD2D::Dock::Fill);

        AddChild(m_host);
    }

    void SetLeftValue(const std::wstring& v)
    {
        if (m_leftText)
        {
            m_leftText->SetText(v);
        }
    }

    void SetRightValue(const std::wstring& v)
    {
        if (m_rightText)
        {
            m_rightText->SetText(v);
        }
    }

    FD2D::Size Measure(FD2D::Size available) override
    {
        // Must account for our own padding (top/bottom) + text line height to avoid clipping.
        // Text::Measure uses (fontSize * 1.2) as line height.
        const float fontSize = 12.0f;
        const float lineH = fontSize * 1.2f;
        const float h = (lineH + (2.0f * m_padding) + 2.0f);
        const float w = (available.w > 0.0f) ? available.w : 0.0f;
        m_desired = { w, h };
        return m_desired;
    }

    void OnRender(ID2D1RenderTarget* target) override
    {
        UNREFERENCED_PARAMETER(target);
        Wnd::OnRender(target);
    }

private:
    std::shared_ptr<FD2D::DockPanel> m_host {};
    std::shared_ptr<FD2D::Text> m_leftText {};
    std::shared_ptr<FD2D::Text> m_rightText {};
};

class PathBar : public FD2D::Wnd
{
public:
    PathBar()
        : Wnd(L"pathBar")
    {
        SetPadding(6.0f);
        m_text = std::make_shared<FD2D::Text>(L"pathText");
        m_text->SetFont(L"Segoe UI", 11.0f);
        m_text->SetColor(D2D1::ColorF(0.90f, 0.90f, 0.90f, 1.0f));
        m_text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_text->SetEllipsisTrimmingEnabled(true);
        m_text->SetCopyTextOnRightClick(true);
        AddChild(m_text);
    }

    void SetRawValue(const std::wstring& v)
    {
        if (v == m_rawValue)
        {
            return;
        }

        m_rawValue = v;
        UpdateFittedText();
    }

    FD2D::Size Measure(FD2D::Size available) override
    {
        const float fontSize = 12.0f;
        const float lineH = fontSize * 1.2f;
        const float h = (lineH + (2.0f * m_padding) + 2.0f);
        const float w = (available.w > 0.0f) ? available.w : 0.0f;
        m_desired = { w, h };
        return m_desired;
    }

    void Arrange(FD2D::Rect finalRect) override
    {
        Wnd::Arrange(finalRect);
        UpdateFittedText();
    }

    void OnRender(ID2D1RenderTarget* target) override
    {
        UNREFERENCED_PARAMETER(target);
        Wnd::OnRender(target);
    }

private:
    void EnsureFormat()
    {
        if (m_format)
        {
            return;
        }

        IDWriteFactory* factory = FD2D::Core::DWriteFactory();
        if (!factory)
        {
            return;
        }

        (void)factory->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            11.0f,
            L"",
            &m_format);

        if (m_format)
        {
            (void)m_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    bool FitsWidth(const std::wstring& s, float maxWidth)
    {
        EnsureFormat();
        if (!m_format)
        {
            return true;
        }

        IDWriteFactory* factory = FD2D::Core::DWriteFactory();
        if (!factory)
        {
            return true;
        }

        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout {};
        (void)factory->CreateTextLayout(
            s.c_str(),
            static_cast<UINT32>(s.size()),
            m_format.Get(),
            100000.0f,
            1000.0f,
            &layout);

        if (!layout)
        {
            return true;
        }

        DWRITE_TEXT_METRICS metrics {};
        (void)layout->GetMetrics(&metrics);
        return metrics.widthIncludingTrailingWhitespace <= maxWidth;
    }

    static std::wstring NormalizeSeparators(std::wstring s)
    {
        for (auto& ch : s)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
        }
        return s;
    }

    static void SplitPathRemainder(const std::wstring& s, std::vector<std::wstring>& outParts)
    {
        outParts.clear();

        size_t start = 0;
        while (start < s.size())
        {
            size_t sep = s.find(L'\\', start);
            if (sep == std::wstring::npos)
            {
                sep = s.size();
            }

            if (sep > start)
            {
                outParts.push_back(s.substr(start, sep - start));
            }

            start = sep + 1;
        }
    }

    static void EnsureTrailingSlash(std::wstring& s)
    {
        if (!s.empty() && s.back() != L'\\')
        {
            s.push_back(L'\\');
        }
    }

    static std::wstring JoinTail(const std::vector<std::wstring>& parts, size_t tailCount)
    {
        if (tailCount == 0 || parts.empty())
        {
            return L"";
        }

        const size_t start = (parts.size() > tailCount) ? (parts.size() - tailCount) : 0;

        std::wstring out;
        for (size_t i = start; i < parts.size(); ++i)
        {
            if (!out.empty())
            {
                out.push_back(L'\\');
            }
            out += parts[i];
        }
        return out;
    }

    std::wstring FolderEllipsize(const std::wstring& raw, float maxWidth)
    {
        if (raw.empty())
        {
            return raw;
        }

        const std::wstring s = NormalizeSeparators(raw);

        // If it already fits, keep as-is.
        if (FitsWidth(s, maxWidth))
        {
            return s;
        }

        std::wstring prefix;
        std::wstring remainder;

        // UNC path: \\server\share\...
        if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\')
        {
            size_t p = 2;
            const size_t serverEnd = s.find(L'\\', p);
            if (serverEnd == std::wstring::npos)
            {
                return s;
            }
            const std::wstring server = s.substr(p, serverEnd - p);

            p = serverEnd + 1;
            const size_t shareEnd = s.find(L'\\', p);
            if (shareEnd == std::wstring::npos)
            {
                return s;
            }
            const std::wstring share = s.substr(p, shareEnd - p);

            prefix = L"\\\\";
            prefix += server;
            prefix.push_back(L'\\');
            prefix += share;
            remainder = s.substr(shareEnd + 1);
        }
        // Drive path: C:\...
        else if (s.size() >= 2 && s[1] == L':')
        {
            if (s.size() >= 3 && s[2] == L'\\')
            {
                prefix = s.substr(0, 3); // "C:\"
                remainder = s.substr(3);
            }
            else
            {
                prefix = s.substr(0, 2); // "C:"
                remainder = s.substr(2);
                EnsureTrailingSlash(prefix);
                if (!remainder.empty() && remainder.front() == L'\\')
                {
                    remainder.erase(remainder.begin());
                }
            }
        }
        else
        {
            remainder = s;
        }

        std::vector<std::wstring> parts;
        SplitPathRemainder(remainder, parts);
        if (parts.empty())
        {
            return s;
        }

        // If even the filename doesn't fit, just return the filename and let Text do char-level ellipsis.
        const std::wstring fileOnly = parts.back();
        if (!FitsWidth(fileOnly, maxWidth))
        {
            return fileOnly;
        }

        std::wstring base = prefix;
        EnsureTrailingSlash(base);

        // Try keeping as many tail folders as possible: prefix + "...\\" + last N parts
        for (size_t tailCount = (parts.size() >= 2) ? (parts.size() - 1) : 1; tailCount >= 1; --tailCount)
        {
            std::wstring candidate = base;
            candidate += L"...\\";
            candidate += JoinTail(parts, tailCount);

            if (FitsWidth(candidate, maxWidth))
            {
                return candidate;
            }

            if (tailCount == 1)
            {
                break;
            }
        }

        // Fallback: prefix + "...\\" + filename
        std::wstring fallback = base;
        fallback += L"...\\";
        fallback += parts.back();
        return fallback;
    }

    void UpdateFittedText()
    {
        if (!m_text)
        {
            return;
        }

        const D2D1_RECT_F r = LayoutRect();
        float maxWidth = (r.right - r.left) - (2.0f * m_padding);
        if (maxWidth < 8.0f)
        {
            maxWidth = 8.0f;
        }

        const std::wstring fitted = FolderEllipsize(m_rawValue, maxWidth);
        if (fitted != m_lastFittedValue)
        {
            m_lastFittedValue = fitted;
            m_text->SetText(fitted);
        }
        // Full path for hover tooltip + clipboard, even when the displayed
        // label is middle-ellipsized by FolderEllipsize (not DWrite trimming).
        m_text->SetTooltipText(m_rawValue);
        m_text->SetCopyText(m_rawValue);
    }

    std::shared_ptr<FD2D::Text> m_text {};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format {};
    std::wstring m_rawValue {};
    std::wstring m_lastFittedValue {};
};

ImageBrowserMainPane::ImageBrowserMainPane()
    : FD2D::Wnd(L"mainPane")
{
}

void ImageBrowserMainPane::Build(
    const std::shared_ptr<FD2D::SplitPanel>& rootSplit,
    const std::wstring& initialFile,
    const std::function<void(const ImageViewTransform&)>& onViewChanged,
    const std::function<void()>& onClick,
    const std::function<void(ImageBrowserMainImage&)>& applyIni,
    const std::function<bool(const POINT&)>& onContextMenuRequest,
    const std::function<void()>& onMainImageWheelFocus)
{
    if (!rootSplit)
    {
        return;
    }

    m_onContextMenuRequest = onContextMenuRequest;
    m_onMainImageWheelFocus = onMainImageWheelFocus;
    m_mainDock.reset();
    m_mainImage.reset();

    auto mainImage = std::make_shared<ImageBrowserMainImage>(L"mainImage0");
    if (!initialFile.empty())
    {
        mainImage->SetSourceFile(initialFile);
    }
    if (onViewChanged)
    {
        mainImage->SetOnViewChanged(onViewChanged);
    }
    if (onClick)
    {
        mainImage->SetOnClick(onClick);
    }
    if (applyIni)
    {
        applyIni(*mainImage);
    }
    m_mainImage = mainImage;

    if (!m_infoBar)
    {
        m_infoBar = std::make_shared<InfoBar>();
    }
    if (!m_pathBar)
    {
        m_pathBar = std::make_shared<PathBar>();
    }

    // Main area layout: DockPanel where bottom is info bar and fill is main image/grid.
    auto dock = std::make_shared<FD2D::DockPanel>(L"mainDock");
    dock->AddChild(m_pathBar);
    dock->SetChildDock(m_pathBar, FD2D::Dock::Top);
    dock->AddChild(m_infoBar);
    dock->SetChildDock(m_infoBar, FD2D::Dock::Bottom);
    dock->AddChild(m_mainImage);
    dock->SetChildDock(m_mainImage, FD2D::Dock::Fill);
    m_mainDock = dock;
    ClearChildren();
    AddChild(dock);
    rootSplit->SetFirstChild(shared_from_this());
}

void ImageBrowserMainPane::UpdateInfo(
    const std::wstring& pathText,
    const std::wstring& infoText,
    const std::wstring& zoomText)
{
    if (m_pathBar)
    {
        if (pathText != m_lastPathText)
        {
            m_lastPathText = pathText;
            m_pathBar->SetRawValue(pathText);
        }
    }

    if (m_infoBar)
    {
        if (infoText != m_lastInfoText)
        {
            m_lastInfoText = infoText;
            m_infoBar->SetLeftValue(infoText);
        }
        if (zoomText != m_lastZoomText)
        {
            m_lastZoomText = zoomText;
            m_infoBar->SetRightValue(zoomText);
        }
    }
}

void ImageBrowserMainPane::ResetInfoCache()
{
    m_lastPathText.clear();
    m_lastInfoText.clear();
    m_lastZoomText.clear();
}

bool ImageBrowserMainPane::TryGetMainImageRect(D2D1_RECT_F& outRect) const
{
    if (m_mainImage == nullptr)
    {
        return false;
    }

    outRect = m_mainImage->LayoutRect();
    return outRect.right > outRect.left && outRect.bottom > outRect.top;
}

void ImageBrowserMainPane::PauseMainImageViewAnimation()
{
    if (m_mainImage == nullptr)
    {
        return;
    }

    auto vt = m_mainImage->GetViewTransform();
    vt.targetZoomScale = vt.zoomScale;
    vt.zoomVelocity = 0.0f;
    m_mainImage->SetViewTransform(vt, false /*notify*/);
}

void ImageBrowserMainPane::RenderCenteredMainOverlayBitmap(
    ID2D1RenderTarget* target,
    ID2D1Bitmap* bitmap,
    float sizeFactor)
{
    if (target == nullptr || bitmap == nullptr)
    {
        return;
    }

    D2D1_RECT_F mainRect {};
    if (!TryGetMainImageRect(mainRect))
    {
        return;
    }

    const float w = mainRect.right - mainRect.left;
    const float h = mainRect.bottom - mainRect.top;
    if (w <= 0.0f || h <= 0.0f)
    {
        return;
    }

    const float clampedFactor = (std::max)(0.01f, (std::min)(1.0f, sizeFactor));
    const float iconSize = (std::min)(w, h) * clampedFactor;
    const float iconX = mainRect.left + (w - iconSize) * 0.5f;
    const float iconY = mainRect.top + (h - iconSize) * 0.5f;

    const D2D1_RECT_F dst = D2D1::RectF(iconX, iconY, iconX + iconSize, iconY + iconSize);
    target->DrawBitmap(
        bitmap,
        dst,
        1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        D2D1::RectF(0.0f, 0.0f, bitmap->GetSize().width, bitmap->GetSize().height));
}

void ImageBrowserMainPane::RenderOnMainRect(const std::function<void(const D2D1_RECT_F&)>& renderer)
{
    if (!renderer)
    {
        return;
    }

    D2D1_RECT_F mainRect {};
    if (!TryGetMainImageRect(mainRect))
    {
        return;
    }

    renderer(mainRect);
}

bool ImageBrowserMainPane::OnInputEvent(const FD2D::InputEvent& event)
{
    D2D1_RECT_F mainRect {};
    const bool pointInMain = event.hasPoint &&
        TryGetMainImageRect(mainRect) &&
        FD2D::Util::RectContainsPoint(mainRect, event.point);
    if (pointInMain)
    {
        if (event.type == FD2D::InputEventType::MouseUp &&
            event.button == FD2D::MouseButton::Right &&
            m_onContextMenuRequest)
        {
            return m_onContextMenuRequest(event.point);
        }

        if (event.type == FD2D::InputEventType::MouseWheel && m_onMainImageWheelFocus)
        {
            m_onMainImageWheelFocus();
        }
    }

    return Wnd::OnInputEvent(event);
}
