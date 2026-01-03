// FICture2.cpp : 애플리케이션 진입점과 FD2D 스켈레톤 예제.

#include "framework.h"
#include "FICture2.h"
#include "FD2D/FD2D.h"

#include <memory>

#define MAX_LOADSTRING 100

WCHAR g_title[MAX_LOADSTRING];

class DemoWnd : public FD2D::Wnd
{
public:
    explicit DemoWnd(const std::wstring& name) 
        : Wnd(name)
    {
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
    }

    FD2D::Size Measure(FD2D::Size available) override
    {
        // 수직으로 자식들을 배치
        float totalHeight = 0.0f;
        float maxWidth = 0.0f;

        for (auto& pair : Children())
        {
            if (pair.second)
            {
                FD2D::Size childSize = pair.second->Measure(available);
                totalHeight += childSize.h;
                maxWidth = (std::max)(maxWidth, childSize.w);
            }
        }

        m_desired = { maxWidth, totalHeight };
        return m_desired;
    }

    void Arrange(FD2D::Rect finalRect) override
    {
        FD2D::Rect inset = FD2D::Inset(finalRect, m_margin);
        FD2D::Rect childArea = FD2D::Inset(inset, m_padding);
        
        float yOffset = childArea.y;
        float remainingHeight = childArea.h;

        // title, status, mainDock, bottomOverlay 순서로 배치
        std::vector<std::wstring> childOrder = { L"title", L"status", L"mainDock", L"bottomOverlay" };

        for (const auto& childName : childOrder)
        {
            auto it = Children().find(childName);
            if (it != Children().end() && it->second)
            {
                auto& child = it->second;
                
                if (childName == L"mainDock")
                {
                    // mainDock은 남은 공간을 모두 차지
                    float mainDockHeight = remainingHeight;
                    if (Children().find(L"bottomOverlay") != Children().end())
                    {
                        // bottomOverlay가 있으면 그 크기를 빼야 함
                        auto bottomOverlay = Children().at(L"bottomOverlay");
                        if (bottomOverlay)
                        {
                            FD2D::Size overlaySize = bottomOverlay->Measure({ childArea.w, remainingHeight });
                            mainDockHeight -= overlaySize.h;
                        }
                    }
                    
                    FD2D::Rect mainDockRect { childArea.x, yOffset, childArea.w, mainDockHeight };
                    child->Arrange(mainDockRect);
                    yOffset += mainDockHeight;
                    remainingHeight -= mainDockHeight;
                }
                else
                {
                    // 다른 자식들은 desired size 사용
                    FD2D::Size desired = child->Measure({ childArea.w, remainingHeight });
                    FD2D::Rect childRect { childArea.x, yOffset, childArea.w, desired.h };
                    child->Arrange(childRect);
                    yOffset += desired.h;
                    remainingHeight -= desired.h;
                }
            }
        }

        m_bounds = finalRect;
        m_layoutRect = FD2D::ToD2D(finalRect);
    }

    void OnAttached(FD2D::Backplate& backplate) override
    {
        Wnd::OnAttached(backplate);

        // 버튼 클릭 핸들러 설정 - Children() 맵에서 찾아서 사용
        auto status = std::dynamic_pointer_cast<FD2D::Text>(Children().at(L"status"));
        if (status)
        {
            // leftStack의 자식인 btn1, btn2 찾기
            auto mainDock = Children().at(L"mainDock");
            if (mainDock)
            {
                auto leftStack = mainDock->Children().at(L"leftStack");
                if (leftStack)
                {
                    auto btn1 = std::dynamic_pointer_cast<FD2D::Button>(leftStack->Children().at(L"btn1"));
                    if (btn1)
                    {
                        btn1->OnClick([status]()
                        {
                            status->SetText(L"Status: Left Stack Button 1 clicked!");
                            status->SetColor(D2D1::ColorF(D2D1::ColorF::Yellow));
                            status->Invalidate();
                        });
                    }

                    auto btn2 = std::dynamic_pointer_cast<FD2D::Button>(leftStack->Children().at(L"btn2"));
                    if (btn2)
                    {
                        btn2->OnClick([status]()
                        {
                            status->SetText(L"Status: Left Stack Button 2 clicked!");
                            status->SetColor(D2D1::ColorF(D2D1::ColorF::Cyan));
                            status->Invalidate();
                        });
                    }
                }
            }
        }
    }

    void OnRender(ID2D1RenderTarget* target) override
    {
        if (target != nullptr)
        {
            target->Clear(D2D1::ColorF(D2D1::ColorF::DarkSlateGray, 1.0f));
        }

        Wnd::OnRender(target);
    }
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    auto& app = FD2D::Application::Instance();

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
    return result;
}
