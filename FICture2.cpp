// FICture2.cpp : 애플리케이션 진입점과 FD2D 스켈레톤 예제.

#include "framework.h"
#include "FICture2.h"
#include "FD2D/FD2D.h"
#include "ImageCore/DecoderRegistry.h"
#include "ImageCore/ImageCore.h"

#include <memory>
#include <objbase.h>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <windowsx.h>
#include <wrl/client.h>
#include <cmath>
#include <shlobj.h>

#define MAX_LOADSTRING 100

WCHAR g_title[MAX_LOADSTRING];

class ThumbNavTile : public FD2D::Wnd
{
public:
    using ClickHandler = std::function<void()>;

    enum class IconKind
    {
        None,
        Folder,
        Up,
    };

    explicit ThumbNavTile(const std::wstring& name)
        : Wnd(name)
        , m_label(name + L"_label")
    {
        m_label.SetFont(L"Segoe UI Semibold", 18.0f);
        m_label.SetColor(D2D1::ColorF(D2D1::ColorF::White, 0.90f));
        m_label.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_label.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    void SetIcon(IconKind kind)
    {
        if (m_icon == kind)
        {
            return;
        }

        m_icon = kind;
        Invalidate();
    }

    void SetText(const std::wstring& text)
    {
        m_label.SetText(text);
    }

    void SetFixedSize(const FD2D::Size& size)
    {
        m_fixedSize = size;
    }

    void SetSelected(bool selected)
    {
        if (m_selected == selected)
        {
            return;
        }

        m_selected = selected;
        m_selectedStartMs = GetTickCount64();
        Invalidate();
    }

    bool Selected() const
    {
        return m_selected;
    }

    void SetOnClick(ClickHandler handler)
    {
        m_onClick = std::move(handler);
    }

    FD2D::Size Measure(FD2D::Size available) override
    {
        UNREFERENCED_PARAMETER(available);
        m_desired = { m_fixedSize.w + 2.0f * m_margin, m_fixedSize.h + 2.0f * m_margin };
        return m_desired;
    }

    void Arrange(FD2D::Rect finalRect) override
    {
        Wnd::Arrange(finalRect);
        const auto r = LayoutRect();

        if (m_icon != IconKind::None)
        {
            // Reserve bottom area for the text when an icon is shown.
            const float h = r.bottom - r.top;
            const float labelH = (std::max)(28.0f, h * 0.34f);
            m_label.SetRect(D2D1::RectF(r.left, r.bottom - labelH, r.right, r.bottom));
            m_label.SetFont(L"Segoe UI Semibold", 16.0f);
        }
        else
        {
            m_label.SetRect(r);
            m_label.SetFont(L"Segoe UI Semibold", 18.0f);
        }
    }

    bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override
    {
        UNREFERENCED_PARAMETER(wParam);

        switch (message)
        {
        case WM_MOUSEMOVE:
        {
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool prevHover = m_hovered;
            m_hovered = HitTest(pt);
            if (m_hovered != prevHover)
            {
                Invalidate();
            }
            return m_hovered;
        }
        case WM_LBUTTONDOWN:
        {
            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (HitTest(pt))
            {
                m_pressed = true;
                Invalidate();
                return true;
            }
            break;
        }
        case WM_LBUTTONUP:
        {
            bool wasPressed = m_pressed;
            m_pressed = false;

            POINT pt { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (wasPressed && HitTest(pt))
            {
                if (m_onClick)
                {
                    m_onClick();
                }
                Invalidate();
                return true;
            }
            if (wasPressed)
            {
                Invalidate();
            }
            break;
        }
        default:
            break;
        }

        return Wnd::OnMessage(message, wParam, lParam);
    }

    void OnRender(ID2D1RenderTarget* target) override
    {
        if (target == nullptr)
        {
            return;
        }

        if (!m_fillBrush)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_fillBrush);
        }
        if (!m_strokeBrush)
        {
            target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_strokeBrush);
        }

