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

#define MAX_LOADSTRING 100

WCHAR g_title[MAX_LOADSTRING];

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
        rootSplit->SetFirstChild(mainImage);
        m_mainImage = mainImage;

        auto thumbs = std::make_shared<FD2D::StackPanel>(L"thumbs", FD2D::Orientation::Horizontal);
        thumbs->SetSpacing(8.0f);
        thumbs->SetPadding(8.0f);
        // 썸네일 스트립은 overflow 컨테이너로 감싸서 (가로) 스크롤 가능 + upward constraint 전파 차단
        auto thumbScroll = std::make_shared<FD2D::ScrollView>(L"thumbScroll");
        thumbScroll->SetScrollStep(96.0f);
        thumbScroll->SetVerticalScrollEnabled(false);
        thumbScroll->SetContent(thumbs);
        rootSplit->SetSecondChild(thumbScroll);
        m_thumbScroll = thumbScroll;

        // 메인 이미지 폴더의 모든 이미지 파일을 썸네일로 표시
        std::filesystem::path folder = std::filesystem::path(mainPath).parent_path();
        std::vector<std::filesystem::path> files;

        if (!folder.empty() && std::filesystem::exists(folder))
        {
            for (auto& entry : std::filesystem::directory_iterator(folder))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }

                const std::filesystem::path p = entry.path();
                if (!ImageCore::DecoderRegistry::Instance().IsSupportedPath(p.wstring()))
                {
                    continue;
                }

                files.push_back(p);
            }
        }

        std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
        {
            return a.filename().wstring() < b.filename().wstring();
        });

        constexpr float thumbW = 128.0f;
        constexpr float thumbH = 128.0f;

        int i = 0;
        size_t foundMainIndex = static_cast<size_t>(-1);
        for (const auto& p : files)
        {
            std::wstring thumbName = L"thumb_" + std::to_wstring(i++);
            auto thumb = std::make_shared<FD2D::Image>(thumbName);
            thumb->SetThumbnailSize({ thumbW, thumbH });
            thumb->SetImagePurpose(ImageCore::ImagePurpose::Thumbnail);
            thumb->SetSourceFile(p.wstring());
            const size_t index = m_thumbs.size();
            m_thumbs.push_back({ p, thumb });

            if (p.wstring() == mainPath)
            {
                foundMainIndex = index;
            }

            thumb->SetOnClick([this, index]()
            {
                SelectThumbByIndex(index, MainApplyMode::Immediate);
            });

            // Thumbnail item: image + filename label
            auto item = std::make_shared<FD2D::StackPanel>(thumbName + L"_item", FD2D::Orientation::Vertical);
            item->SetSpacing(thumbItemSpacing);
            item->SetPadding(0.0f);

            auto label = std::make_shared<FD2D::Text>(thumbName + L"_label");
            label->SetText(p.filename().wstring());
            label->SetFont(L"Segoe UI", thumbLabelDip);
            label->SetFixedWidth(thumbW);
            label->SetColor(D2D1::ColorF(0.20f, 0.20f, 0.20f, 1.0f));
            label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            label->SetEllipsisTrimmingEnabled(true);
            label->SetOnClick([this, index]()
            {
                SelectThumbByIndex(index, MainApplyMode::Immediate);
            });

            item->AddChild(thumb);
            item->AddChild(label);
            thumbs->AddChild(item);
        }

        // Initial selection should match main image when possible.
        if (!m_thumbs.empty())
        {
            if (foundMainIndex != static_cast<size_t>(-1))
            {
                SelectThumbByIndex(foundMainIndex, MainApplyMode::None);
            }
            else
            {
                SelectThumbByIndex(0, MainApplyMode::None);
            }
        }
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
        if (!m_initialThumbEnsured && m_thumbScroll && m_selectedThumb)
        {
            m_thumbScroll->EnsureVisible(m_selectedThumb->LayoutRect(), 8.0f);
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
        // Keyboard navigation:
        // - Act on WM_KEYDOWN for immediate response
        // - Support key-repeat, but throttle to a stable rate (so it's predictable even with fast OS repeat)
        if (message == WM_KEYDOWN)
        {
            const bool isRepeat = ((lParam & (1LL << 30)) != 0);

            if (wParam == VK_RETURN)
            {
                // Ignore repeats for "apply" (holding Enter should not spam applies).
                if (!isRepeat && !m_thumbs.empty() && m_selectedIndex < m_thumbs.size())
                {
                    ApplyMainFromIndex(m_selectedIndex);
                }
                return true;
            }

            if ((wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_HOME || wParam == VK_END) && !m_thumbs.empty())
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

                size_t idx = (m_selectedIndex < m_thumbs.size()) ? m_selectedIndex : 0;

                if (wParam == VK_HOME)
                {
                    idx = 0;
                }
                else if (wParam == VK_END)
                {
                    idx = m_thumbs.size() - 1;
                }
                else if (wParam == VK_LEFT)
                {
                    idx = (idx == 0) ? (m_thumbs.size() - 1) : (idx - 1);
                }
                else
                {
                    idx = (idx + 1) % m_thumbs.size();
                }

                // Keyboard navigation: debounce main image load to avoid spamming decode.
                SelectThumbByIndex(idx, MainApplyMode::Debounced);
                return true;
            }
        }

        if (message == WM_TIMER && static_cast<UINT_PTR>(wParam) == kThumbApplyTimerId)
        {
            if (BackplateRef() != nullptr)
            {
                KillTimer(BackplateRef()->Window(), kThumbApplyTimerId);
            }

            if (m_hasPendingApply && m_pendingApplyIndex < m_thumbs.size())
            {
                ApplyMainFromIndex(m_pendingApplyIndex);
            }

            m_hasPendingApply = false;
            return true;
        }

        return Wnd::OnMessage(message, wParam, lParam);
    }