        D2D1_COLOR_F fill = D2D1::ColorF(0.18f, 0.18f, 0.18f, m_hovered ? 0.95f : 0.85f);
        if (m_pressed)
        {
            fill = D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.95f);
        }

        m_fillBrush->SetColor(fill);
        target->FillRectangle(LayoutRect(), m_fillBrush.Get());

        float strokeAlpha = 0.25f;
        float strokeThickness = 1.5f;
        D2D1_COLOR_F stroke = D2D1::ColorF(1.0f, 1.0f, 1.0f, strokeAlpha);

        if (m_selected)
        {
            // Subtle breathe: modulate alpha.
            unsigned long long nowMs = GetTickCount64();
            float t = static_cast<float>((nowMs - m_selectedStartMs) % 1800) / 1800.0f;
            float s = 0.5f + 0.5f * sinf(t * 6.2831853f);
            float a = 0.55f + (0.10f * s);
            stroke = D2D1::ColorF(1.0f, 0.60f, 0.24f, a);
            strokeThickness = 2.0f;

            if (BackplateRef() != nullptr)
            {
                BackplateRef()->RequestAnimationFrame();
            }
        }

        m_strokeBrush->SetColor(stroke);
        target->DrawRectangle(LayoutRect(), m_strokeBrush.Get(), strokeThickness);

        // Draw icon (folder / up) if requested.
        if (m_icon != IconKind::None)
        {
            if (!m_iconStrokeBrush)
            {
                target->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.65f), &m_iconStrokeBrush);
            }
            if (!m_iconAccentBrush)
            {
                target->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.95f), &m_iconAccentBrush);
            }

            const auto r = LayoutRect();
            const float w = r.right - r.left;
            const float h = r.bottom - r.top;
            const float minSide = (std::min)(w, h);

            // Icon bounds (bitmap).
            const float iconW = minSide * 0.72f;
            const float iconH = iconW; // square icon
            const float iconX = r.left + (w - iconW) * 0.5f;
            const float iconY = r.top + (h * 0.40f) - (iconH * 0.5f);

            if (EnsureFolderBitmap(target))
            {
                const D2D1_RECT_F dst = D2D1::RectF(iconX, iconY, iconX + iconW, iconY + iconH);
                // High-quality scaling for the icon.
                target->DrawBitmap(
                    m_folderBitmap.Get(),
                    dst,
                    1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    D2D1::RectF(0.0f, 0.0f, m_folderBitmap->GetSize().width, m_folderBitmap->GetSize().height));
            }

            if (m_icon == IconKind::Up)
            {
                // Simple "up" arrow over the folder.
                const float cx = iconX + iconW * 0.52f;
                const float cy = iconY + (iconH * 0.54f);
                const float shaftH = iconH * 0.20f;
                const float head = iconH * 0.12f;

                const D2D1_POINT_2F p0 = D2D1::Point2F(cx, cy + shaftH);
                const D2D1_POINT_2F p1 = D2D1::Point2F(cx, cy - shaftH * 0.10f);
                target->DrawLine(p0, p1, m_iconAccentBrush.Get(), 2.75f);

                // Arrow head
                const D2D1_POINT_2F h0 = D2D1::Point2F(cx, cy - shaftH * 0.15f);
                const D2D1_POINT_2F hl = D2D1::Point2F(cx - head, cy + head * 0.45f);
                const D2D1_POINT_2F hr = D2D1::Point2F(cx + head, cy + head * 0.45f);
                target->DrawLine(h0, hl, m_iconAccentBrush.Get(), 2.75f);
                target->DrawLine(h0, hr, m_iconAccentBrush.Get(), 2.75f);
            }
        }

        m_label.OnRender(target);

        Wnd::OnRender(target);
    }

private:
    bool EnsureFolderBitmap(ID2D1RenderTarget* target)
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

        hr = target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &m_folderBitmap);
        if (FAILED(hr) || !m_folderBitmap)
        {
            return false;
        }

        m_folderBitmapTarget = target;
        return true;
    }

    bool HitTest(const POINT& pt) const
    {
        const auto& rect = LayoutRect();
        return pt.x >= rect.left &&
            pt.x <= rect.right &&
            pt.y >= rect.top &&
            pt.y <= rect.bottom;
    }

    FD2D::Size m_fixedSize { 128.0f, 128.0f };
    FD2D::Text m_label;
    ClickHandler m_onClick {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_fillBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_strokeBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_iconStrokeBrush {};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_iconAccentBrush {};
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_folderBitmap {};
    ID2D1RenderTarget* m_folderBitmapTarget { nullptr };
    IconKind m_icon { IconKind::None };
    bool m_hovered { false };
    bool m_pressed { false };
    bool m_selected { false };
    unsigned long long m_selectedStartMs { 0 };
};

class DemoWnd : public FD2D::Wnd
{
public:
    explicit DemoWnd(const std::wstring& name) 
        : Wnd(name)
    {
        // 기존 샘플 코드 주석 처리
        /*
        // DemoWnd는 수직 StackPanel처럼 동작
        // 제목
        auto title = std::make_shared<FD2D::Text>(L"title");
        title->SetText(L"FD2D Layout Test - All Panels & Elements");
        title->SetFont(L"Segoe UI Semibold", 20.0f);
        title->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        title->SetMargin(10.0f);
        AddChild(title);

        // 상태 텍스트
        auto status = std::make_shared<FD2D::Text>(L"status");
        status->SetText(L"Status: Ready - Click buttons to test interactions");
        status->SetColor(D2D1::ColorF(D2D1::ColorF::LightGray));
        status->SetMargin(10.0f);
        AddChild(status);

        // 메인 컨테이너: DockPanel로 좌우 분할
        auto mainDock = std::make_shared<FD2D::DockPanel>(L"mainDock");
        mainDock->SetMargin(10.0f);
        AddChild(mainDock);

        // 좌측: StackPanel (수직)
        auto leftStack = std::make_shared<FD2D::StackPanel>(L"leftStack", FD2D::Orientation::Vertical);
        leftStack->SetSpacing(10.0f);
        mainDock->SetChildDock(leftStack, FD2D::Dock::Left);
        mainDock->AddChild(leftStack);

        // 좌측 StackPanel 내용
        auto leftTitle = std::make_shared<FD2D::Text>(L"leftTitle");
        leftTitle->SetText(L"StackPanel (Vertical)");
        leftTitle->SetFont(L"Segoe UI", 14.0f);
        leftTitle->SetColor(D2D1::ColorF(D2D1::ColorF::Cyan));
        leftStack->AddChild(leftTitle);

        auto btn1 = std::make_shared<FD2D::Button>(L"btn1");
        btn1->SetLabel(L"Button 1");
        btn1->SetColors(
            D2D1::ColorF(D2D1::ColorF::DarkSlateGray),
            D2D1::ColorF(D2D1::ColorF::Teal),
            D2D1::ColorF(D2D1::ColorF::CadetBlue));
        leftStack->AddChild(btn1);

        auto btn2 = std::make_shared<FD2D::Button>(L"btn2");
        btn2->SetLabel(L"Button 2");
        btn2->SetColors(
            D2D1::ColorF(D2D1::ColorF::MidnightBlue),
            D2D1::ColorF(D2D1::ColorF::SteelBlue),
            D2D1::ColorF(D2D1::ColorF::LightSteelBlue));
        leftStack->AddChild(btn2);

        auto text1 = std::make_shared<FD2D::Text>(L"text1");
        text1->SetText(L"Text Element 1");
        text1->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        leftStack->AddChild(text1);

        auto text2 = std::make_shared<FD2D::Text>(L"text2");
        text2->SetText(L"Text Element 2");
        text2->SetColor(D2D1::ColorF(D2D1::ColorF::LightGray));
        leftStack->AddChild(text2);

        // 중앙: SplitPanel (좌우 분할)
        auto centerSplit = std::make_shared<FD2D::SplitPanel>(L"centerSplit", FD2D::SplitterOrientation::Horizontal);
        centerSplit->SetSplitRatio(0.5f);
        mainDock->SetChildDock(centerSplit, FD2D::Dock::Fill);
        mainDock->AddChild(centerSplit);

        // SplitPanel 좌측: GridPanel
        auto centerGrid = std::make_shared<FD2D::GridPanel>(L"centerGrid");
        centerSplit->SetFirstChild(centerGrid);

        // GridPanel 컬럼/행 정의
        std::vector<FD2D::GridLength> cols;
        cols.push_back({FD2D::GridLength::Type::Auto, 0.0f});
        cols.push_back({FD2D::GridLength::Type::Star, 1.0f});
        cols.push_back({FD2D::GridLength::Type::Fixed, 150.0f});
        centerGrid->SetColumns(cols);

        std::vector<FD2D::GridLength> rows;
        rows.push_back({FD2D::GridLength::Type::Auto, 0.0f});
        rows.push_back({FD2D::GridLength::Type::Star, 1.0f});
        rows.push_back({FD2D::GridLength::Type::Fixed, 100.0f});
        centerGrid->SetRows(rows);

        // GridPanel 내용
        auto gridTitle = std::make_shared<FD2D::Text>(L"gridTitle");
        gridTitle->SetText(L"GridPanel");
        gridTitle->SetFont(L"Segoe UI", 14.0f);
        gridTitle->SetColor(D2D1::ColorF(D2D1::ColorF::Yellow));
        centerGrid->SetChildCell(gridTitle, 0, 0, 3, 1);
        centerGrid->AddChild(gridTitle);

        // 이미지 (Grid 중앙)
        auto image = std::make_shared<FD2D::Image>(L"image");
        image->SetSourceFile(L"assets\\kitten.jpg");
        centerGrid->SetChildCell(image, 0, 1, 3, 1);
        centerGrid->AddChild(image);

        // Grid 하단 버튼들
        auto gridBtn1 = std::make_shared<FD2D::Button>(L"gridBtn1");
        gridBtn1->SetLabel(L"Grid Btn 1");
        gridBtn1->SetColors(
            D2D1::ColorF(D2D1::ColorF::DarkGreen),
            D2D1::ColorF(D2D1::ColorF::Green),
            D2D1::ColorF(D2D1::ColorF::LightGreen));
        centerGrid->SetChildCell(gridBtn1, 0, 2, 1, 1);
        centerGrid->AddChild(gridBtn1);

        auto gridBtn2 = std::make_shared<FD2D::Button>(L"gridBtn2");
        gridBtn2->SetLabel(L"Grid Btn 2");
        gridBtn2->SetColors(
            D2D1::ColorF(D2D1::ColorF::DarkRed),
            D2D1::ColorF(D2D1::ColorF::Red),
            D2D1::ColorF(D2D1::ColorF::LightCoral));
        centerGrid->SetChildCell(gridBtn2, 1, 2, 1, 1);
        centerGrid->AddChild(gridBtn2);

        auto gridBtn3 = std::make_shared<FD2D::Button>(L"gridBtn3");
        gridBtn3->SetLabel(L"Grid Btn 3");
        gridBtn3->SetColors(
            D2D1::ColorF(D2D1::ColorF::DarkMagenta),
            D2D1::ColorF(D2D1::ColorF::Magenta),
            D2D1::ColorF(D2D1::ColorF::Plum));
        centerGrid->SetChildCell(gridBtn3, 2, 2, 1, 1);
        centerGrid->AddChild(gridBtn3);

        // SplitPanel 우측: StackPanel (수평)
        auto rightStack = std::make_shared<FD2D::StackPanel>(L"rightStack", FD2D::Orientation::Horizontal);
        rightStack->SetSpacing(5.0f);
        centerSplit->SetSecondChild(rightStack);

        auto rightTitle = std::make_shared<FD2D::Text>(L"rightTitle");
        rightTitle->SetText(L"H");
        rightTitle->SetFont(L"Segoe UI", 12.0f);
        rightTitle->SetColor(D2D1::ColorF(D2D1::ColorF::Lime));
        rightStack->AddChild(rightTitle);

        auto rightBtn1 = std::make_shared<FD2D::Button>(L"rightBtn1");
        rightBtn1->SetLabel(L"R1");
        rightBtn1->SetColors(
            D2D1::ColorF(D2D1::ColorF::DarkOrange),
            D2D1::ColorF(D2D1::ColorF::Orange),
            D2D1::ColorF(D2D1::ColorF::PeachPuff));
        rightStack->AddChild(rightBtn1);

        auto rightBtn2 = std::make_shared<FD2D::Button>(L"rightBtn2");
        rightBtn2->SetLabel(L"R2");
        rightBtn2->SetColors(
            D2D1::ColorF(D2D1::ColorF::DarkGoldenrod),
            D2D1::ColorF(D2D1::ColorF::Gold),
            D2D1::ColorF(D2D1::ColorF::Khaki));
        rightStack->AddChild(rightBtn2);

        // 하단: OverlayPanel
        auto bottomOverlay = std::make_shared<FD2D::OverlayPanel>(L"bottomOverlay");
        bottomOverlay->SetMargin(10.0f);
        AddChild(bottomOverlay);

        // OverlayPanel 배경
        auto overlayBg = std::make_shared<FD2D::Panel>(L"overlayBg");
        bottomOverlay->AddChild(overlayBg);

        // OverlayPanel 텍스트
        auto overlayText = std::make_shared<FD2D::Text>(L"overlayText");
        overlayText->SetText(L"OverlayPanel: Elements stack on top of each other");
        overlayText->SetFont(L"Segoe UI", 12.0f);
        overlayText->SetColor(D2D1::ColorF(D2D1::ColorF::White));
        overlayText->SetMargin(10.0f);
        bottomOverlay->AddChild(overlayText);

        // OverlayPanel 버튼
        auto overlayBtn = std::make_shared<FD2D::Button>(L"overlayBtn");
        overlayBtn->SetLabel(L"Overlay Button");
        overlayBtn->SetColors(
            D2D1::ColorF(D2D1::ColorF::DarkBlue),
            D2D1::ColorF(D2D1::ColorF::Blue),
            D2D1::ColorF(D2D1::ColorF::LightBlue));
        bottomOverlay->AddChild(overlayBtn);
        */

        // 메인 이미지 + 하단 썸네일 리스트 (Splitter로 분할)
        auto rootSplit = std::make_shared<FD2D::SplitPanel>(L"rootSplit", FD2D::SplitterOrientation::Vertical);
        rootSplit->SetSplitRatio(0.80f); // 위: 메인 이미지, 아래: 썸네일
        // Clamp to "thumb height + StackPanel padding*2" to avoid unnecessary vertical slack.
        constexpr float thumbStripPadding = 8.0f;
        // Include filename label height under thumbnails.
        constexpr float thumbLabelPt = 8.0f;
        constexpr float thumbLabelDip = thumbLabelPt * (96.0f / 72.0f); // DWrite font size is DIP
        constexpr float thumbLabelLineH = thumbLabelDip * 1.2f;
        constexpr float thumbItemSpacing = 2.0f;
        constexpr float thumbStripMaxH = 128.0f + thumbItemSpacing + thumbLabelLineH + (thumbStripPadding * 2.0f);
        // Ensure the filename label is never clipped by the splitter.
        rootSplit->SetSecondPaneMinExtent(thumbStripMaxH);
        rootSplit->SetSecondPaneMaxExtent(thumbStripMaxH);
        // Upward constraint는 필요 시에만 켠다. (ScrollView/overflow 케이스에서는 보통 꺼두는 게 자연스럽다)
        rootSplit->SetConstraintPropagation(FD2D::ConstraintPropagation::None);
        AddChild(rootSplit);

        auto mainImage = std::make_shared<FD2D::Image>(L"mainImage");
        std::wstring mainPath = L"D:/Works/FICture2/landscape/cavebaseground01.dds";
        mainImage->SetSourceFile(mainPath);
        mainImage->SetImagePurpose(ImageCore::ImagePurpose::FullResolution);
        {
            // Customize main-image loading spinner style (optional).
            auto spinner = mainImage->LoadingSpinner();
            if (spinner)
            {
                FD2D::Spinner::Style style {};
                style.color = D2D1::ColorF(D2D1::ColorF::White, 1.0f);
                style.thickness = 2.5f;
                style.ticks = 12;
                style.periodMs = 750;
                style.dimBackground = true;
                style.dimAlpha = 0.20f;
                spinner->SetStyle(style);
            }
        }
        
        // Load zoom speed from INI file
        {
            wchar_t iniPath[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, iniPath)))
            {
                std::wstring iniFile = std::wstring(iniPath) + L"\\FICture2\\FICture2.ini";
                wchar_t zoomSpeedStr[32];
                DWORD result = GetPrivateProfileStringW(
                    L"Image",
                    L"ZoomSpeed",
                    L"10.0",
                    zoomSpeedStr,
                    static_cast<DWORD>(std::size(zoomSpeedStr)),
                    iniFile.c_str());
                if (result > 0)
                {
                    float zoomSpeed = static_cast<float>(_wtof(zoomSpeedStr));
                    if (zoomSpeed > 0.0f && zoomSpeed <= 100.0f)
                    {
                        mainImage->SetZoomSpeed(zoomSpeed);
                    }
                }

                // Read ZoomStiffness from INI
                wchar_t zoomStiffnessStr[32];
                result = GetPrivateProfileStringW(
                    L"Image",
                    L"ZoomStiffness",
                    L"80.0",
                    zoomStiffnessStr,
                    static_cast<DWORD>(std::size(zoomStiffnessStr)),
                    iniFile.c_str());
                if (result > 0)
                {
                    float zoomStiffness = static_cast<float>(_wtof(zoomStiffnessStr));
                    if (zoomStiffness >= 10.0f && zoomStiffness <= 500.0f)
                    {
                        mainImage->SetZoomStiffness(zoomStiffness);
                    }
                }
            }
        }
        
        rootSplit->SetFirstChild(mainImage);
        m_mainImage = mainImage;

        auto thumbs = std::make_shared<FD2D::StackPanel>(L"thumbs", FD2D::Orientation::Horizontal);
        thumbs->SetSpacing(8.0f);
        thumbs->SetPadding(8.0f);
        // 썸네일 스트립은 overflow 컨테이너로 감싸서 (가로) 스크롤 가능 + upward constraint 전파 차단
        auto thumbScroll = std::make_shared<FD2D::ScrollView>(L"thumbScroll");
        thumbScroll->SetScrollStep(96.0f);
        // Pixel-unit smooth scrolling (no selection-centering logic).
        thumbScroll->SetSmoothScrollEnabled(true);
        thumbScroll->SetSmoothTimeMs(110);
        thumbScroll->SetVerticalScrollEnabled(false);
        thumbScroll->SetContent(thumbs);
        rootSplit->SetSecondChild(thumbScroll);
        m_thumbScroll = thumbScroll;

        constexpr float thumbW = 128.0f;
        constexpr float thumbH = 128.0f;
        m_thumbPanel = thumbs;
        m_thumbW = thumbW;
        m_thumbH = thumbH;
        m_thumbLabelDip = thumbLabelDip;
        m_thumbItemSpacing = thumbItemSpacing;

        m_currentFolder = std::filesystem::path(mainPath).parent_path();
        RebuildThumbList(mainPath);
    }

    FD2D::Size Measure(FD2D::Size available) override
    {
        // 윈도우 전체 크기를 사용
        m_desired = available;
        return m_desired;
    }

    void Arrange(FD2D::Rect finalRect) override
    {
        // 자식(루트 SplitPanel)에게 전체 영역을 위임
        Wnd::Arrange(finalRect);

        // First layout: ensure the selected thumb is visible without user interaction.
        if (!m_initialThumbEnsured && m_thumbScroll && m_selectedFocus)
        {
            m_thumbScroll->EnsureCentered(m_selectedFocus->LayoutRect());
            m_initialThumbEnsured = true;
        }
    }

    void OnAttached(FD2D::Backplate& backplate) override
    {
        Wnd::OnAttached(backplate);
        // 기존 샘플 이벤트 핸들러 주석 처리
    }

    void OnRender(ID2D1RenderTarget* target) override
    {
        Wnd::OnRender(target);
    }

    bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam) override
    {
        if (message == WM_FIC2_DEFERRED_ACTION)
        {
            RunDeferredAction();
            return true;
        }

        // Keyboard navigation:
        // - Act on WM_KEYDOWN for immediate response
        // - Support key-repeat, but throttle to a stable rate (so it's predictable even with fast OS repeat)
        if (message == WM_KEYDOWN)
        {
            const bool isRepeat = ((lParam & (1LL << 30)) != 0);

            if (wParam == 'N')
            {
                if (!isRepeat)
                {
                    QueueToggleNavItems();
                }
                return true;
            }

            if (wParam == VK_BACK)
            {
                if (!isRepeat)
                {
                    QueueNavigateUp();
                }
                return true;
            }

            if (wParam == VK_RETURN)
            {
                // Ignore repeats for "apply" (holding Enter should not spam applies).
                if (!isRepeat && !m_items.empty() && m_selectedIndex < m_items.size())
                {
                    ActivateSelected();
                }
                return true;
            }

            if ((wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_HOME || wParam == VK_END) && !m_items.empty())
            {
                const ULONGLONG nowMs = GetTickCount64();
                if (isRepeat)
                {
                    if ((nowMs - m_lastKeyNavMs) < kKeyRepeatMinIntervalMs)
                    {
                        return true;
                    }
                }
                m_lastKeyNavMs = nowMs;

                size_t idx = (m_selectedIndex < m_items.size()) ? m_selectedIndex : 0;

                if (wParam == VK_HOME)
                {
                    idx = 0;
                }
                else if (wParam == VK_END)
                {
                    idx = m_items.size() - 1;
                }
                else if (wParam == VK_LEFT)
                {
                    idx = (idx == 0) ? (m_items.size() - 1) : (idx - 1);
                }
                else
                {
                    idx = (idx + 1) % m_items.size();
                }

                // Keyboard navigation: debounce main image load to avoid spamming decode.
                SelectItemByIndex(idx, MainApplyMode::Debounced);
                return true;
            }
        }

        if (message == WM_TIMER && static_cast<UINT_PTR>(wParam) == kThumbApplyTimerId)
        {
            if (BackplateRef() != nullptr)
            {
                KillTimer(BackplateRef()->Window(), kThumbApplyTimerId);
            }

            if (m_hasPendingApply && m_pendingApplyIndex < m_items.size())
            {
                ApplyMainFromIndex(m_pendingApplyIndex);
            }

            m_hasPendingApply = false;
            return true;
        }

        if (message == WM_FIC2_DEFERRED_ACTION)
        {
            RunDeferredAction();
            return true;
        }

        return Wnd::OnMessage(message, wParam, lParam);
    }