private:
    struct ThumbEntry
    {
        std::filesystem::path path {};
        std::shared_ptr<FD2D::Image> image {};
    };

    enum class MainApplyMode
    {
        None,
        Debounced,
        Immediate,
    };

    void ApplyMainFromIndex(size_t index)
    {
        if (!m_mainImage || index >= m_thumbs.size())
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
        m_mainImage->SetSourceFile(m_thumbs[index].path.wstring());
        m_mainImage->Invalidate();
    }

    void ScheduleApply(size_t index)
    {
        if (!m_mainImage || index >= m_thumbs.size())
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

    void SelectThumbByIndex(size_t index, MainApplyMode mode)
    {
        if (m_thumbs.empty())
        {
            return;
        }

        if (index >= m_thumbs.size())
        {
            index = m_thumbs.size() - 1;
        }

        if (m_selectedThumb)
        {
            m_selectedThumb->SetSelected(false);
        }

        m_selectedIndex = index;
        m_selectedThumb = m_thumbs[m_selectedIndex].image;

        if (m_selectedThumb)
        {
            m_selectedThumb->SetSelected(true);
        }

        if (m_thumbScroll && m_selectedThumb)
        {
            m_thumbScroll->EnsureVisible(m_selectedThumb->LayoutRect(), 8.0f);
        }

        if (mode == MainApplyMode::Immediate)
        {
            ApplyMainFromIndex(m_selectedIndex);
        }
        else if (mode == MainApplyMode::Debounced)
        {
            ScheduleApply(m_selectedIndex);
        }

        // Selection changed: on first interaction, consider initial ensure done.
        m_initialThumbEnsured = true;
    }

    static constexpr UINT_PTR kThumbApplyTimerId = 0x4D21;
    static constexpr ULONGLONG kKeyRepeatMinIntervalMs = 60;

    std::shared_ptr<FD2D::Image> m_mainImage {};
    std::shared_ptr<FD2D::Image> m_selectedThumb {};
    std::shared_ptr<FD2D::ScrollView> m_thumbScroll {};
    std::vector<ThumbEntry> m_thumbs {};
    size_t m_selectedIndex { static_cast<size_t>(-1) };
    bool m_initialThumbEnsured { false };
    ULONGLONG m_lastKeyNavMs { 0 };

    bool m_hasPendingApply { false };
    size_t m_pendingApplyIndex { static_cast<size_t>(-1) };
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

    auto backplate = app.CreateWindowedBackplate(L"main", opts);
    if (!backplate)
    {
        app.Shutdown();
        return FALSE;
    }

    backplate->AddWnd(std::make_shared<DemoWnd>(L"demo"));

    backplate->Show(nCmdShow);

    int result = app.RunMessageLoop();
    app.Shutdown();

    if (coInitialized)
    {
        CoUninitialize();
    }
    return result;
}