private:
    static constexpr UINT WM_FIC2_DEFERRED_ACTION = WM_APP + 0x7A11;

    enum class DeferredActionKind
    {
        None,
        ToggleNavItems,
        NavigateToFolder,
        NavigateUp,
        ActivateSelected,
    };

    void QueueDeferredAction(DeferredActionKind kind, const std::filesystem::path& path = {})
    {
        m_deferredKind = kind;
        m_deferredPath = path;

        if (BackplateRef() != nullptr)
        {
            PostMessageW(BackplateRef()->Window(), WM_FIC2_DEFERRED_ACTION, 0, 0);
        }
    }

    void RunDeferredAction()
    {
        const DeferredActionKind kind = m_deferredKind;
        const std::filesystem::path path = m_deferredPath;
        m_deferredKind = DeferredActionKind::None;
        m_deferredPath.clear();

        switch (kind)
        {
        case DeferredActionKind::ToggleNavItems:
            ToggleNavItems();
            break;
        case DeferredActionKind::NavigateToFolder:
            NavigateToFolder(path);
            break;
        case DeferredActionKind::NavigateUp:
            NavigateUp();
            break;
        case DeferredActionKind::ActivateSelected:
            ActivateSelectedImpl();
            break;
        default:
            break;
        }
    }

    enum class ThumbItemKind
    {
        Up,
        Folder,
        Image,
    };

    struct ThumbItem
    {
        ThumbItemKind kind { ThumbItemKind::Image };
        std::filesystem::path path {};
        std::shared_ptr<FD2D::Wnd> focus {};
        std::shared_ptr<FD2D::Image> image {};
        std::shared_ptr<ThumbNavTile> navTile {};
    };

    enum class MainApplyMode
    {
        None,
        Debounced,
        Immediate,
    };

    void ApplyMainFromIndex(size_t index)
    {
        if (!m_mainImage || index >= m_items.size())
        {
            return;
        }

        if (m_items[index].kind != ThumbItemKind::Image)
        {
            return;
        }

        // Cancel any pending debounced apply.
        m_hasPendingApply = false;
        if (BackplateRef() != nullptr)
        {
            KillTimer(BackplateRef()->Window(), kThumbApplyTimerId);
        }

        // Real apply -> show spinner if loading takes time.
        m_mainImage->SetLoadingSpinnerEnabled(true);

        m_mainImage->SetImagePurpose(ImageCore::ImagePurpose::FullResolution);
        m_mainImage->SetSourceFile(m_items[index].path.wstring());
        m_mainImage->Invalidate();
    }

    void ScheduleApply(size_t index)
    {
        if (!m_mainImage || index >= m_items.size())
        {
            return;
        }

        if (m_items[index].kind != ThumbItemKind::Image)
        {
            return;
        }

        // Debounce window: don't show spinner (we are still browsing).
        m_mainImage->SetLoadingSpinnerEnabled(false);

        m_pendingApplyIndex = index;
        m_hasPendingApply = true;

        if (BackplateRef() != nullptr)
        {
            HWND hwnd = BackplateRef()->Window();
            KillTimer(hwnd, kThumbApplyTimerId);
            // Debounce interval (ms)
            SetTimer(hwnd, kThumbApplyTimerId, 150, nullptr);
        }
    }

    void SelectItemByIndex(size_t index, MainApplyMode mode)
    {
        if (m_items.empty())
        {
            return;
        }

        if (index >= m_items.size())
        {
            index = m_items.size() - 1;
        }

        if (m_selectedIndex < m_items.size())
        {
            if (m_items[m_selectedIndex].image)
            {
                m_items[m_selectedIndex].image->SetSelected(false);
            }
            if (m_items[m_selectedIndex].navTile)
            {
                m_items[m_selectedIndex].navTile->SetSelected(false);
            }
        }

        m_selectedIndex = index;
        m_selectedFocus = m_items[m_selectedIndex].focus;

        if (m_items[m_selectedIndex].image)
        {
            m_items[m_selectedIndex].image->SetSelected(true);
        }
        if (m_items[m_selectedIndex].navTile)
        {
            m_items[m_selectedIndex].navTile->SetSelected(true);
        }

        if (m_thumbScroll && m_selectedFocus)
        {
            m_thumbScroll->EnsureCentered(m_selectedFocus->LayoutRect());
        }

        if (m_items[m_selectedIndex].kind == ThumbItemKind::Image && mode == MainApplyMode::Immediate)
        {
            ApplyMainFromIndex(m_selectedIndex);
        }
        else if (m_items[m_selectedIndex].kind == ThumbItemKind::Image && mode == MainApplyMode::Debounced)
        {
            ScheduleApply(m_selectedIndex);
        }

        // Selection changed: on first interaction, consider initial ensure done.
        m_initialThumbEnsured = true;
    }

    void ActivateSelected()
    {
        // Activation can rebuild the UI tree; defer it to avoid mutating children during message dispatch.
        QueueDeferredAction(DeferredActionKind::ActivateSelected);
    }

    void QueueToggleNavItems()
    {
        QueueDeferredAction(DeferredActionKind::ToggleNavItems);
    }

    void ToggleNavItems()
    {
        m_showNavItems = !m_showNavItems;

        std::filesystem::path prefer {};
        if (m_selectedIndex < m_items.size())
        {
            prefer = m_items[m_selectedIndex].path;
        }

        RebuildThumbList(prefer);
    }

    void QueueNavigateUp()
    {
        QueueDeferredAction(DeferredActionKind::NavigateUp);
    }

    void NavigateUp()
    {
        if (m_currentFolder.empty())
        {
            return;
        }

        std::filesystem::path parent = m_currentFolder.parent_path();
        if (parent.empty() || parent == m_currentFolder)
        {
            return;
        }

        NavigateToFolder(parent);
    }

    void ActivateSelectedImpl()
    {
        if (m_selectedIndex >= m_items.size())
        {
            return;
        }

        const ThumbItem item = m_items[m_selectedIndex];
        if (item.kind == ThumbItemKind::Image)
        {
            ApplyMainFromIndex(m_selectedIndex);
        }
        else if (item.kind == ThumbItemKind::Up || item.kind == ThumbItemKind::Folder)
        {
            NavigateToFolder(item.path);
        }
    }

    void NavigateToFolder(const std::filesystem::path& folder)
    {
        if (folder.empty() || !std::filesystem::exists(folder) || !std::filesystem::is_directory(folder))
        {
            return;
        }

        m_currentFolder = folder;
        m_initialThumbEnsured = false;
        RebuildThumbList({});

        if (m_thumbScroll)
        {
            m_thumbScroll->SetScrollX(0.0f);
        }

        // After rebuilding, find the first image and load it into the main view.
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            if (m_items[i].kind == ThumbItemKind::Image)
            {
                ApplyMainFromIndex(i);
                break;
            }
        }
    }

    void RebuildThumbList(const std::filesystem::path& preferSelectPath)
    {
        if (!m_thumbPanel)
        {
            return;
        }

        m_thumbPanel->ClearChildren();
        m_items.clear();
        m_selectedIndex = static_cast<size_t>(-1);
        m_selectedFocus.reset();

        std::vector<std::filesystem::path> folders;
        std::vector<std::filesystem::path> files;

        if (!m_currentFolder.empty() && std::filesystem::exists(m_currentFolder))
        {
            for (auto& entry : std::filesystem::directory_iterator(m_currentFolder))
            {
                const std::filesystem::path p = entry.path();
                if (entry.is_directory())
                {
                    folders.push_back(p);
                }
                else if (entry.is_regular_file())
                {
                    if (ImageCore::DecoderRegistry::Instance().IsSupportedPath(p.wstring()))
                    {
                        files.push_back(p);
                    }
                }
            }
        }

        std::sort(folders.begin(), folders.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
        {
            return a.filename().wstring() < b.filename().wstring();
        });

        std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
        {
            return a.filename().wstring() < b.filename().wstring();
        });

        int tileId = 0;

        if (m_showNavItems)
        {
            std::filesystem::path parent = m_currentFolder.parent_path();
            if (!parent.empty() && parent != m_currentFolder)
            {
                std::wstring base = L"nav_up_" + std::to_wstring(tileId++);
                auto tile = std::make_shared<ThumbNavTile>(base + L"_tile");
                tile->SetFixedSize({ m_thumbW, m_thumbH });
                tile->SetText(L"");
                tile->SetIcon(ThumbNavTile::IconKind::Up);
                const size_t index = m_items.size();
                tile->SetOnClick([this]()
                {
                    QueueNavigateUp();
                });

                auto label = std::make_shared<FD2D::Text>(base + L"_label");
                label->SetText(L"(parent)");
                label->SetFont(L"Segoe UI", m_thumbLabelDip);
                label->SetFixedWidth(m_thumbW);
                label->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.75f, 1.0f));
                label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

                auto item = std::make_shared<FD2D::StackPanel>(base + L"_item", FD2D::Orientation::Vertical);
                item->SetSpacing(m_thumbItemSpacing);
                item->AddChild(tile);
                item->AddChild(label);

                m_thumbPanel->AddChild(item);
                m_items.push_back({ ThumbItemKind::Up, parent, tile, nullptr, tile });

                UNREFERENCED_PARAMETER(index);
            }

            for (const auto& dir : folders)
            {
                std::wstring base = L"nav_folder_" + std::to_wstring(tileId++);
                auto tile = std::make_shared<ThumbNavTile>(base + L"_tile");
                tile->SetFixedSize({ m_thumbW, m_thumbH });
                tile->SetText(L"");
                tile->SetIcon(ThumbNavTile::IconKind::Folder);

                tile->SetOnClick([this, dir]()
                {
                    QueueDeferredAction(DeferredActionKind::NavigateToFolder, dir);
                });

                auto label = std::make_shared<FD2D::Text>(base + L"_label");
                label->SetText(dir.filename().wstring());
                label->SetFont(L"Segoe UI", m_thumbLabelDip);
                label->SetFixedWidth(m_thumbW);
                label->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.75f, 1.0f));
                label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                label->SetEllipsisTrimmingEnabled(true);
                label->SetOnClick([this, dir]()
                {
                    QueueDeferredAction(DeferredActionKind::NavigateToFolder, dir);
                });

                auto item = std::make_shared<FD2D::StackPanel>(base + L"_item", FD2D::Orientation::Vertical);
                item->SetSpacing(m_thumbItemSpacing);
                item->AddChild(tile);
                item->AddChild(label);
                m_thumbPanel->AddChild(item);

                m_items.push_back({ ThumbItemKind::Folder, dir, tile, nullptr, tile });
            }
        }

        for (const auto& p : files)
        {
            std::wstring thumbName = L"thumb_" + std::to_wstring(tileId++);
            auto thumb = std::make_shared<FD2D::Image>(thumbName);
            thumb->SetThumbnailSize({ m_thumbW, m_thumbH });
            thumb->SetImagePurpose(ImageCore::ImagePurpose::Thumbnail);
            thumb->SetSourceFile(p.wstring());

            const size_t index = m_items.size();

            thumb->SetOnClick([this, index]()
            {
                SelectItemByIndex(index, MainApplyMode::Immediate);
            });

            auto item = std::make_shared<FD2D::StackPanel>(thumbName + L"_item", FD2D::Orientation::Vertical);
            item->SetSpacing(m_thumbItemSpacing);
            item->SetPadding(0.0f);

            auto label = std::make_shared<FD2D::Text>(thumbName + L"_label");
            label->SetText(p.filename().wstring());
            label->SetFont(L"Segoe UI", m_thumbLabelDip);
            label->SetFixedWidth(m_thumbW);
            label->SetColor(D2D1::ColorF(0.20f, 0.20f, 0.20f, 1.0f));
            label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            label->SetEllipsisTrimmingEnabled(true);
            label->SetOnClick([this, index]()
            {
                SelectItemByIndex(index, MainApplyMode::Immediate);
            });

            item->AddChild(thumb);
            item->AddChild(label);
            m_thumbPanel->AddChild(item);

            m_items.push_back({ ThumbItemKind::Image, p, thumb, thumb, nullptr });
        }

        // Restore selection:
        size_t selectIndex = 0;
        if (!preferSelectPath.empty())
        {
            for (size_t i = 0; i < m_items.size(); ++i)
            {
                if (m_items[i].path == preferSelectPath)
                {
                    selectIndex = i;
                    break;
                }
            }
        }

        if (!m_items.empty())
        {
            SelectItemByIndex(selectIndex, MainApplyMode::None);
        }

        // Request layout recalculation and repaint.
        if (BackplateRef() != nullptr)
        {
            BackplateRef()->RequestLayout();
            HWND hwnd = BackplateRef()->Window();
            if (hwnd != nullptr)
            {
                // Force immediate repaint with layout recalculation.
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
        }
    }

    static constexpr UINT_PTR kThumbApplyTimerId = 0x4D21;
    static constexpr ULONGLONG kKeyRepeatMinIntervalMs = 60;

    std::shared_ptr<FD2D::Image> m_mainImage {};
    std::shared_ptr<FD2D::Wnd> m_selectedFocus {};
    std::shared_ptr<FD2D::ScrollView> m_thumbScroll {};
    std::shared_ptr<FD2D::StackPanel> m_thumbPanel {};
    std::vector<ThumbItem> m_items {};
    size_t m_selectedIndex { static_cast<size_t>(-1) };
    bool m_initialThumbEnsured { false };
    ULONGLONG m_lastKeyNavMs { 0 };

    bool m_hasPendingApply { false };
    size_t m_pendingApplyIndex { static_cast<size_t>(-1) };

    std::filesystem::path m_currentFolder {};
    bool m_showNavItems { true };
    float m_thumbW { 128.0f };
    float m_thumbH { 128.0f };
    float m_thumbLabelDip { 10.0f };
    float m_thumbItemSpacing { 2.0f };

    DeferredActionKind m_deferredKind { DeferredActionKind::None };
    std::filesystem::path m_deferredPath {};
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    std::wstring cmdLine = (lpCmdLine != nullptr) ? lpCmdLine : L"";
    std::wstring cmdLower = cmdLine;
    std::transform(cmdLower.begin(), cmdLower.end(), cmdLower.begin(), [](wchar_t c)
    {
        return static_cast<wchar_t>(towlower(c));
    });

    // COM lifetime is owned by the application (not FD2D)
    bool coInitialized = false;
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(coHr))
    {
        coInitialized = true;
    }
    else if (coHr == RPC_E_CHANGED_MODE)
    {
        // Another component initialized COM with a different model; continue without owning lifetime
        coInitialized = false;
    }
    else
    {
        return -1;
    }

    auto& app = FD2D::Application::Instance();

    // Register built-in image decoders before any folder scan or decode request
    ImageCore::RegisterBuiltInDecoders();

    FD2D::InitContext initContext {};
    initContext.instance = hInstance;
    if (FAILED(app.Initialize(initContext)))
    {
        return -1;
    }

    // Log detected Direct2D version at startup
    {
        const char* d2dVersionStr = FD2D::Core::GetD2DVersionString();
        FD2D::D2DVersion d2dVersion = FD2D::Core::GetSupportedD2DVersion();
        wchar_t dbgMsg[256];
        swprintf_s(dbgMsg, L"[FICture2] Direct2D Version: %S (enum value: %d)\n", 
            d2dVersionStr, static_cast<int>(d2dVersion));
        OutputDebugStringW(dbgMsg);
    }

    LoadStringW(hInstance, IDS_APP_TITLE, g_title, MAX_LOADSTRING);

    FD2D::WindowOptions opts {};
    opts.title = g_title;
    opts.chrome = FD2D::ChromeStyle::Standard;
    opts.instance = hInstance;
    // User override:
    //   --renderer=d2d  : force D2D-only renderer (more compatible, no D3D pass)
    //   --renderer=d3d  : prefer D3D11 swapchain renderer (default, fastest)
    if (cmdLower.find(L"--renderer=d2d") != std::wstring::npos)
    {
        opts.rendererId = L"d2d_hwndrt";
    }
    else if (cmdLower.find(L"--renderer=d3d") != std::wstring::npos)
    {
        opts.rendererId = L"d3d11_swapchain";
    }

    int result = -1;
    {
        // IMPORTANT:
        // Ensure all UI objects (Backplate/Wnd tree) are destroyed BEFORE CoUninitialize().
        // Some COM-backed objects (e.g. WIC bitmaps created for thumbnails) must be released
        // before the calling thread uninitializes COM.
        auto backplate = app.CreateWindowedBackplate(L"main", opts);
        if (!backplate)
        {
            app.Shutdown();
            return FALSE;
        }

        backplate->AddWnd(std::make_shared<DemoWnd>(L"demo"));

        backplate->Show(nCmdShow);

        result = app.RunMessageLoop();
        // backplate is destroyed here (end of scope), releasing any COM resources it owns.
    }

    app.Shutdown();

    if (coInitialized)
    {
        CoUninitialize();
    }
    return result;
}
